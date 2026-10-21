# SolariNet dashboard — intranet deployment

Deployment instructions for the dashboard tier (PHP API + SPA) behind HTTPS on a
single intranet host. Production uses the reviewed uid split in
`deploy/dashboard/hardened/`: `solariServer` runs as `solari`, PHP-FPM runs as
`www-solari`, and privileged control requests pass through the peer-credential
ACL and server-owned queue. Secrets are **never** committed; provision them in
root-owned configuration files on the target host.

Stack: **MariaDB** ← **solariServer** (SCP + `solariCtl` bridge) ← **php-fpm**
pool (API) ← **Apache** vhost (HTTPS, serves the SPA and proxies `/api/`).

## 1. Database
Create separate service and dashboard accounts. The dashboard account must not
have a grant on `ctlRequest`; PHP submits privileged work through the Unix socket.
Use the per-table grant generator and verification query in
[`hardened/PERMISSIONS.md`](hardened/PERMISSIONS.md#db-grants).

```sql
CREATE DATABASE IF NOT EXISTS solarinet CHARACTER SET utf8mb4 COLLATE utf8mb4_unicode_ci;
CREATE USER IF NOT EXISTS 'solari'@'127.0.0.1' IDENTIFIED BY 'CHANGE_ME_SERVER';
GRANT SELECT,INSERT,UPDATE,DELETE ON solarinet.* TO 'solari'@'127.0.0.1';
CREATE USER IF NOT EXISTS 'solari_dash'@'127.0.0.1' IDENTIFIED BY 'CHANGE_ME_DASHBOARD';
```

Apply the monitoring migrations only after the schema audit and backup described
in the migration runbook. Use a migration-only administrator account; do not put
that account's credentials in either runtime service configuration.

```sh
python3 tools/migrate.py --database monitoring --db-name solarinet
python3 tools/migrate.py --database monitoring --db-name solarinet --check
```

## 2. TLS PKI (internal CA + server/dashboard certs)
Generate an internal CA and issue: a SCP server cert (`server.*`) and a web cert
(`dashboard.*`, SANs covering the hostname/IP). Stage the web cert where Apache
reads it:
```sh
sudo mkdir -p /etc/ssl/solarinet
sudo install -m644 run/pki/dashboard.pem /etc/ssl/solarinet/dashboard.pem
sudo install -m640 run/pki/dashboard.key /etc/ssl/solarinet/dashboard.key
```

The dashboard's Certificates view contacts `https://chlorine.akoria.net` and
checks both the certificate chain and hostname. Install the Akoria root in the
host's system trust store, or set `env[STEPCA_CA_FILE]` in the PHP-FPM pool to a
readable PEM trust bundle. The hardened pool sets `SOLARINET_CA_PEM` to the
public SolariNet CA certificate for its local expiry display; the CA private key
remains readable only by `solari`. Keep the URL on the certificate's DNS name; using an
IP address requires that IP to appear in the certificate's subjectAltName.

## 3. Admin credential
```sh
sudo -u www-solari env SOLARI_AUTH_FILE=/var/lib/solarinet-dashboard/solari-auth.json \
  php /opt/solarinet/dashboard/api/tools/solari-passwd.php --user admin --random
```
Local password login is disabled unless the FPM pool explicitly sets both
`SOLARI_LOCAL_LOGIN=1` and `SOLARI_AUTH_RATE_DIR` to a private writable directory
(mode 0700) owned by the FPM account. This is a break-glass path; successful
SSO does not require it. Five password attempts per source/account in 15 minutes
are allowed. Disabling the switch revokes existing local sessions on their next
request. OIDC sessions expire after one hour or 30 minutes idle; local sessions
expire after eight hours or 30 minutes idle.

## 4. Service identities and server boundary

Install the dedicated service users and dashboard state paths. The canonical
production unit and PHP-FPM pool are the hardened files; the root-level legacy
samples run both processes as `jason` and are for isolated development only.
The active-discovery worker is a separate process with no CA key or control
socket authority; it uses the same DB credentials as `solari` and the lease row
to claim one durable scan at a time.

```sh
sudo install -o root -g root -m 0644 deploy/dashboard/hardened/solarinet-users.sysusers.conf /etc/sysusers.d/solarinet.conf
sudo systemd-sysusers
sudo install -o root -g root -m 0644 deploy/dashboard/hardened/solarinet-dashboard.tmpfiles.conf /etc/tmpfiles.d/solarinet-dashboard.conf
sudo systemd-tmpfiles --create /etc/tmpfiles.d/solarinet-dashboard.conf
sudo install -d -o root -g root -m 0755 /etc/solarinet
sudo install -o root -g solari -m 0640 deploy/dashboard/hardened/server.conf.example /etc/solarinet/server.conf
sudoedit /etc/solarinet/server.conf  # set the actual host name and installed certificate paths
sudoedit /etc/solarinet/server.env  # set SOLARI_DB_PASS only
sudo chown solari:solari /etc/solarinet/server.env && sudo chmod 0600 /etc/solarinet/server.env
sudo install -d -o solari -g solari -m 0755 /var/lib/solarinet/pki
sudo install -o solari -g solari -m 0600 run/pki/ca.key /var/lib/solarinet/pki/ca.key
sudo install -o solari -g solari -m 0644 run/pki/ca.pem /var/lib/solarinet/pki/ca.pem
sudo install -o solari -g solari -m 0600 run/pki/server.key /var/lib/solarinet/pki/server.key
sudo install -o solari -g solari -m 0644 run/pki/server.pem /var/lib/solarinet/pki/server.pem
```

The example includes the enforcing `[ctl]` block. For an existing configuration,
merge the block from `hardened/server.conf.ctl-fragment` and verify the effective
values. Install the server's CA/TLS keys under the ownership rules in
`hardened/PERMISSIONS.md`. Install the hardened systemd unit and verify its
paths match the deployed release before starting it:

```sh
sudo install -o root -g root -m 0644 deploy/dashboard/hardened/solarinet-server.service /etc/systemd/system/solarinet-server.service
sudo install -o root -g root -m 0644 deploy/dashboard/hardened/solarinet-scan-worker.service /etc/systemd/system/solarinet-scan-worker.service
sudo systemctl daemon-reload && sudo systemctl enable --now solarinet-server solarinet-scan-worker
```

## 5. PHP-FPM boundary

Configure the pool with the `solari_dash` password. Keep the pool file root-owned
and mode `0640`; the FPM master reads it and passes the secret to the `www-solari`
workers. Seed the dashboard credential store under `/var/lib/solarinet-dashboard`
with owner `www-solari` and mode `0600`; never put the CA private key or server
DB password there.

```sh
sudo install -o root -g root -m 0640 deploy/dashboard/hardened/php-fpm-solarinet.conf /etc/php/8.4/fpm/pool.d/solarinet.conf
sudoedit /etc/php/8.4/fpm/pool.d/solarinet.conf
sudo php-fpm8.4 -t && sudo systemctl reload php8.4-fpm
sudo deploy/dashboard/hardened/verify-boundary.sh
```

The boundary verifier must pass every check, including the actual socket
connection and queue-submission probes, before the host is considered protected.
Do not enable `enforcePeer` until both the hardened unit and pool are installed
and the dashboard routes use `REQUEST_SUBMIT` for privileged actions. Rollback
instructions and the complete ownership matrix are in
[`hardened/PERMISSIONS.md`](hardened/PERMISSIONS.md).

## 6. Release tree, SPA, and Apache vhost

Build an allowlisted release archive from the tested server and scan-worker
binaries and the matching API and SPA. The archive excludes runtime state,
secrets, and configuration. Install
it below `/opt/solarinet/releases/`; the installer verifies every file hash and
atomically points `/opt/solarinet/current` at the new release. Server, PHP, and
Apache all use that same active tree:

Initialize the pinned RackWire submodule before packaging. The relative submodule
URL resolves to the sibling RackWire repository on either hosting service. The
packager requires its checkout to match the parent repository's gitlink and records the exact
RackWire commit in `RELEASE.json` (`sourceSubmodules`), alongside the SolariNet
source revision. The payload hashes still identify the bytes that were actually
packaged.

```sh
git submodule update --init --recursive
python3 deploy/dashboard/package-release.py --version 1.0.0
sudo python3 deploy/dashboard/install-release.py --bundle deploy/dist/solarinet-dashboard-1.0.0.tar.gz
sudo a2enmod ssl proxy proxy_fcgi headers
sudo cp deploy/dashboard/apache-solarinet.conf /etc/apache2/sites-available/solarinet.conf
sudo a2ensite solarinet && sudo apache2ctl configtest && sudo systemctl reload apache2
sudo systemctl restart solarinet-server solarinet-scan-worker php8.4-fpm
```

After the service restart, browse to `https://dashboard.akoria.net/` (alias
`https://solarinet.akoria.net/`) and sign in. Confirm `solarinet-scan-worker`
is active before testing Discovery scans. To roll back, run
`sudo python3 deploy/dashboard/install-release.py --activate <previous-version>`
and restart the same services. See [`RELEASE-CONTRACT.md`](RELEASE-CONTRACT.md)
for installer constraints and the acceptance checks. Do not remove a retained
release until the rollback window is closed.

The HTTPS vhost sends `nosniff`, same-origin framing, `no-referrer`, a
six-month HSTS policy, and a Permissions Policy that permits camera access to
same-origin RackWire while denying microphone, geolocation, payment, and USB
access. A Content Security Policy applies to the main dashboard shell: scripts
and connections are same-origin, objects are disabled, and the page can frame
same-origin RackWire. `unsafe-eval` remains necessary because
the no-build SPA transpiles JSX with vendored Babel at runtime; inline styles
remain enabled for React style properties. RackWire's independent document is
outside this CSP because its current scanner and diagram tools use inline code
and styles. After installing the candidate vhost, verify the HTTPS response
headers with `curl -sSI https://dashboard.akoria.net/` from a trusted client.

The dashboard polls every 10 seconds. Failed refreshes retain the last real
snapshot with a visible **DATA STALE** age and pause write actions. The demo
fixture is labeled separately. `/api/stream` returns an immediate 204 so older
EventSource clients release their FPM worker and stop reconnecting; a dedicated
event bridge requires a monotonic durable cursor before SSE can return.

## 7. Client agents (host metrics)

A `solariClient` reports a host's CPU/RAM/disk/process metrics to the server's
ingest listener; its HELLO registers the node, and reports populate `hostCurrent`
so the host appears in Fleet Overview / NodeDetail with live metrics.

- **Same-host (self-monitoring):** copy `client.conf.sample` to `run/client.conf`,
  point `primaryUrl` at `tls+tcp://localhost:7701`, use the local `client.*` cert,
  install `solarinet-client.service`, `systemctl enable --now solarinet-client`.
- **Remote host (one command):** `deploy/remote-deploy.sh --host user@HOST`
  detects the target arch, places the client binary, issues a per-host cert from
  the internal CA, drops `client.conf` + CA/cert/key, installs the systemd unit,
  and starts it. `--dry-run` prints the plan first. The node self-registers via
  HELLO on first connect. Requirements:
  - the target user needs passwordless **sudo**, and key-based SSH;
  - the `--server` URL (default `tls+tcp://<server-fqdn>:7701`) must be a name/IP
    in the server cert SAN (now `localhost`, `xenon`, `xenon.akoria.net`,
    `127.0.0.1`, `10.0.0.20` — re-issue `run/pki/server.*` to add others);
  - **non-x86 targets** (e.g. a Raspberry Pi pihole = arm64): build the client
    for that arch first with `deploy/cross-build-client.sh arm64` — a native
    build inside an emulated container (Docker + qemu/binfmt; register once with
    `sudo docker run --privileged --rm tonistiigi/binfmt --install arm64`). It
    outputs `deploy/dist/arm64/solariClient`, which remote-deploy picks up by
    arch. remote-deploy also installs the client's runtime libs (libnng1,
    libmbedtls, libsqlite3-0, libcjson1) on the target.

  Cert issuance: the client's key + CSR are generated for the target and signed
  by the server's internal CA over the solariCtl SIGN verb — the CA private key
  never touches the deploy host. Relocate the CA later via the server [ca]
  block (mode=remote + url).

