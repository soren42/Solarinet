# F-10 browser trust and managed TLS issuance contract

**Status (2026-10-23):** the candidate now contains a static public-root
artifact generator (17 focused tests) and a release publisher (11 focused
tests). The publisher regenerates every artifact from the validated PEM bytes,
so a valid checksum file cannot make a profile/page for a different root
acceptable. It generates a PEM, Apple root
profile, Firefox Linux policy, HTML page, and checksums from chlorine's live
root certificate; the profile contents and fingerprint were verified against
that self-signed root, and intermediate certificates are rejected. A fresh
read-only retrieval regenerated and checksum-verified the artifacts. They were
temporary and were not served or deployed. An atomic publisher now installs
immutable fingerprint-named releases and switches the `current` symlink while
retaining prior releases for rollback. Its root-run smoke test passed in a
disposable release tree, including owner/mode checks and an atomic `current`
switch. Apache configuration syntax passed with the candidate mapping included.
The live `step-ca`
on `chlorine` is healthy and exposes an ACME directory, verified over HTTPS
using its configured root certificate. Browser trust on devices, ACME client
deployment, server certificate automation, and fleet rollout remain undone.

## Observed current state

- Project documentation identifies `chlorine` (`10.7.0.10`) as the internal
  `step-ca` host and `ca.akoria.net` as a CA name.
- A read-only SSH check observed `step-ca` active and listening on TCP 443.
  `/etc/step-ca/config/ca.json` contains an ACME provisioner named `acme`.
- From chlorine, the ACME directory
  `https://ca.akoria.net/acme/acme/directory` returned the expected ACME
  endpoints when verified against `/etc/step-ca/certs/root_ca.crt`.
- The live public root certificate's SHA-256 fingerprint is
  `D7:40:F6:B0:1A:6E:25:47:D1:3D:6A:37:55:0C:B2:85:EA:16:4C:91:51:A9:31:6A:ED:34:F2:83:8F:39:48:88`.
- The Apple Developer key is not needed to trust this private root. Signing a
  configuration profile may establish profile provenance, but does not
  establish TLS root trust and cannot replace fingerprint verification.
- The live CA runs Smallstep CA 0.30.2. Its authority has no X.509 name policy
  and no authority claims. Provisioners include `admin@akoria.net` (JWK),
  `acme` (ACME), `sshpop` (SSHPOP), and `smime` (JWK); `smime` has explicit
  six-month default / one-year maximum TLS claims. No ACME-specific claims or
  challenge list is configured. A broad authority policy therefore needs an
  audit of existing non-ACME identities before it can be applied.
