# F-01 certificate revocation lifecycle contract

**Status (2026-10-21):** the durable operator command and database ledger
described below are implemented in the local candidate; they are not deployed.
The candidate now has a CA-signed CRL generator, but production scheduling,
bundle distribution, missing/expired CRL policy, and client activation are not
implemented. The CRL loopback test proves that the pinned TLS stack rejects a
revoked certificate while accepting its newly issued same-CN replacement when
given a suitable CA/CRL bundle.

## Observed current state

- The server signs enrollment CSRs with its local mbedTLS CA backend. Certificate
  serials are random positive values.
- The approved leaf PEM remains in its enrollment row. The latest serial and
  SHA-256 DER fingerprint are also stored on the node row. Re-enrollment can
  produce multiple issued certificates for one node, so node serial alone is
  insufficient for revoking every historical credential.
- TLS trusts a configured PEM bundle. The loopback integration test confirms
  that a CRL in that bundle rejects a revoked peer.
- The candidate has a ledger, historical issuance inventory/backfill, and CRL
  generator. It has no peer distribution/activation path or fleet
  acknowledgement/freshness policy.

## Candidate implementation status (2026-10-21)

- Migrations 029–032 add revocation challenges, persisted signer identity, and
  an issuer-qualified issuance inventory. Enrollment issuance is recorded in
  the approval transaction. Direct operator `SIGN` records the identity before
  returning the PEM. Existing revocation rows seed the inventory. The singleton
  `certificateInventoryState` starts incomplete, and all revocation lookups
  require it to be complete. The gate remains closed until an operator runs the
  reconciliation and attestation step. `tools/backfill_certificate_issuance.py`
  now performs the controlled reconciliation in dry-run mode by default. It
  verifies retained and archived leaf certificates against exactly one CA in
  the supplied CA bundle, reconciles current node identities and existing
  revocation/inventory rows, and refuses conflicts or missing history. Applying
  it requires explicit `--apply`, `--confirm-complete`, `--confirm-quiesced`,
  and operator attribution. Out-of-band historical `SIGN` results must be
  supplied using the external certificate manifest; the tool cannot infer
  certificates that were never archived.
- Migration 032 adds per-issuer monotonic CRL number state. The local
  `deploy/crl/generate_crl.py` candidate reconstructs an isolated OpenSSL CA
  index from one issuer's database rows, checks that the inventory is complete,
  verifies the CA key pair, CRL signature/issuer/number/serial set/freshness,
  and atomically replaces its output. The temporary OpenSSL CA database is
  isolated from the application database. The CA private key is read only by
  this generator process. Run once per issuer and distribute every resulting
  CRL.
- `REVOKE_CERT` binds its confirmation to issuer fingerprint, serial, leaf
  fingerprint, node association when present, reason, operator, and a
  ten-minute token digest. Confirmation consumes the challenge and records the
  revocation in one MariaDB transaction. Replaying the same consumed challenge
  is idempotent. A node is retired only when the revoked identity still matches
  its current leaf. The response explicitly says `tls_effective=0`: there is
  no peer activation, so a ledgered revocation is not effective at the TLS
  handshake until every peer loads the matching fresh CRL.
- Verification in this worktree: migrations 031–032 applied successfully to a
  disposable MariaDB 11 schema; the live DB suite passed 15/15 after fixing
  two test fixtures (a duplicate unique certificate identity and an incorrect
  expectation that a historical leaf revocation retires its replacement).
  `tools/test_migrate.py` passed 8/8, the full CTest suite passed 36/36, and
  the live MariaDB suite passed 15/15 on a fresh migration. Four OpenSSL-backed
  backfill tests and three CRL-generator tests passed. Disposable MariaDB checks exercised dry-run, apply,
  current-node reconciliation, an idempotent second dry-run, and rollback after
  an injected insert failure (the readiness gate stayed closed and no partial
  issuance rows remained). CRL tests checked signature, issuer, CRL number,
  reason codes, serial set, freshness bound, incomplete-inventory refusal, and
  preservation of an existing output on malformed input. Production
  migration/backfill, database-to-CRL run against production data, peer
  distribution, deployment, and cross-lab review are **UNVERIFIED**.
