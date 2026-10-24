# Static CA trust artifacts

`build_trust_portal.py` creates a static trust package from the public root
certificate. It validates the certificate's CA constraints, signing key usage,
current validity, and SHA-256 fingerprint, then emits:

- `root_ca.pem` — the public trust anchor only;
- `akoria-root.mobileconfig` — an unsigned Apple profile containing that root;
- `firefox-policies.json` — a Firefox policy that imports the root from
  `/etc/ssl/solarinet/akoria-root.pem` on Linux;
- `index.html` — a standalone trust page with platform steps and fingerprint;
- `SHA256SUMS` — checksums for all four artifacts.

The tool never reads the CA key and does not publish, install, or deploy the
output. It accepts only a currently valid, self-signed CA root and refuses to
overwrite an existing output path. It writes into a private sibling staging
directory and atomically renames the complete directory into place. Keep
generated output out of Git; rebuild it from the live public certificate
during a reviewed publication step. The published directory is mode `0755`
and its public artifacts are mode `0644`; its parent path must also be
traversable by the intended web-server account.

`apache-trust-portal.conf.example` is an optional static-only location mapping
for a verified HTTPS VirtualHost. It serves `/trust/` from
`/var/lib/solarinet/pki/trust/current`, disables directory listings and
unowned symlink following, denies script-like files only within the trust
directory even when a server-wide Files/FilesMatch PHP handler is active, and
assigns Apple's profile MIME type only there. The complete VirtualHost must
have no overlapping `<Location>` or `<LocationMatch>` handler for `/trust/`,
because those sections merge after Files/Directory sections. `current` points
to a root-owned, immutable directory under `releases/`; owner-matched symlink
traversal lets a rotation switch that pointer atomically. It uses `mod_alias`
and `mod_headers`; enable both before including it. The publication root and all parent
directories must be root-owned and traversable by Apache. Review the target
VirtualHost and validate the complete Apache configuration with `apachectl -t`
before enabling this mapping. This example is not installed or enabled on any
host.

## Build and verify

On a trusted workstation with SSH access to chlorine and OpenSSL 3.0 or later
(required for isolated root verification):

```sh
umask 077
ssh jason@chlorine 'sudo -n cat /etc/step-ca/certs/root_ca.crt' > /dev/shm/akoria-root-ca.pem
portal_out="/dev/shm/solarinet-trust-portal-$(date +%Y%m%d-%H%M%S)"
python3 deploy/pki/build_trust_portal.py \
  --root /dev/shm/akoria-root-ca.pem \
  --output "$portal_out"
(cd "$portal_out" && sha256sum -c SHA256SUMS)
openssl x509 -in "$portal_out/root_ca.pem" \
  -noout -subject -issuer -dates -fingerprint -sha256
```

Compare the fingerprint with the independently published value before
installing the root on any device. The first portal visit must be bootstrapped
through a trusted transfer: send `akoria-root.mobileconfig` to iPhone/iPad with
AirDrop or open it from Files after a trusted transfer; send `root_ca.pem` to
macOS/Android using AirDrop, cable, USB, or another already trusted path. For
iPhone/iPad, install the profile in Settings, then explicitly
enable full trust under Settings → General → About → Certificate Trust Settings.
Manually downloaded
profiles do not enable TLS trust automatically. This workflow is user initiated
and does not require Apple device management or an Apple Developer key. Later
visits to the portal require an HTTPS certificate the device already trusts;
the portal cannot establish its own initial trust.

For macOS Safari, verify the fingerprint on a trusted channel, then install the
root into the System keychain and explicitly trust it for TLS. Firefox 120 and
later can use roots from the macOS system store; Zen is Firefox based, so enable
Firefox's third-party system-root trust setting and verify the installed Zen
build separately. Limit macOS trust to TLS and basic X.509 validation:

```sh
sudo security add-trusted-cert -d -r trustRoot -p ssl -p basic \
  -k /Library/Keychains/System.keychain root_ca.pem
```

To remove it, locate the certificate by its SHA-256 fingerprint in Keychain
Access and delete it from the System keychain. For Android, install the root as
a CA certificate in Android Settings after verifying its fingerprint. Firefox
120 and later can use third-party roots in the Android system store by default. Restart each browser
and test against a known HTTPS host. Device labels and browser builds can
change; use the vendor references embedded in the generated portal page.

For Linux Firefox, install the public root and policy, then restart Firefox:

```sh
sudo install -d -o root -g root -m 0755 /etc/ssl/solarinet /etc/firefox/policies
sudo install -o root -g root -m 0644 root_ca.pem /etc/ssl/solarinet/akoria-root.pem
sudo install -o root -g root -m 0644 firefox-policies.json /etc/firefox/policies/policies.json
```

Firefox supports the system-wide `/etc/firefox/policies/policies.json` path on
Linux. An installation-local policy can instead go in the browser
installation's `distribution/policies.json` directory. Keep the root at the
absolute path in the policy and confirm it is active in `about:policies`.
Check the [Mozilla policy installation guide](https://support.mozilla.org/en-US/kb/customizing-firefox-using-policiesjson)
for the installed build's supported location.

## Private CA issuance and renewal for web servers

`acme_certificates.py` is the candidate host-side automation for Apache,
nginx, and custom HTTPS services. It enrolls with Certbot against a private
ACME directory using HTTP-01 and a dedicated Certbot store, so private-root
trust settings and renewal records cannot alter the host's public ACME
lineages. The host must provide Python 3.10 or later, Certbot 4.0.0 or later,
and OpenSSL 3.0 or later. Use an apt-packaged Certbot rather than a snap build;
the hook, webroot, and CA trust paths must be visible to the Certbot process.
Certbot 4.0 is required for lifetime-based renewal
windows that work with the short-lived certificates in the pilot. The helper
checks both tool versions; isolated chain verification uses OpenSSL 3's
`-no-CAstore` option. Certbot still reads its default `/etc/letsencrypt/cli.ini`
and per-user XDG `letsencrypt/cli.ini` files when given a dedicated
`--config-dir`; command-line options take precedence. The helper explicitly
clears global pre/post hooks, disables hook-directory executables, and supplies
its own deploy hook. Review both default CLI files for other settings before
use. A legacy `renew-hook` setting conflicts with the helper's deploy hook and
can make Certbot reject the run; remove such a setting before the pilot. A
global `server` setting must match the registered private ACME endpoint and
its root bundle. On a disposable pilot only, verify that a harmless default
pre/post hook is suppressed; do not edit an existing host's public Certbot
configuration for this rehearsal. The deploy hook checks the exact
registered domain set, leaf SANs, server-auth EKU, key usage, at least six
days of certificate lifetime, private-key match, and chain to the
configured private root. It stages a versioned certificate/key directory,
switches one symlink atomically, runs a service-specific configuration check,
and reloads the service. A failed check or reload restores the previous
symlink. The key is stored as mode `0600` unless a service group is explicitly
configured, in which case it is `0640` and readable only by that group.

The live CA's ACME provisioner still lacks an audited name restriction and an
explicit challenge allowlist. **Do not issue against the live CA yet.** The
isolated, name-restricted step-ca/Certbot/Apache pilot described in
[F-10](../../docs/rework/F10-trust-and-issuance-contract.md) passed on
2026-10-24, including denial of an unapproved but challengeable name, forced
renewal, and rollback. That validates the client helper and disposable CA only;
it does not constrain Chlorine. Before any live issuance, audit all identities
affected by policy and confirm the chosen live policy cannot broaden them.
Smallstep warns that unrestricted ACME provisioners
can issue for any name whose challenge can be satisfied and recommends
explicit name policies and challenge controls ([provisioner guidance](https://smallstep.com/docs/step-ca/provisioners/),
[production considerations](https://smallstep.com/docs/step-ca/certificate-authority-server-production/)).

**Policy scope requires a CA-wide identity audit.** Open-source self-hosted
step-ca only supports X.509 name policy at the authority level; applying a
DNS/IP allowlist for web services also constrains JWK and S/MIME issuance,
but not SolariNet's separate mTLS CA used by `solariCtl` node enrollment. The
live Chlorine configuration currently has no authority X.509 policy, and its
ACME provisioner has no explicit challenge list or claims. Before changing it,
inventory every current and planned step-ca certificate SAN/subject across
S/MIME, CA endpoints, CRL publication, and web services. Then either define a
complete tested authority-wide allowlist or design a separate web-only CA
trust path. A separate web CA is the current architectural recommendation to
contain its strict DNS allowlist and HTTP-01-only policy; determine first
whether target browsers already trust Chlorine's root, because a second root
could add client setup. No live CA policy or provisioner was changed. Do not
assume that a per-provisioner name policy is available in self-hosted step-ca;
the [policy reference](https://smallstep.com/docs/step-ca/policies/) documents
the authority-wide limit.

The checked-in [example registry](acme_services.example.json) uses the reserved
`example.invalid` DNS name and a non-routable `ca.example.invalid` CA address.
Replace both names and every certificate path before any issuance. Copy it to a root-owned,
mode-`0600` host file only after replacing every value from an approved service
inventory. Each service entry has an exact DNS SAN allowlist, a distinct
Certbot lineage, a target directory, a key-reader group, and explicit argv
arrays for configuration validation and service reload. The registry is
trusted root configuration because its commands execute as root; do not make
it writable by a service account. The private Certbot store must remain outside
the host's normal `/etc/letsencrypt` store, and every certificate target must
be a direct child of the configured `target_root`.

For a disposable test host, prepare the webroot and private CA trust file,
install the helper into a root-owned path, and copy the registry:

```sh
sudo install -d -o root -g root -m 0755 \
  /usr/local/libexec/solarinet-pki /usr/local/share/doc/solarinet-pki \
  /var/lib/solarinet/acme-webroot/.well-known/acme-challenge \
  /etc/solarinet/pki /etc/ssl/solarinet
sudo install -o root -g root -m 0755 deploy/pki/acme_certificates.py \
  /usr/local/libexec/solarinet-pki/acme_certificates.py
sudo install -o root -g root -m 0644 deploy/pki/README.md \
  /usr/local/share/doc/solarinet-pki/README.md
sudo install -o root -g root -m 0644 /path/to/verified/test-root.pem \
  /etc/ssl/solarinet/test-root.pem
sudo install -o root -g root -m 0600 /path/to/reviewed-test-registry.json \
  /etc/solarinet/pki/acme_services.json
sudo install -d -o root -g root -m 0700 /etc/letsencrypt-solarinet
sudo /usr/bin/python3 /usr/local/libexec/solarinet-pki/acme_certificates.py \
  --registry /etc/solarinet/pki/acme_services.json --plan-service example_web
```

Configure the intended HTTP virtual host with the matching
[Apache](apache-acme-webroot.conf.example) or
[nginx](nginx-acme-webroot.conf.example) snippet. The challenge URL must reach
the exact webroot from the CA host on port 80 without an authentication gate,
rewrite, or redirect that changes the challenge response. Validate the
complete server configuration before enabling the route. Then issue only the
registered service certificate:

```sh
sudo /usr/bin/python3 /usr/local/libexec/solarinet-pki/acme_certificates.py \
  --registry /etc/solarinet/pki/acme_services.json --issue-service example_web
```

Certbot's hook is saved with the lineage so a successful first issuance and
future renewals use the same validation and rollback path. The dedicated
`solarinet-acme-renew.service` and `.timer` templates invoke renewal only in
`/etc/letsencrypt-solarinet`, with the private root supplied to Certbot as
`REQUESTS_CA_BUNDLE`. Install and enable the timer only after a test issuance,
web-server configuration cutover, forced-renewal rehearsal, default CLI file
review, and an external expiry-monitoring check pass. The
pilot provisioner should issue a default lifetime of at least seven days and
cap certificates at 30 days so a twice-daily renewal timer has room to
recover from transient CA or host outages. Certbot 4.0.0 and later uses a
lifetime-based renewal threshold: one half of lifetime for certificates of ten
days or less, and one third for longer certificates. The timer may check twice
daily, but Certbot only issues inside that window. Confirm the installed
Certbot version and observed threshold during the disposable pilot. Certbot HTTP-01 challenge files
must be readable by the web server; the supplied renewal service uses
`UMask=0022` for public challenge files while Certbot keeps account and leaf
private keys mode `0600`. Its systemd sandbox uses `ProtectSystem=full`:
`/etc` is read-only except the private Certbot store and certificate root,
while `/var` remains writable so Apache/nginx syntax checks can open configured
logs. The challenge webroot is explicitly writable in `/var/lib`. Public
Let's Encrypt certificates remain in the host's normal Certbot store.
The supplied systemd unit grants write access to the documented private store,
certificate root, and challenge webroot. If a host uses different paths, add
matching reviewed `ReadWritePaths` before enabling its timer.

The systemd timer supplies the randomized start delay. The renewal command
passes `--no-random-sleep-on-renew` so Certbot does not add a second delay;
this keeps the systemd service's 45-minute start limit aligned with the
helper's 30-minute Certbot execution limit. Do not remove either bound without
rechecking the renewal timeout and alerting behavior.

The renewal command requires the private Certbot renewal file set to match the
registry exactly. Enroll each new service before adding it to the timer's
registry; a pending registry addition intentionally makes renewal fail closed.
For service retirement, stop the timer, remove the service from the registry,
delete its lineage with Certbot using the private `--config-dir`, `--work-dir`,
and `--logs-dir`, then verify the remaining registry and renewal files match
before restarting the timer.

## Certificate expiry alerts

`acme_expiry_watch.py` checks each configured leaf's exact DNS and IP SAN sets
and `notAfter` date, then publishes warning/critical transitions through the
existing `notify-send.py` and RabbitMQ `notify.events` path. It alerts at 21
days and escalates at 7 days by default. Notifications are deduplicated by
service, state, and certificate fingerprint; a renewed healthy leaf emits a
recovery message. Its state file is replaced atomically and contains no
credentials. Certificate parse errors and SAN mismatches are critical.

Install a root-owned mode-`0600` config from
`acme_expiry_watch.example.json`; Xenon's reviewed current certificate
inventory is in `acme_expiry_watch.xenon.example.json`. Replace or remove
entries as endpoints are migrated, and replace placeholder certificate paths
and names with the approved service inventory; include optional `ip_sans` when
a deployed certificate intentionally contains IP SANs. Include public ACME
lineages and private-CA releases as needed. The root-run watcher's
`notify_command` is
an argv array, not a shell command, but remains executable root policy; keep
the configuration root-owned and mode `0600`. Because the unit runs as root,
the notifier executable and its resolved target must be root-owned and located
beneath root-owned, non-writable directories. The watcher checks both the
configured path and resolved target immediately before execution. Both
programs use Python isolated mode so environment variables and user-site
packages cannot redirect imports. Never point
it at a checkout, home directory, venv, or other user-writable path. The Xenon
example invokes an installed executable copy of
`notify-send.py` under `/usr/local/libexec/solarinet-pki`; install the distro's
`python3-pika` package (or another root-managed Python environment) and keep
the AMQP config at
`/etc/solarinet/pki/notify.conf`, root-owned mode `0600`. Keep credentials out
of the watcher config and repository. The notifier emits generic parse and
publisher errors so malformed config or broker failures do not print the AMQP
URL.

Run the inert check first, then verify the notify publisher against the
existing RabbitMQ notification path before enabling the timer:

```sh
sudo install -o root -g root -m 0600 deploy/pki/acme_expiry_watch.example.json \
  /etc/solarinet/pki/acme_expiry_watch.json
sudo /usr/bin/python3 -I /usr/local/libexec/solarinet-pki/acme_expiry_watch.py \
  --config /etc/solarinet/pki/acme_expiry_watch.json --dry-run
```

Replace the example config before running it. Install the watcher at
`/usr/local/libexec/solarinet-pki/acme_expiry_watch.py`, then install
`acme_certificates.py` beside it at
`/usr/local/libexec/solarinet-pki/acme_certificates.py`; the watcher imports
that sibling module in Python isolated mode. Install
`notify-send.py` at `/usr/local/libexec/solarinet-pki/notify-send.py` with
mode `0755`, then install `solarinet-acme-expiry-watch.service` and `.timer` under
`/etc/systemd/system/`. The unit runs as root to read root-only Certbot paths
and uses `StateDirectory` for private state storage. Verify with
`sudo systemd-analyze verify` and a real notification before enabling the
hourly timer. The watcher does not issue certificates. Do not enable unattended
certificate renewal until both expiry alerts and the renewal/reload/rollback
rehearsal pass.

The opt-in integration test `deploy/pki/acme_integration.py` builds a disposable
Certbot 4.0.0/Apache client image from a digest-pinned Certbot base and pinned
Alpine packages, then runs step-ca v0.30.2 and the client on an internal,
egress-disabled Docker network. The image build downloads those packages before
the test starts. The test checks HTTP-01 issuance, an exact CA name policy
(including rejection of an unapproved but challengeable name), trusted HTTPS,
forced renewal, and rollback after an injected service-check failure. It does
not contact Chlorine, expose ports on the host, or alter host service
configuration. On a disposable Docker runner, invoke it with:

```sh
sudo env SOLARINET_ACME_INTEGRATION=1 TMPDIR=/dev/shm \
  python3 deploy/pki/acme_integration.py
```

Deployment also refuses unexpected entries under a service's `releases/`
directory. Investigate unexpected files before changing them; do not remove
release directories while they may be needed for rollback.

To recover after an issued certificate could not be deployed, correct the
service check/reload configuration and retry the registered lineage without
requesting another certificate:

```sh
sudo /usr/bin/python3 /usr/local/libexec/solarinet-pki/acme_certificates.py \
  --registry /etc/solarinet/pki/acme_services.json --deploy-service example_web
```

Rehearse renewal and the complete service deploy path with a disposable CA
using `--renew --force-renewal`. The command forces every registered lineage
in the private store, then verifies that every current symlink and deployed
certificate matches Certbot's active lineage. The renewal service returns a
failure if a deploy hook logged an error or left an old certificate active.

Point Apache or nginx at `TARGET/current/fullchain.pem` and
`TARGET/current/privkey.pem` only after the active symlink exists. For a custom
server, configure those same stable paths, set a reader group in the registry
if it does not load keys as root, and supply its syntax check and reload/restart
commands as argument arrays. Keep old version directories until rollback is
no longer required. The tool does not copy key material to a dashboard, CA, or
shared repository.

For an operator-directed rollback, stop the renewal timer and record the current
link target. Select a previously verified release directory under
`TARGET/releases/`. As root, create a temporary symlink and atomically replace
`current`, then run the service check and reload commands from that service's
reviewed registry entry. If either command fails, atomically restore the
recorded link target and reload again. Keep this action inside the service's
declared target; do not edit or copy certificate/key files by hand. A manual
rollback intentionally makes `--renew` fail its post-check until the Certbot
lineage is restored to `current` (for example, with `--deploy-service`) or
successfully renewed. Keep unattended renewal disabled during recovery.

The helper validates SANs, TLS server usage, validity at the time of
deployment, chain, and key correspondence. It also rejects leaves below RSA
2048 bits, EC keys outside NIST P-256/P-384/P-521, signatures below SHA-256,
and validity periods over Apple's 825-day TLS ceiling. Keep the issuing CA
policy aligned with these checks and test both layers in the isolated pilot.

Focused tests cover valid chain/SAN/key acceptance, domain and fullchain
mismatch rejection, key permissions, atomic activation, rollback after reload
failure, and inert planning. They do not replace the required live ACME pilot,
host-specific challenge routing test, forced renewal, or browser handshake.

## Publish and rotate

Run this on the HTTPS host as root after the trust portal's virtual host has
passed the publication gate. The publisher validates the exact artifact set
and checksums, regenerates every artifact from the validated `root_ca.pem`
bytes, and rejects any profile/page mismatch before it creates an immutable
fingerprint-named release and atomically replaces `current` with a relative
symlink to that release. Prior releases stay in place for rollback.

The publisher imports its sibling builder module. Install both reviewed files
into a root-owned directory before running the publisher; do not execute Python
from a user-writable checkout with `sudo`.

```sh
sudo install -d -o root -g root -m 0755 /usr/local/libexec/solarinet-pki
sudo install -o root -g root -m 0644 deploy/pki/build_trust_portal.py \
  /usr/local/libexec/solarinet-pki/build_trust_portal.py
sudo install -o root -g root -m 0755 deploy/pki/publish_trust_portal.py \
  /usr/local/libexec/solarinet-pki/publish_trust_portal.py
sudo install -d -o root -g root -m 0755 /var/lib/solarinet/pki/trust
sudo /usr/local/libexec/solarinet-pki/publish_trust_portal.py \
  --source /path/to/verified/solarinet-trust-portal \
  --expect-sha256 D740F6B01A6E2547D13D6A37550CB285EA164C9151A9316AED34F2838F394888 \
  --root /var/lib/solarinet/pki/trust
sudo apachectl -t
sudo systemctl reload apache2
```

To roll back, activate a previously retained immutable release directly. This
path verifies ownership, modes, the exact file set, checksums, release name,
and the independent root pin without regenerating artifacts from the current
builder templates:

```sh
sudo /usr/local/libexec/solarinet-pki/publish_trust_portal.py \
  --activate-release FINGERPRINT_HEX \
  --expect-sha256 D740F6B01A6E2547D13D6A37550CB285EA164C9151A9316AED34F2838F394888 \
  --root /var/lib/solarinet/pki/trust
```

The current pin is the Akoria root's SHA-256 from the independent channel
recorded in [F-10](../../docs/rework/F10-trust-and-issuance-contract.md).
Recheck that value before each publication. For a CA rotation or rollback to a
different root, pass that root's separately verified fingerprint; the
publisher refuses to create or activate a release when the pin does not match.
Transfer only the five generated artifacts: the publisher intentionally rejects
extra files such as `.DS_Store` or editor backups.

Apache follows `current` only when the symlink and target directory have the
same owner. Run the publisher with `sudo` so the root-owned release tree
satisfies this check. The publisher never deletes prior releases or reads a
private key.

See [F-10](../../docs/rework/F10-trust-and-issuance-contract.md)
for the browser-specific boundaries and the remaining rollout gates.

## Publication gate

Before serving these files, select and verify a portal HTTPS endpoint trusted
by first-time clients, confirm its network access boundary, review the exact
root fingerprint through a separate trusted channel, and test profile
installation on the actual iPhone/iPad and browsers. The HTTPS host must be
reachable for intended clients and have a certificate those clients already
trust; the root portal cannot bootstrap its own trust. No host currently serves
these artifacts; public certificate coverage for the portal remains
**UNVERIFIED**.