- The live CA endpoint's `Step Online CA` TLS leaf was valid through
  2026-10-24 18:58 UTC. No separate renewal timer is installed; Smallstep
  documents this self-certificate as a 24-hour leaf renewed internally by
  `step-ca` around two-thirds of its lifetime ([design document](https://smallstep.com/docs/design-document/),
  [renewal](https://smallstep.com/docs/step-ca/renewal/)). `step-ca` was active
  during the check.
- `deploy/dashboard/README.md` documents an optional `STEPCA_CA_FILE` for
  verified dashboard-to-CA requests. Project docs report the root installed
  in Xenon's system trust store; this is not a fleet-wide trust inventory.
- Read-only live endpoint inventory (2026-10-23): a scan of the 49 IP addresses
  in `netdb/akoria-hosts.yml` checked ten common web ports; it is not exhaustive
  for other ports or unlisted devices. On Xenon, Apache and nginx are active;
  Apache serves `akoria.net`/`www.akoria.net` with a Let's Encrypt leaf whose
  `notAfter` was 2026-07-19, and it still served that expired leaf on
  2026-10-23. Its Certbot timer is active, but the 2026-10-23 run failed with
  “Some challenges have failed”; the next timer was scheduled for 2026-10-24.
  **The public HTTPS certificate is expired; no renewal or service change was
  attempted.** Apache's `dashboard.akoria.net` vhost serves a self-signed
  certificate through 2036-07-26; `rackwire.akoria.net` serves a separate
  self-signed certificate through 2028-11-14. Xenon's nginx listener is
  `:8080` without a TLS certificate directive in the active site config.
- Public DNS for `akoria.net` and `www.akoria.net` resolves to Cloudflare
  addresses. Read-only HTTP and HTTPS HEAD requests forced to a Cloudflare edge
  returned HTTP 521 for both names; ordinary in-network DNS instead resolved
  `akoria.net` to Xenon and reached the expired origin certificate. Xenon's
  Apache process is active and bound to wildcard listeners on ports 80 and 443;
  UFW is inactive and `cloudflared` is not active. **Inference:** the failure is
  upstream of Xenon's local listeners. The Certbot log for the 2026-10-23
  13:39 UTC renewal identifies HTTP-01 521 responses for `akoria.net` and
  `www.akoria.net`; it reported NXDOMAIN for `xenon.akoria.net`. A fresh
  20:30 UTC query to public resolvers 1.1.1.1, 8.8.8.8, and 9.9.9.9 instead
  returned `xenon.akoria.net A 10.0.0.20` with a five-second TTL and no AAAA
  answer. Thus public DNS currently publishes a private address for this SAN;
  **inference:** an external HTTP-01 validator cannot route to that RFC1918
  address. The change from NXDOMAIN to a private A answer and its intended
  status are **UNVERIFIED**. The cause of the Cloudflare 521, router/NAT
  configuration, and Cloudflare-to-origin TLS mode remain **UNVERIFIED**. No
  public DNS or certificate settings were changed.
- A TLS-verified, GET-only UniFi Integration API check reported the `chemistry`
  UDM Pro Max online and returned four WAN interface definitions. The documented
  sites/WAN/devices endpoints did not provide current WAN state or port-forward
  mappings. SSH to the `chemistry` hostname passed host-key verification but
  rejected the `jason` key. Therefore whether either WAN currently has Internet
  connectivity and where ports 80/443 forward remain **UNVERIFIED**. No router
  or firewall settings were read through undocumented endpoints or changed.
- Radium's Keycloak endpoint at `10.1.0.10:8443` presents a self-signed
  `sso.akoria.org` leaf through 2036-07-02. Cesium's Forgejo endpoint at
  `10.1.0.200:3000` returned HTTP 200 over plaintext HTTP and presented no TLS
  certificate on that port. The Developer Reference lists a Caddy edge proxy
  on silicon, but SSH returned `No route to host`; its current state is
  **UNVERIFIED**.
- The scan also found TLS endpoints on UniFi, NAS, printer, and other appliance
  management interfaces. They are not included in the SolariNet server ACME
  enrollment set; their ownership and vendor-managed renewal paths need separate
  classification. Current device/browser-specific trust state remains
  **UNVERIFIED**.
- Current device/browser-specific trust state, ACME challenge policy, host
  authorization constraints, provisioner defaults, and existing server
  certificate automation are **UNVERIFIED**.

## Recommendation

Keep `step-ca` as the CA and ACME as the common issuance protocol. Build two
separate user-facing paths around it:

1. A read-only trust portal distributes the public root, an Apple configuration
   profile containing that root, its SHA-256 fingerprint, and device-specific
   install/verification instructions. The first visit must use a trusted
   bootstrap channel (a previously trusted HTTPS origin or an independently
   verified local transfer); the portal cannot bootstrap its own TLS trust.
   Downloading a profile must never be represented as equivalent to trusting
   it. Apple users explicitly approve installation and enable full trust.
   The practical manual iOS bootstrap is to verify the root fingerprint on a
   trusted computer, transfer the profile by AirDrop or cable, install it in
   Settings, then enable full trust. The dashboard trust page is for later
   visits after the root has been installed.
2. A documented ACME service enrollment path lets each server obtain and renew
   its own key and certificate. Use supported ACME clients with narrowly
   scoped DNS-01 or HTTP-01 validation, atomic file replacement, and an
   explicit service reload hook. A custom web server uses the same ACME
   certificate/key files and a documented renew/deploy hook.

Do not place CA signing keys, provisioner JWK secrets, ACME account private
keys, or leaf private keys in the portal, database, or shared repository.
Publish the root fingerprint through an independent trusted channel before a
user accepts the root/profile. The portal serves only public artifacts and
instructions; it does not provide an unauthenticated certificate-signing
proxy.

## Browser and OS behavior to support

| Target | Initial functional path | Lower-friction path |
|---|---|---|
| Safari on iOS/iPadOS | Download/install the profile, then explicitly enable full trust under Certificate Trust Settings. | A concise install guide and profile reduce repeated setup; the owner approval remains explicit. |
| Safari on macOS | Install the root in the System keychain with an explicit trustRoot setting (for example, `security add-trusted-cert -d -r trustRoot -k /Library/Keychains/System.keychain root_ca.pem`); verify Safari. | A concise install guide and downloadable root. |
| Zen and Firefox on macOS | Treat Zen as Firefox-family software; install the root in the System keychain and use Firefox enterprise-root integration or an explicit Firefox certificate policy. Verify the installed Zen build. | Keep policy and keychain steps together in the guide. |
| Firefox on Linux | Use a Firefox `Certificates.Install` policy with the root PEM/DER. Mozilla documents the system-wide `/etc/firefox/policies/policies.json` path and the installation-local `<Firefox installation>/distribution/policies.json` path. Confirm the active policy in `about:policies`. | Package the policy and root together for the supported Linux distributions. |
| Firefox on Android | Install the root through Android's supported user/device trust path and test the current Firefox release on a real device. | Keep browser version and any trust setting in the guide. |
| Firefox on iOS | Use the Apple device trust path; Firefox for iOS shares platform constraints and does not bypass iOS certificate trust. | Same manual profile install and trust setting as Safari. |

Treat per-browser verification as an acceptance test, not an assumption.
Browser trust UX and supported root stores can change across OS/browser
versions.

## Delivery stages

### Stage 1 — usable, self-service trust and server issuance

- Add a SolariNet dashboard trust page with clear root identity, validity,
  downloadable PEM and Apple profile, published fingerprint, and exact
  platform-specific steps. Serve it only over verified HTTPS that chains to a
  root already trusted by the client; after internal-root installation, that
  may be the SolariNet root. The first-use iOS package is transferred and
  fingerprint-checked separately. Current public-certificate coverage for the
  portal is **UNVERIFIED**.
- Make the profile and PEM derive from the live public root certificate as
  build/release artifacts; validate subject, validity, fingerprint, and
  profile payload before publication. Never export a CA key.
- Include the generated Firefox Linux certificate-install policy beside the
  PEM, and verify its path and trust behavior on each supported distribution.
- Add tested, host-scoped ACME examples for Apache, nginx, and Caddy, plus a
  generic client/deploy/reload hook for custom services. Document the verified
  CA bundle needed by ACME clients to connect to the private ACME endpoint.
- Inventory each web endpoint, owner, SANs, challenge route, ACME client,
  certificate/key paths, reload command, and renewal health check before
  enrolling it. Start with an isolated development service.
- Verify issuance and unattended renewal end to end, including key permissions,
  SANs, chain presentation, service reload, expiry alerting, and rollback.

### Stage 2 — reduce repeated device and service work

- Preserve the explicit manual client-trust path; no Apple device management
  is assumed or required for this project.
- Add authenticated SoR-backed service enrollment metadata and dashboard
  status (issuer, SANs, expiry, last renewal, deployment result) without
  storing private keys.
- Add supported client configuration automation for the actual server OSes
  and app stacks. Keep per-service deploy/reload hooks explicit and auditable.

## Security and operations invariants

- The trust portal returns only the public root and a profile containing that
  root. It does not return leaf or signing private keys.
- Display the SHA-256 fingerprint before the install action and in the
  resulting trust instructions. HTTPS protects delivery; the independent
  fingerprint check protects the first trust decision.
- ACME validation must be constrained to approved internal DNS names/IPs.
  Do not enable a challenge method until its routing, DNS authority, and
  issuance scope are tested. DNS-01 credentials, if needed, are host-local,
  minimally scoped, and never rendered to the dashboard.
- ACME issuance must use an explicit, reviewed name policy. In self-hosted
  `step-ca`, X.509 issuance policy is authority-level and therefore affects
  existing non-ACME X.509 provisioners too. Audit every existing identity and
  SAN before applying it; if a narrow web-server allowlist would disrupt those
  identities, use a separate constrained web-issuance authority. Do not assume
  ACME challenge possession alone is a sufficient internal-name policy.
- Pin an ACME certificate lifetime and a matching renewal-check interval after
  confirming the installed `step-ca` version and client behavior. The current
  configuration is expected to use the documented 24-hour default; use a
  longer explicit web-leaf duration if client renewal semantics or CA
  availability cannot safely sustain that cadence. Alert well before expiry
  and monitor CA reachability. Do not change global or existing provisioner
  lifetimes as a side effect.
- Renewal writes new key/certificate files safely and reloads only the
  allowlisted service. Keep private keys root-owned and mode `0600` where the
  service's privilege model permits; otherwise use the narrowest service group
  read access. Failed validation or reload preserves the last known good
  serving pair and raises a visible health error.
- Root rotation is a separate planned trust migration: overlap roots, publish
  both fingerprints/artifacts, verify clients and servers, then retire the
  old root only after measured adoption.
- Avoid treating CRL acknowledgement as proof that a web server loaded its
  renewed certificate; these are separate health signals.

## Acceptance gates

1. Fingerprint in the portal, PEM, and Apple profile all matches the live CA
   root; the PEM and profile are static, read-only public artifacts with
   recorded checksums. No private key appears in portal responses, repository
   artifacts, or logs. Provide OpenSSL/step fingerprint commands, and publish
   the fingerprint through a separate already-trusted channel.
2. A test iPhone/iPad can install the profile, explicitly enable full trust,
   and reach an internal HTTPS
   service after the explicit trust step; Safari and Firefox for iOS both
   succeed. The UI explains the manual step and how to remove the profile.
3. Safari, Zen, Firefox macOS, Firefox Linux, Firefox Android, and Firefox
   iOS each pass a browser handshake test on the actual OS/browser versions in
   use. Record the Firefox Android version and any required trust setting.
4. Apache, nginx, Caddy, and a custom test service each obtain and renew a
   certificate through the internal ACME endpoint, present the leaf and
   intermediate chain (the trust portal distributes the root only), and
   reload without service downtime or key exposure.
5. Challenge authorization and the issuance policy refuse unapproved names;
   expired, wrong-SAN,
   untrusted-CA, and failed-reload cases produce clear errors and preserve the
   prior serving certificate.
6. Apple-compatible TLS leaves use a DNS SAN, `serverAuth` EKU, RSA key size
   of at least 2048 bits or an accepted modern key, SHA-2 signatures, and
   validity no longer than 825 days. ACME policy configuration is tested
   against every existing provisioner before use.
7. Documentation is a single-file HTML manual with inline CSS/JS/SVG and a
   matching PDF, consistent with the project documentation specification.

## References

- Apple Support, [Trust manually installed certificate profiles](https://support.apple.com/en-us/102390).
- Apple Platform Deployment, [Certificates device management payload](https://support.apple.com/en-gb/guide/deployment/dep91d2eb26/web).
- Apple Support, [TLS certificate requirements for iOS 13 and macOS 10.15](https://support.apple.com/en-gb/103769).
- Mozilla Support, [Set up certificate authorities in Firefox](https://support.mozilla.org/en-US/kb/setting-certificate-authorities-firefox).
- Mozilla Support, [Customize Firefox using policies.json](https://support.mozilla.org/en-US/kb/customizing-firefox-using-policiesjson).
- Mozilla, [Firefox policy templates](https://mozilla.github.io/policy-templates/#certificates).
- Smallstep, [Configure ACME clients with step-ca](https://smallstep.com/docs/tutorials/acme-protocol-acme-clients/) and [step-ca ACME provisioners](https://smallstep.com/docs/step-ca/provisioners/).
- Zen Browser, [desktop code structure](https://docs.zen-browser.app/contribute/desktop/code-structure-and-prefs) (Firefox fork).
- Smallstep, [Certificate issuance policies](https://smallstep.com/docs/step-ca/policies/) and [certificate renewal options](https://smallstep.com/docs/step-ca/renewal/).

## UNVERIFIED

The generator's seventeen isolated tests pass, and its generated profile was
verified to contain the exact live CA root with a matching checksum manifest.
An example Apache `/trust/` mapping is scoped to the trust directory, limits
symlink traversal to matching owners, and denies script-like files there. An
isolated Apache 2.4.66 configuration containing it passed
`sudo -n apache2 -t`, and Xenon's existing Apache configuration passed
`sudo -n apachectl -t`.
The unprivileged config test could not traverse `/etc/letsencrypt`; a privileged
read confirmed that the configured certificate files exist and that the
`akoria.net` leaf expired 2026-07-19. The mapping remains an uninstalled
example; the host vhost integration and intended-client HTTPS reachability are
not yet verified. The publisher's eleven isolated tests pass, and a live public
root was generated and published into a disposable local directory; no host
publication path was used. Actual host publication and rollback remain
unverified.
The candidate has not yet tested profile installation or browser trust on a
device, portal HTTPS/bootstrap, ACME issuance for a SolariNet server,
renewal/reload, or hostname policy. The CA live-state observations above were
read-only checks made on 2026-10-23.