- The pinned NNG/mbedTLS loopback handshake test now uses two leaves with the
  same monitor CN and distinct serials. It revokes the historic leaf, verifies
  that leaf is rejected, then verifies the replacement leaf is accepted and
  its logical node identity is admitted to gossip. This covers replacement
  behavior after CRL activation in the fixture; it does not prove fleet CRL
  distribution or acknowledgement.

### Historical inventory procedure

Run this after migration 031, with the server and every certificate issuer
stopped. First inspect the default dry-run report:

```sh
python3 tools/backfill_certificate_issuance.py \
  --database solarinet \
  --ca-bundle /etc/solarinet/ca-bundle.pem \
  --defaults-file /root/.my.cnf --user solarinet
```

For direct `SIGN` certificates created before migration 031, provide an external
JSON manifest. Each array entry has a path to one public leaf PEM, an optional
`nodeId` when it belongs to a node, an `issuedBy` audit label, and optional
`issuedAtMs` (otherwise the certificate's `notBefore` date is used). Relative
certificate paths are resolved from the manifest directory. For example:

```json
[
  {"certificate":"archive/client-2025.pem", "nodeId":1234,
   "issuedBy":"certificate-archive", "issuedAtMs":1761000000000},
  {"certificate":"archive/operator-sign.pem", "issuedBy":"old SIGN archive"}
]
```

Once the dry-run
reconciles every current node, known issuance, and old revocation, the operator
may apply the inventory and attest that the server is quiesced and the archive
is complete:

```sh
python3 tools/backfill_certificate_issuance.py \
  --database solarinet \
  --ca-bundle /etc/solarinet/ca-bundle.pem \
  --external-manifest /root/solari-issued-certificates.json \
  --defaults-file /root/.my.cnf --user solarinet \
  --operator 'Jason C. Kay' --apply --confirm-complete --confirm-quiesced
```

The apply operation uses one MariaDB transaction with a rollback handler and
marks `certificateInventoryState.complete=1` only after every reconciled row
has been written. Re-run the dry-run and verify the database state before
restarting the issuer. Do not attest completeness when any pre-031 direct-sign
certificate is unaccounted for.

## Required behavior

1. An operator can revoke one exact issued leaf certificate by its persisted
   enrollment/certificate identity. Revocation is attributable, idempotent, and
   cannot accidentally revoke a replacement certificate for the same node.
2. The database is authoritative for revocation state. It records the exact
   issuer identity and normalized serial, certificate fingerprint, node and
   enrollment IDs, reason, operator, and UTC revocation time. A unique key on
   issuer plus serial prevents duplicate records. All historical issued leaf
   certificates remain addressable; revocation does not depend on the current
   `node.leafCertSerial` value.
3. A CA-signed CRL is generated from the authoritative ledger with a bounded
   `thisUpdate`/`nextUpdate` interval. Generation fails closed on a missing or
   unreadable CA key, malformed ledger entry, wrong issuer, bad signature, or
   expired output. Publish by writing a temporary file, verifying it against
   the configured CA, fsyncing it, then atomically replacing the active CRL.
4. A node marked retired is rejected by application identity checks immediately.
   TLS revocation is effective on each peer only after that peer has activated
   a fresh CRL. The system must report which nodes have acknowledged the current
   CRL generation and must show stale or unreachable nodes as pending; an
   attempted distribution is not an acknowledgement.
5. CRL freshness has a defined maximum. Peers fail closed when a present CRL is
   expired or absent after the configured rollout grace period. Tests prove the
   actual pinned NNG/mbedTLS behavior for valid, revoked, expired, malformed,
   and wrong-issuer CRLs before selecting the policy.
6. Revoked credentials cannot be reissued from a prior pending approval, and
   certificate pickup/renewal checks revocation state. A re-enrolled node gets a
   new certificate identity and does not clear the old certificate's revocation.
7. The mutating revocation command is operator-only. Read-only status and CRL
   distribution expose no secrets; the web tier never reads the CA private key
   or invokes signing/revocation directly.

## Candidate implementation boundary

- Extend the first-slice ledger to immutable certificate issuance records and a
  revocation ledger. Backfill issued certificates from retained enrollment
  PEMs, parse and verify them against the configured CA, and reconcile each
  serial/fingerprint to its node. Refuse migration completion if any existing
  issued certificate cannot be reconciled; do not silently omit it from the
  revocation inventory.
- Add a destructive, operator-attributed control-socket command with a
  two-step confirmation tied to the exact certificate identity. It inserts the
  revocation record and marks the node retired when the revoked certificate is
  its active identity. A repeated request for the same certificate returns the
  existing result without changing the original audit record.
- Generate CRLs from the database ledger with the system OpenSSL `ca -gencrl`
  command, invoked with an argument vector (never a shell). The current mbedTLS
  signer has no CRL-writing path in this repository. Build an isolated temporary
  OpenSSL index from validated revocation-ledger rows for each generation; the
  database remains the only authoritative ledger and no mutable OpenSSL CA
  database is introduced. OpenSSL documents `-gencrl` as generating a CRL from
  the index file ([`openssl-ca`](https://docs.openssl.org/master/man1/openssl-ca/)).
  A disposable probe on OpenSSL 3.5.5 verified a CRL
  generated from a reconstructed revoked-only index, its CA signature, serial,
  reason, and one-day `nextUpdate`. Production eligibility still requires
  validating the deployed OpenSSL version, command availability, private-key
  file access, reason mapping, output verification, and atomic publication. The
  generator must run under the existing CA-owning service boundary and must not
  expose the key to the web tier.
- Publish only the signed CRL through a read-only endpoint on verified HTTPS;
  the CRL is public and clients validate its signature before use. Add a client
  updater that verifies issuer/signature, CRL number monotonicity, and freshness,
  then atomically installs the CRL bundle and reloads/restarts the SolariNet
  process. Acknowledgements report the activated generation, not download
  success.
- Keep CA certificate and CRL parsing compatible with existing `caFile` users.
  The bundle format and replacement must be exercised by client and monitor
  integration tests, including startup with an expired CRL and update failure.

## Verification gates

- Disposable MariaDB tests cover ledger migration/backfill, exact-certificate
  revocation, duplicates, operator attribution, stale enrollment pickup, and
  historical leaf revocation after renewal.
- Cryptographic tests verify CRL signature, issuer, serial list, reason/time,
  update interval, atomic replacement, and rejection of malformed or stale
  source data. They use a temporary CA and never production key material.
- Process-level integration starts the pinned SolariNet TLS endpoints with
  temporary CA/leaf credentials. It proves a valid peer connects before
  revocation, the revoked peer is rejected after CRL activation, and a current
  replacement leaf remains accepted. It also proves documented stale-CRL
  behavior on restart and failed update.
- Distribution integration proves nodes acknowledge only after the CRL bundle
  is installed and the service has reloaded it. Lost replies are idempotent;
  stale or offline nodes remain visibly pending.
- A cross-lab security review must examine issuer/serial identity, ledger
  completeness, private-key access, confirmation replay, CRL freshness, update
  atomicity, and fleet rollout before deployment.

## Release gate and unverified items

Do not deploy revocation until the complete ledger-to-live-handshake path passes
the gates above. The current CRL loopback test does not prove operational
revocation. Cross-lab review, generation backend/API selection, pinned-stack
freshness semantics, fleet updater/reload behavior, and production
acknowledgement coverage remain **UNVERIFIED**. Backend selection is a
candidate decision (OpenSSL `ca -gencrl`); production integration and review
remain open.
