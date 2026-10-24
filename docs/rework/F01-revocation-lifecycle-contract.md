# F-01 certificate revocation lifecycle contract

**Status (updated 2026-10-23):** the durable operator command, database ledger,
historical issuance backfill, CA-signed CRL generator, static publisher, peer
updater, and node-level current-generation acknowledgement path are implemented
in the local candidate; none are deployed. The
publisher verifies each CRL and exposes it under an issuer-fingerprint URL for
an HTTPS web server. A read-only dashboard API and Certificates-screen
generation/peer view now exist in the local candidate, including explicit
current, stale, pending, offline, expired, and unavailable states. No HTTPS
vhost, public endpoint, deployment config, peer activation, or missing-CRL
rollout policy is installed; nothing is deployed.
The CRL loopback test proves that the pinned TLS stack rejects a
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
- The candidate has a ledger, historical issuance inventory/backfill, CRL
  generator, static file publisher, and peer-side HTTPS validator/atomic
  installer. It has no deployed HTTPS publisher, peer configuration,
  missing-CRL rollout policy, or deployed acknowledgement status view.

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
  verifies the CA key pair, CRL signature/issuer/Authority Key Identifier/
  number/serial set/freshness, and atomically replaces its output with
  world-readable mode for the static publisher. An `unspecified` revocation
  omits the CRL reason-code extension in line with RFC 5280's SHOULD. Each
  server DB session is initialized to
  UTC on connect and reconnect; the generator sets UTC for ledger reads and
  state writes. Audit historical `DATETIME` rows created before this change
  against the server timezone in effect when written. File replacement precedes the database
  generation-state update; if that commit fails in the narrow boundary, the
  output can temporarily be one number ahead of acknowledgement state. After
  DB recovery, rerun the generator and verify both agree. The temporary
  OpenSSL CA database is
  isolated from the application database. The CA private key is read only by
  this generator process. Run once per issuer and distribute every resulting
  CRL.
- `deploy/crl/solari-crl-generate.service` and
  `deploy/crl/solari-crl-generate.timer` define a hardened six-hour schedule
  under the CA-owning `solari` account, with a dedicated DB defaults file and
  private output directory. Systemd unit verification and the nine-test CRL
  generator suite pass locally. These definitions are not installed or enabled;
  deployed DB grants, CA paths/key access, generated output, and actual
  scheduling remain **UNVERIFIED**.
- `deploy/crl/activate_crl.py` validates HTTPS transport, issuer identity, CRL
  signature, time bounds, and monotonic numbering before atomically replacing
  one service's CA bundle and restarting that allowlisted service. It restores
  the prior bundle if restart/active-state verification fails. It accepts only
  one PEM CRL, serializes activation per bundle, limits validation to 120
  seconds, bounds CA bundles to 4 MiB, and gives recovery a 240-second systemd
  reserve. It reports rollback-write failure as requiring manual recovery.
  Eighteen local tests cover valid activation, verified HTTPS, refusal of all
  redirects, signature failure, expiry, rollback, idempotence, timeout recovery,
  pre-replacement write failure, extra-PEM rejection, rollback failure, public
  activation-receipt mode and recovery, and same-number base-bundle
  reconciliation. After a
  verified restart, the updater atomically records the issuer fingerprint, CRL
  number, and CRL fingerprint in a public `<bundle>.activated` receipt. The
  reporter adds this receipt to its authenticated durable client/monitor report;
  the server stores it only when it matches the latest generated, unexpired CRL
  generation. Stale, malformed, or unwritable optional receipts are ignored
  after ordinary report persistence, and later reports retry DB acknowledgement.
  A well-formed stale or unpublished receipt returns `ERR_REJECTED` internally
  and is info-logged; it does not reuse the transport-retry status.
  The
  migration and live DB test cover exact-generation matching,
  idempotence, rotation, and expiry; the report parser and reporter receipt
  suites also pass locally. A candidate dashboard view distinguishes current,
  stale, pending, offline, and expired-generation reporting agents; offline
  means no check-in within 15 minutes or node state `down`. Its denominator is
  client/monitor nodes whose
  current leaf issuer matches that CRL; nodes with no or a different leaf issuer
  are listed as excluded with unknown and different issuers distinguished;
  server-role peers and other services on the host are out of scope. API and UI
  fixtures cover each state, exclusion, and missing-migration behavior. A
  templated systemd timer polls hourly. No hosted endpoint
  or cross-issuer bundle merger exists. Host configuration and live process
  behavior remain **UNVERIFIED**.