A reporting client is marked `up` and its `lastSeenAt` advances each cycle.

Client configuration pushes now require a writable `[control] stateFile` or the
derived `<spoolDb>.ctrl` path. The client persists a complete effective config
snapshot with a version, length, and checksum; it syncs and atomically replaces
the file before changing its running config or reporting the new epoch. Legacy
unversioned state is rejected with an error log and the base `.conf` epoch until
the server resynchronizes it. Rehearse that resynchronization before upgrading a
fleet with saved pushes. POSIX file/rename/directory-sync failures and truncated
state are covered by fault tests; Windows durability still needs native proof.

Report delivery carries `TLV_REPORT_SESSION` (0x0105), a random 16-byte
reporter incarnation retained by spooled frames. Migration 021 adds receipts
keyed by node/session/sequence/type, committed with current/history rows. The
server accepts durable reports over its authenticated REQ/REP control endpoint
and sends ACK only after persistence succeeds; reporters remove a frame only
after the ACK matches its sequence and session. Identical receipt replays are
idempotent. Upgrade client and monitor reporters together with the server, then
apply the complete monitoring stream through `tools/migrate.py` after the
production schema audit and backup; do not apply 021 by itself. Do not prune
receipts while a sender could replay an older spooled frame. Local loopback
tests cover ACK validation and retention, but server-handler commit failure,
ACK-loss/restart, primary/failover, and live-fleet compatibility remain release
gates; the delivery guarantee is still a candidate, not a production claim.

## Gotchas
- **`<LocationMatch "^/api/">` needs the trailing slash** — `^/api` also matches
  the SPA's `/api.jsx` adapter and would route it to PHP, breaking the app.
- Production PHP-FPM runs as `www-solari` and uses the reviewed peer-credential
  boundary. The root-level legacy pool sample is development-only; see
  `deploy/dashboard/hardened/` before any production install.
- The service worker serves app code **network-first**; a redeploy is picked up
  on reload. It never stores `/api/` responses because those may contain
  session-bound inventory or health data. An offline API request fails, and the
  dashboard must label any fallback as offline. Bump `CACHE_VERSION` when a
  changed worker must evict older caches; v7 removes the former API cache.
