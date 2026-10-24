# F-07 signed provisioning trust contract

**Status (2026-10-23):** design established and a fail-closed containment gate
is installed on the five current provisioning entry points. A local release
stager now creates CMS signatures for every payload and a canonical manifest
binding release ID, validity interval, supported OS/architecture, file lengths,
read-only execution mode, SHA-256 digests, and the signer certificate
fingerprint. Verification checks the manifest and each payload against the
supplied code-signing CA and separately pinned signer, enforces read-only file
modes, rejects unlisted,
missing, symlinked, traversing, expired, wrong-target, or altered files, and
never publishes or activates a release. Seventeen offline tests cover signature,
path, target, expiry, immutability, executable-mode preservation, and tamper
cases. The authenticated loader,
installer integration, and full VM acceptance suite are still unimplemented.
Fleet provisioning remains blocked by default; the explicit
`SOLARI_ALLOW_UNVERIFIED_PROVISIONING=1` escape hatch is only for an isolated
lab and emits a warning. Keep provisioning isolated and disabled until the
bootstrap trust path and every acceptance gate below pass.

## Observed state

The current fleet renderer defaults to HTTP. iPXE scripts, per-MAC wipe
profiles, unattended installer configurations, firstboot, and the client binary
are fetched without a signature check. The firstboot script is executed as root
and installs the fetched binary. A detached hash served beside an artifact over
the same unauthenticated channel would not fix this: an attacker could replace
both.

## Selected trust chain

1. **Authenticate the first iPXE loader.** UEFI targets must accept the custom
   iPXE loader only through Secure Boot with the operator-controlled key. A
   BIOS/TFTP path without an equivalent authenticated loader is not an accepted
   deployment mode. The signed iPXE loader contains the provisioning signing
   trust root and an embedded script that enables permanent image trust.
2. **Verify before iPXE executes or boots.** The embedded script verifies the
   rendered top-level script before chaining it. The top-level script verifies
   staged per-MAC profiles, kernels, and initrds before execution. Every
   independently downloaded image is checked; trusting a kernel does not
   implicitly trust its initrd.
3. **Authenticate installer inputs.** Debian preseed, Ubuntu NoCloud
   user-data/meta-data, and AutoYaST configuration must be authenticated before
   the installer consumes them. Each supported installer receives the trust
   root through an authenticated, immutable path (for example, a verified
   initrd overlay); fetching both a config and its checksum over plain HTTP is
   forbidden.
4. **Verify root-run firstboot artifacts.** Firstboot and the architecture
   matched client binary are listed in a versioned signed manifest. The
   installed environment obtains the trust root from the authenticated
   installer path, verifies the manifest signature and the exact artifact
   digest, then executes/installs only those bytes. Any missing, malformed,
   expired, or mismatched metadata fails closed before mutation.
5. **Publish immutable releases.** A release directory contains the complete
   artifact set, a canonical manifest, and detached signature. It is staged
   atomically and never modified after activation. The manifest binds release
   ID, supported OS/architecture, relative path, byte length, and SHA-256 for
   every kernel, initrd, installer config, script, and binary. Promotion changes
   only the active release pointer; rollback selects a previously verified
   release.

The signing private key stays outside the provisioning host and repository.
The public trust root is pinned in the authenticated bootloader and each
installer environment. The manifest signature is an additional artifact-level
check; HTTPS alone is not the signing root. Until every supported platform has
an authenticated way to verify its unattended config, that platform is not
eligible for automated installation.

## Candidate artifact tool (partial implementation)

`deploy/fleet/release_manifest.py` stages a source tree into a new, read-only
release directory, signs every payload and `manifest.json` as detached DER CMS,
then verifies the signed manifest, each payload signature, supported target,
exact length/mode/digest, path safety, and release validity window. Verification
requires both the pinned CA path and the independently pinned lowercase SHA-256
fingerprint of the code-signing certificate; trusting any code-signing leaf
under the CA is not sufficient. The root and signer pins must already have
arrived through the authenticated loader or installer path. Never place the
signing key in the repository or on benzene. The tool does not copy a release
to benzene, change an active pointer, alter DHCP/TFTP, or enable provisioning.
It is a point-in-time verifier: read-only permission bits help prevent
accidental edits but do not make files immutable against their owner or root.
There is currently no installer consumer binding later execution to the exact
verified file descriptors or a private verified snapshot. Any future consumer
must close this verify/use race before it is allowed to install or execute
artifacts; a successful CLI verification alone is not deployment authorization.