- `deploy/crl/publish_crl.py` now validates the generator output against the
  configured issuer and publishes a fingerprint-named static PEM using an
  atomic replacement, monotonic CRL-number check, and seven-day maximum
  lifetime. It applies the peer updater's `thisUpdate`, one-hour minimum
  remaining validity, and validity-interval checks. Its dedicated unprivileged
  account can read the generated CRL and CA certificate; its systemd sandbox
  masks the signing key and DB defaults. A
  one-minute timer checks for a newer generation; an Apache HTTPS-vhost snippet
  serves only public CRL files and denies dotfiles/indexes. Six publisher
  tests pass, including recovery when the previous CRL is expired, rejection of
  future/overlong/near-expiry CRLs, and explicit key/credential path masking.
  The HTTPS host/certificate, installed service, remote retrieval, activation,
  and acknowledgements remain **UNVERIFIED**.
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
- Publish only the signed CRL through the candidate's issuer-scoped static
  publisher and a read-only endpoint on verified HTTPS; the CRL is public and
  clients validate its signature before use. The peer updater verifies
  issuer/signature, CRL number monotonicity, and freshness, then atomically
  installs the CRL bundle and restarts the SolariNet process. **Open:** configure
  a peer-trusted HTTPS host, add an operator-facing view that compares eligible
  peers with the current generation and marks stale or offline peers pending,
  and verify the full distribution path end to end.
- Keep CA certificate and CRL parsing compatible with existing `caFile` users.
  The pinned gossip integration test already covers a revoked leaf, same-CN
  replacement leaf, expired and malformed CRLs, and a wrong-issuer CRL. Peer
  updater tests cover expired input and atomic rollback after restart failure.
  **Open:** prove startup with a missing CRL fails at every deployment entry
  point and exercise the full client/monitor restart and recovery path.

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
revocation. Cross-lab review, hosted HTTPS publication, missing-CRL startup
behavior, fleet-status presentation/convergence, and deployed configuration remain
**UNVERIFIED**. Backend selection is a candidate decision (OpenSSL `ca -gencrl`);
production integration and fresh post-fix review remain open.

## Candidate review follow-up · 2026-10-23

- A fresh independent Fable review flagged legacy control-result compatibility,
  UTC interpretation, the configured issuer's SKI, pending-control cleanup,
  acknowledgement retention, failed-result epoch writes, and incomplete expiry
  presentation. The supplied packet did not include the existing control CLI
  sender or migration files 034/035; the source review below verifies those
  artifacts separately.
- Control results without an epoch are accepted for non-convergent verbs and
  failures without changing `appliedEpoch`. A successful `SET_CONFIG` or
  `PROVISION` result without an epoch is consumed as `epoch-missing`, preserves
  the last applied epoch, and remains eligible for reconciliation. A matching
  correlation and verb are still mandatory. The live MariaDB suite exercises
  exact epoch matching, legacy missing-epoch behavior, and failed results.
- The production control CLI already persists a directive before publishing it
  (`src/server/solariCtl.c`). The active monitoring MariaDB on Xenon reported
  `@@global.time_zone=SYSTEM`, `@@system_time_zone=UTC`, and identical
  `NOW()`/`UTC_TIMESTAMP()` values in a read-only query through the configured
  dashboard DB identity. Xenon's OS timezone is UTC; this confirms the current
  database did not reinterpret older `FROM_UNIXTIME`-derived `DATETIME` rows.
- The active Akoria root certificate on Chlorine contains the required Subject
  Key Identifier. Its public SHA-256 fingerprint matches the previously
  recorded root pin. The generator's configured Xenon certificate path is not
  installed, so that exact file-to-root match remains a deployment check; no
  private key was read.
- Migration 036 changes CRL acknowledgement cleanup to `ON DELETE CASCADE`,
  matching retired-node retention. Pending control rows older than seven days
  are pruned by the scheduled `solari_retention_run`, not on operator-send.
  Failed results and non-convergent results no longer overwrite the last
  successfully applied configuration epoch. A missing CRL `nextUpdate` is now
  treated as expired/unavailable by the API and shown as unsafe in the UI.
