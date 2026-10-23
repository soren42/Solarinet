# SolariNet CRL generation service

The generator reconstructs a temporary OpenSSL CA index from the authoritative
MariaDB revocation ledger and signs a bounded-lifetime CRL. The systemd service
runs it every six hours as `solari`, the account that owns the CA key. A
dedicated `solari_crl` DB account avoids giving the generator the server's
general database credential. Its password belongs only in
`/etc/solarinet/crl-generator.my.cnf`, never in the repository or command line.

This service generates `/var/lib/solari-crl/current.pem`. A separate unprivileged
publisher validates that file and exposes it as a static object for an
HTTPS-only web server. Publishing does not install the CRL in a peer trust
bundle, restart a SolariNet service, or report activation acknowledgements. Do
not treat a generated or published CRL as effective TLS revocation.
Every MariaDB connection opened by the SolariNet C server daemon sets its
session timezone to UTC, including after automatic reconnect. The generator
also sets UTC in each ledger-read and generation-write session, so
`FROM_UNIXTIME`, `UNIX_TIMESTAMP`, and CRL expiry
checks use one timezone regardless of the database server default. Before
release, audit historical `DATETIME` rows created before this change against
the server timezone in effect when they were written.

## Peer activation candidate

`activate_crl.py` is the local peer-side updater. It requires a verified HTTPS
URL, a separate immutable CA bundle, and the exact issuing CA certificate. It
checks the issuer, CRL signature, number, and validity interval with OpenSSL;
rejects rollback and conflicting same-number CRLs, requires exactly one PEM CRL
in the response, and atomically writes the base CA bundle plus OpenSSL's
canonical CRL output. A per-bundle file lock serializes updates, and the fetch
has a total deadline. It restarts only an allowlisted SolariNet unit after
installing the file. If restart or active-state verification fails, it restores
the previous bundle and retries the restart against that previous state. A
failed bundle restoration is reported as requiring manual recovery. Validation
has a 120-second operation budget, active/base bundles are capped at 4 MiB, and
the systemd unit allows 240 additional seconds for activation and recovery. The
activation unit runs as root to update the trust bundle and invoke systemd, but
it has no CA private-key path or access.

The fetcher refuses every HTTP redirect. By default it uses the operating
system CA store for the HTTPS endpoint. If `--https-ca-file` is supplied during
a direct updater invocation, that file is the exclusive trust store; it is not
added to the system roots. Keep the endpoint hostname covered by its certificate.

The static file publisher is implemented in `publish_crl.py`; it verifies the
CRL issuer, signature, number, and expiry before atomically publishing
`<issuer-sha256>.crl.pem`. A one-minute timer makes generation visible to the web
server shortly after the generator replaces its output. The dedicated static
system account can read
the CA certificate and generated CRL but has no CA-key or database access. The
included Apache snippet is an example to include only inside an HTTPS vhost
whose certificate is trusted by every peer. Do not expose this alias over
plaintext HTTP. Do not enable the activation timer until the TLS vhost and
hostname are installed and verified from a peer. Each updater instance handles
one issuer and one service; different issuer fingerprints have separate files.

Create one non-secret, root-owned config file per service instance, for example
`/etc/solarinet/crl-activation-client.conf`:

```ini
CRL_URL=https://crl-host.example.invalid/solarinet/crl/ISSUER_SHA256.crl.pem
BASE_CA_BUNDLE=/etc/solarinet/trust-roots.pem
ISSUER_CA=/etc/solarinet/issuing-ca.pem
SOLARI_SERVICE=solari-client.service
```

```sh
sudoedit /etc/solarinet/crl-activation-client.conf
sudo chown root:root /etc/solarinet/crl-activation-client.conf
sudo chmod 0644 /etc/solarinet/crl-activation-client.conf
```

Replace `ISSUER_SHA256` with the lowercase issuer fingerprint printed by the
generator. Use a hostname covered by the endpoint's TLS certificate. Keep the base trust
bundle immutable and free of CRLs. The updater timer first runs two minutes
after boot or at the next hour with up to five minutes of jitter. Seed its
active bundle from the configured base bundle before pointing the service's
`tls.caFile` at that path; otherwise an early service restart can fail to load
the missing file:

```sh
sudo install -d -o root -g root -m 0755 /var/lib/solari-crl-active
sudo install -o root -g root -m 0644 /etc/solarinet/trust-roots.pem \
  /var/lib/solari-crl-active/client.pem
```

Replace the source with the exact `BASE_CA_BUNDLE` configured for this peer.
After seeding, configure the service's `tls.caFile` as
`/var/lib/solari-crl-active/client.pem`. Install and enable the template units
only after a real endpoint exists:

```sh
sudo install -o root -g root -m 0755 deploy/crl/activate_crl.py \
  /usr/local/libexec/solarinet/activate_crl.py
sudo install -o root -g root -m 0644 deploy/crl/solari-crl-activate@.service \
  /etc/systemd/system/solari-crl-activate@.service
sudo install -o root -g root -m 0644 deploy/crl/solari-crl-activate@.timer \
  /etc/systemd/system/solari-crl-activate@.timer
sudo systemctl daemon-reload
sudo systemd-analyze verify /etc/systemd/system/solari-crl-activate@.service \
  /etc/systemd/system/solari-crl-activate@.timer
sudo systemctl enable --now solari-crl-activate@client.timer
```

After a successful restart, the updater atomically writes
`<active-bundle>.activated` as `issuer-sha256:number:crl-sha256`. This receipt
contains public metadata only and is mode `0644` so the local client or monitor
reporter can read it beside the exact `tls.caFile` used for its authenticated
connection. The reporter includes it in its authenticated durable report, and
the server records it only when issuer, CRL number, fingerprint, and expiry match
the latest generated CRL generation. A stale, malformed, or unwriteable optional
receipt is ignored after the ordinary report has persisted, so it cannot create
a report retry loop; later reports retry the receipt. This records the reporting
agent's activation only; it does not
prove that another service on the host loaded the CRL. The dashboard candidate
shows the latest generation and client/monitor receipt states, including peers
unseen for more than 15 minutes. Agents with no recorded issuer and agents with
a different current issuer appear in a separate excluded list with distinct
reasons; neither group is silently counted as pending. The status view is not deployed; the hosted
HTTPS endpoint, peer rollout, missing-CRL policy, and live stale/unreachable
behavior remain release gates.

## Database grants

The dashboard DB account needs read access to both CRL tables in addition to
its existing `node` read grants:

```sql
GRANT SELECT ON solarinet.certificateCrlState TO 'SOLARINET_DASHBOARD'@'DASHBOARD_HOST';
GRANT SELECT ON solarinet.certificateCrlAcknowledgement TO 'SOLARINET_DASHBOARD'@'DASHBOARD_HOST';
```

Add the following least-privilege rights to the existing SolariNet server DB
account so report receipts can be stored after migration 035:

```sql
GRANT SELECT ON solarinet.certificateCrlState TO 'SOLARINET_SERVER'@'SERVER_HOST';
GRANT SELECT, INSERT, UPDATE ON solarinet.certificateCrlAcknowledgement TO 'SOLARINET_SERVER'@'SERVER_HOST';
```

Acknowledgement writes are best-effort: missing migration or DB rights are
logged and do not reject an already-persisted report. Later reports retry the
receipt. Without these grants, the dashboard will show peers as pending/stale.

Create a dedicated account using the site's DB host restrictions and secret
policy. Grant only the reads and CRL-state writes used by
`deploy/crl/generate_crl.py`:

```sql
GRANT SELECT ON solarinet.certificateInventoryState TO 'solari_crl'@'CRL_HOST';
GRANT SELECT ON solarinet.certificateRevocation TO 'solari_crl'@'CRL_HOST';
GRANT SELECT, INSERT, UPDATE ON solarinet.certificateCrlState TO 'solari_crl'@'CRL_HOST';
```

Use a strong generated password and the narrowest DB host identity available.
Install a MariaDB client defaults file readable only by `solari`:

```ini
[client]
host=DB_HOST
port=3306
user=solari_crl
password=REPLACE_ON_HOST_ONLY
```

```sh
sudo install -d -o root -g solari -m 0750 /etc/solarinet
sudoedit /etc/solarinet/crl-generator.my.cnf
sudo chown root:solari /etc/solarinet/crl-generator.my.cnf
sudo chmod 0640 /etc/solarinet/crl-generator.my.cnf
```