The current verifier requires OpenSSL 3.0 or later. It checks the signer's
code-signing EKU, digital-signature key usage, and non-CA constraint directly,
then validates the chain against only the supplied trust root. OpenSSL 3.0's
`-no-CAstore` option prevents host trust stores from extending that trust path;
older versions fail closed and are unsupported by this candidate tool.

Example from the isolated release workstation (key and certificates remain
outside the repository):

```sh
python3 deploy/fleet/release_manifest.py create \
  --source ./fleet-release-staging \
  --output ./fleet-releases/2026.10.23-1 \
  --release-id 2026.10.23-1 \
  --not-before 2026-10-23T00:00:00Z \
  --expires-at 2027-01-23T00:00:00Z \
  --target debian/x86_64 --target ubuntu/arm64 \
  --signer-cert /secure/fleet-code-signing.crt \
  --signing-key /secure/fleet-code-signing.key \
  --ca-cert /secure/fleet-code-root.pem

python3 deploy/fleet/release_manifest.py verify \
  --release-root ./fleet-releases/2026.10.23-1 \
  --expected-release-id 2026.10.23-1 \
  --os debian --arch x86_64 \
  --trust-root /authenticated/fleet-code-root.pem \
  --expected-signer-sha256 <pinned-lowercase-64-hex-fingerprint>
```

The tool emits detached CMS signatures in the format used by iPXE's documented
`imgverify` flow; runtime compatibility has not yet been exercised. The current
loader build does not enable `IMAGE_TRUST_CMD`, embed `imgtrust --permanent`, or
provide Secure Boot signing. Installer and firstboot templates also do not yet
verify the manifest. Those remain hard blockers; the local manifest tool does
not make PXE safe to enable. See the [iPXE trust guide](https://ipxe.org/crypto)
and its [`imgtrust` documentation](https://ipxe.org/cmd/imgtrust).

## iPXE requirements

The custom iPXE build must enable `IMAGE_TRUST_CMD`, embed the trust-enforcing
script, and permanently require trusted images. It must verify every kernel,
initrd, and chained script before use. The code-signing certificate must carry
`digitalSignature` key usage and the `codeSigning` extended key usage. These
requirements follow the [iPXE image-trust command](https://ipxe.org/cmd/imgtrust),
[image verification command](https://ipxe.org/cmd/imgverify), and
[cryptography guide](https://ipxe.org/crypto). The guide explicitly states
that `imgtrust` does not authenticate an initrd implicitly and that the
embedded script is needed to enforce image trust.

## Acceptance gates

- Unit tests reject unsigned manifests, wrong release IDs, unlisted paths,
  traversal/symlink paths, length and digest mismatches, wrong signer,
  unsupported OS/architecture, and expired or malformed signatures.
- Tamper each staged iPXE script, profile, kernel, initrd, unattended config,
  firstboot script, and client binary independently; no modified artifact may
  execute or reach a destructive installer action.
- Exercise the full rendered flow for Debian, Ubuntu, and openSUSE in isolated
  VM networks with a scratch TFTP/HTTP(S) server. Verify disk selection,
  interrupted downloads, stale release metadata, and local-disk fallback.
- Verify loader authenticity under Secure Boot. Reject unsigned loader
  replacement, unknown signing key, wrong certificate usage, bad signature,
  and rollback to a revoked release.
- Test immutable release activation and rollback, including interrupted staging
  and concurrent readers. Verify the manifest against the installed bytes.
- Confirm DHCP/PXE remains disabled on all shared networks during testing.

## Deployment gate

No hardware or shared-network install is authorized by this contract. Enable
PXE only after the isolated VM acceptance suite passes and the initial loader
trust root is installed on each target through its authenticated boot path.
Legacy HTTP templates remain compatibility artifacts and must not be selected
by release tooling.