- Disposable Xenon verification applied migrations 001–036, exercised the real
  CRL SQL helper's multi-statement UTC read and monotonic allocation, and passed
  the server live DB integration suite 17/17. Its sentinel scratch schema and
  unix-socket test account were removed and independently checked absent. The
  production-handler unit test, migration preflight tests, dashboard API/UI
  fixtures, PHP lint, and `git diff --check` also pass.
- A fresh cross-lab Fable review found a transaction reconnect hazard, a stale
  header comment, a missing migration-036 checksum in the exported manifest,
  operator-send coupling to the expiry purge, noisy duplicate-result logging,
  and coverage gaps. Result application and CRL issue/confirm now use dedicated
  MariaDB connections with automatic reconnect disabled; expired directive
  cleanup is a maintenance procedure intended for nightly invocation; the
  comment and manifest are corrected; unmatched or
  duplicate results log at info; and a present zero epoch is tested. A late
  result still produces an explicit unmatched response, and the process-local
  sequence generator's restart collision risk remains documented below.
- The fresh scratch rehearsal applied migrations 001–036, loaded the actual
  maintenance SQL, proved an eight-day-old pending row is purged by its
  maintenance procedure, and passed the live server database suite 17/17.
  The result unit and ingest tests pass 2/2; migration 036 rollback/forward was
  separately rehearsed (`RESTRICT` to `CASCADE`).
- The live DB suite was run twice against the same scratch schema. The pending
  directive fixture clears its node-scoped test rows before seeding; the second
  run passed. The reconnect test now asserts that MariaDB's thread ID changed
  after `KILL CONNECTION` and `mysql_ping`, proving a replacement session. The
  transaction-only connector omits `CLIENT_MULTI_STATEMENTS` and closes after
  commit without a post-commit reset.
- `dbOpenTxnConn` applies the same five-second connect-timeout and UTC-init
  options as `serverDbOpen`, with automatic reconnect deliberately disabled
  (the pool enables reconnect between transactions). Only the pooled connection
  requests `CLIENT_MULTI_STATEMENTS`, which prepared transaction statements do
  not need.
  Each CONTROL_RESULT opens and closes this connection, so it adds one DB
  authentication round trip with a five-second connect timeout on this
  low-rate control path.
- **Pooled transaction guard follow-up (2026-10-23):** decommission, lifecycle,
  asset purge, enrollment finalization, report ingestion, and lease-fenced scan
  transactions share `dbTxnBegin`/`dbTxnFinish`. The guard pings before the
  transaction, disables transparent reconnect for its duration, and restores a
  fresh configured pool connection if rollback or session reset fails; a later
  database operation retries opening the slot if that replacement could not be
  established. Confirmed
  commits remain successful if only post-commit session cleanup requires
  replacing the connection, avoiding an unsafe caller replay. Live tests now
  assert `@@autocommit=1` and transparent reconnect after `KILL CONNECTION`
  following both a commit and rollback. They do not exercise the replacement
  branch. Unknown node retirement, asset purge, and lifecycle transition return
  `ERR_AUTH_ROLE`; an already-retired existing node remains an idempotent success.
- **UNVERIFIED:** deterministic connection termination between statements of
  each multi-statement transaction. The session-reset tests kill the connection
  immediately after a completed transaction, and the ordinary live cases cover
  commit/rollback behavior; neither proves all mid-transaction driver timing.
- **UNVERIFIED:** rollback/reset replacement branch after a failed connection
  recovery. It is covered by code inspection, but no integration test forces the
  configured replacement connection open to fail and then recover lazily.
- **UNVERIFIED:** an installed timer/cron job invokes `solari_retention_run` in
  the current environment. The procedure is implemented and its SQL passed the
  disposable-schema check; cleanup will not occur automatically until a job is
  configured.
- **UNVERIFIED:** hosted CI on this exact tree, installed generator CA path,
  physical-client CRL activation, sequence-ID collision across process restart,
  and deployment/release. The configured-CA path and trust activation remain
  deployment gates; the collision risk is low-probability but can reject an
  operator send for a node with the same still-pending ID.