Confirm `/var/lib/solarinet/pki/ca.pem` is readable by `solari` and
`/var/lib/solarinet/pki/ca.key` is mode `0600`, owned by `solari:solari`. Do not
copy the CA key to a web or proxy account. The CRL output directory is a private
`StateDirectory`. The CRL publisher runs as a dedicated unprivileged system
account. Its public directory is traversable and its public CRL files are
world-readable; they contain no secrets. Its
source directory is group-readable only to `solari-crl-publisher`; it cannot
read the signing key or DB defaults file.

## Install and operate

Install the generator as a root-owned executable from the reviewed release,
then install the service and timer:

```sh
sudo install -d -o root -g root -m 0755 /usr/local/libexec/solarinet
sudo install -o root -g root -m 0755 deploy/crl/generate_crl.py \
  /usr/local/libexec/solarinet/generate_crl.py
sudo install -o root -g root -m 0644 deploy/crl/solari-crl-generate.service \
  /etc/systemd/system/solari-crl-generate.service
sudo install -o root -g root -m 0644 deploy/crl/solari-crl-generate.timer \
  /etc/systemd/system/solari-crl-generate.timer
sudo install -o root -g root -m 0644 deploy/crl/solari-crl-publisher.sysusers.conf \
  /etc/sysusers.d/solari-crl-publisher.conf
sudo systemd-sysusers /etc/sysusers.d/solari-crl-publisher.conf
sudo install -o root -g root -m 0755 deploy/crl/publish_crl.py \
  /usr/local/libexec/solarinet/publish_crl.py
sudo install -o root -g root -m 0644 deploy/crl/solari-crl-publish.service \
  /etc/systemd/system/solari-crl-publish.service
sudo install -o root -g root -m 0644 deploy/crl/solari-crl-publish.timer \
  /etc/systemd/system/solari-crl-publish.timer
sudo systemctl daemon-reload
sudo systemd-analyze verify /etc/systemd/system/solari-crl-generate.service \
  /etc/systemd/system/solari-crl-generate.timer \
  /etc/systemd/system/solari-crl-publish.service \
  /etc/systemd/system/solari-crl-publish.timer
```

Before enabling the timer, verify that the schema migrations are applied, the
certificate issuance inventory is complete, the CA certificate and key match,
and the dedicated account has exactly the grants above. Confirm the configured
CA certificate contains a Subject Key Identifier with
`openssl x509 -in /var/lib/solarinet/pki/ca.pem -noout -ext subjectKeyIdentifier`;
the generator requires it to verify the CRL Authority Key Identifier. Run one
generation manually and inspect its issuer fingerprint, CRL number, revoked count,
fingerprint, and `next_update` output. A pre-publication failure leaves the
prior CRL file unchanged and is reported by systemd. There is a short
non-transactional boundary after the atomic CRL file replacement and before the
generator advances the DB generation row. If the DB commit fails in that
interval, the file can be one number ahead of acknowledgement state. After DB
recovery, rerun the generator and verify the CRL file and
`certificateCrlState` agree before interpreting dashboard status.

```sh
sudo systemctl start solari-crl-generate.service
sudo systemctl status solari-crl-generate.service --no-pager
sudo openssl crl -in /var/lib/solari-crl/current.pem -noout -verify \
  -CAfile /var/lib/solarinet/pki/ca.pem -crlnumber -lastupdate -nextupdate
sudo systemctl enable --now solari-crl-generate.timer solari-crl-publish.timer
```

Install `apache-crl-location.conf.example` within the chosen peer-trusted TLS
vhost (or implement an equivalent static-only location in the site's HTTPS
server). Verify an unauthenticated GET from a peer returns the exact current
CRL and that the same URL is unavailable over HTTP before configuring clients.

`Persistent=true` catches up after downtime. The timer does not create a
publication or activation acknowledgement. Do not claim operational revocation
until the distribution, stale-CRL policy, activation, acknowledgement, and
failure/recovery tests in the [F-01 contract](../../docs/rework/F01-revocation-lifecycle-contract.md)
pass.
