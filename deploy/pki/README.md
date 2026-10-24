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
