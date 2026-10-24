# F-05 scan worker contract

**Status (2026-10-21):** asynchronous worker/API/UI implementation is present
in the integration candidate. `DISCOVER` admits one durable job through the
operator queue; the main server loop returns without running the sweep. The
standalone `solariScanWorker` owns its DB handle, takes a local singleton lock,
claims only under the live lease, reports bounded progress, and fences writes.
The dashboard exposes admission and requester-scoped polling and retains scan
state in session storage. A clean migration run applies migration 028, the live
MariaDB suite passes all 14 cases, and a disposable worker run completes a real
loopback scan and persists its discovered host. Unit, route, UI, release
packaging, and installer/rollback tests also pass. The worker has not been
installed or enabled on production hosts. Optional enrichment commands now run
in bounded process groups with a 35-second production timeout. Unit tests use a
short test timeout to verify cancellation stops a fake tool descendant, prevents
the result write, and that a continuously writing tool cannot bypass timeout
checks. A disposable MariaDB process test now verifies SIGTERM cancellation,
lease-loss cancellation during a controlled enrichment subprocess, zero stale
result writes, standby refusal, successor orphan recovery, and successful scan
completion. The ASan/UBSan worker binary passes this test. The worker has not
been installed or enabled on production hosts; systemd-managed stop behavior and
deployed lease timing remain outstanding.

**Additional end-to-end verification (2026-10-21):** reran the process/API/SoR
path against temporary MariaDB 11.8.6 bound to loopback. Fresh monitoring and
SoR schemas accepted all 34 and 8 migrations. The native worker scanned a real
loopback listener and persisted an adoptable candidate. The authenticated PHP
API read and adopted that exact row through the C control bridge, creating the
monitoring asset and TCP service target and mirroring one dashboard-provenance
host/address into the separate SoR schema. All 13 HTTP/persistence assertions
passed alongside worker cancellation, lease-loss fencing, standby refusal,
orphan recovery, and successor scan completion. Temporary DBs and rows were
removed. A direct-route network-scale scan now passes separately: the default
release configuration (`SOLARI_WITH_DISCOVERY_TOOLS=OFF`) scanned the directly
connected `10.0.0.0/24` on 2026-10-21, processed all 254 host addresses, persisted
13 candidates, and completed in 10.32 seconds against a fresh scratch monitoring
schema. The worker, temporary schema, and temporary DB account were removed.
No live monitoring or SoR database was written. Before the enrichment change,
a separate tool-enabled build completed its 254-host TCP probe sweep but was
stopped after about two minutes while serial Nmap enrichment had persisted nine
candidates. That run is superseded by the completed measurement below.

**Enrichment latency mitigation (2026-10-21):** optional Nmap enrichment now
receives only the ports already found open by the bounded native TCP sweep,
uses `--version-light`, and limits OS detection to hosts where Nmap has enough
port evidence. A focused unit test captures the Nmap arguments and verifies the
discovered port list and bounded-probe flags. The tool-enabled worker and focused
test build successfully; `test_server_scan` passes. The optimized tool-enabled
worker then completed a directly connected `10.0.0.0/24` against a disposable
monitoring schema: **254/254** hosts processed, **12** candidates persisted,
and terminal completion in **136.34 seconds**. Its scratch DB, temporary user,
config, lock, and worker were removed. **UNVERIFIED:** other VLANs, deployed
worker behavior, and browser polling during enrichment.

**UI fixture verification (2026-10-21):** the dashboard browser acceptance drives
the operator flow through pending admission, running progress, disabled repeat
submission, navigation away and back with session-storage recovery, and terminal
completion with controlled API responses.

**Lease-renewal under active scan verification (2026-10-21):** the two-server
MariaDB process test now starts the real scan worker while the active server and
standby are running. The worker reaches a controlled slow enrichment command;
through more than two renewal intervals, the primary retains the same lease
holder and epoch and advances the expiry timestamp. The worker then stops
on takeover without publishing stale results. The standby acquires the next
epoch and rebinds all listeners; its separate worker marks the orphaned job
failed and completes a queued successor scan. This test is wired into CI and
passed against a fresh local MariaDB 11.8.6 schema. The enrichment command is a
local fixture; no production database is involved.

The live status now distinguishes the host sweep from candidate finalization. It
reports checked hosts and candidates saved, then explicitly says the sweep is
complete while candidate records are still being finalized. The
scanner→SoR→RackWire browser acceptance exercises the incomplete sweep,
post-sweep/no-candidate-yet, one-candidate-saved, and terminal states.

**RackWire-to-worker browser verification (2026-10-21):** the separate browser
acceptance boots the dashboard in operator mode, uses RackWire's Network
discovery handoff, submits through the real PHP API and privileged C queue, and
polls the real native worker until the candidate appears in Discovery. It passed
both the loopback fast-completion path and an opt-in scan of the directly
connected `10.0.0.0/24`: **254/254** hosts, **14** candidates, with the local
listener candidate visible after navigation away and back. The test requires an
explicit `SOLARI_TEST_NETWORK_SCAN=1`, the fixed `/24`, an assigned local
listener address, and a direct route; normal CI stays loopback-only. Both runs
used disposable, fully migrated monitoring schemas and temporary services, all
removed afterward. Other VLANs and deployment remain unverified.

**Observed defect:** the original `DISCOVER` called `serverScanRun` inside
`ctlDispatch`, reached synchronously from `serverCtlPoll` in the lease and fleet
event loop. A broad scan could postpone lease renewal beyond the 15-second
default TTL. The candidate now submits a job instead; the worker process performs
the blocking network work away from that event loop.
**Scope:** operator-triggered active discovery and server shutdown/role-loss handling. Passive discovery remains on its existing path.

## Required behavior

1. `serverCtlPoll` and the main service loop do not wait for a network scan to finish. Lease renewals continue at their configured cadence while a scan runs.
2. A scan has an explicit lifecycle: accepted, running, and one terminal result (completed or failed). The operator can retrieve the terminal count/error using a stable request identifier after the original connection closes.
3. Admission is bounded. The server accepts no more concurrent scans than its configured fixed worker capacity; excess requests receive a clear retryable busy response. A client cannot grow worker or result memory without bound by submitting scans.
4. Each worker owns its database connection and scan resources. It never uses the event-loop's MariaDB connection concurrently. Existing host and port bounds remain enforced before admission.
5. Only the active lease holder starts new scans. The worker checks the lease between bounded connect batches and result writes; fenced DB transactions prevent stale writes. systemd bounds shutdown at 10 seconds. Optional enrichment subprocess cancellation and shutdown behavior still require live verification.
6. Client disconnects do not orphan a worker. The worker unit's local lock prevents duplicate same-host workers; a replacement claim marks a same-epoch orphan failed before claiming queued work. Same-epoch recovery passed against an isolated migrated MariaDB schema; two-host takeover remains required.
7. Authorization remains at the existing operator bridge. Status lookup is scoped so a dashboard peer cannot gain a privileged scan trigger by polling or replaying a job identifier.

## Compatibility and user experience

- Preserve the existing `DISCOVER cidr=... [ports=...]` entry point where practical. If the response contract must change to return a job identifier, update the PHP bridge and UI in the same change and retain a clear busy/error state.
- Never represent an accepted or interrupted scan as a complete zero-result scan.
- Keep the standalone scanner and RackWire discovery services independent; this worker handles only SolariNet's C server scan.

## Selected implementation direction

Run scans in a **dedicated `solariScanWorker` service** with its own process and
MariaDB connection. A cooperative state machine in the event loop is
insufficient: `serverScanRun` performs reverse DNS and can invoke optional
blocking enrichment tools in addition to connect polling. The worker preserves
that behavior without making lease renewal wait, and it does not share the
server's `serverDb` connection.

- `DISCOVER` validates the target, checks the active lease, creates one durable
  job, and returns its ID. The worker claims jobs only when the shared lease row
  names its node and epoch as the live holder. At most one cluster-wide scan is
  queued or running.
- Persist job ID, requester, owner node, lease epoch, target, state, bounded
  progress, result count, and failure reason in a new monitoring migration.
  Job admission, claim, progress, host upsert, and terminal transition lock and
  verify the lease row in the same transaction. A stale owner cannot commit
  discovery state after takeover.
- The worker checks its lease before each bounded scan batch and each result
  commit. On loss or service stop it terminates and closes scan resources. A
  successor marks old queued/running rows failed with an explicit lease-lost
  reason before accepting a new job. A restart of the same server advances a
  fencing epoch so an orphaned prior worker cannot keep writing.
- `DISCOVER_STATUS` requires the original operator identity and returns bounded
  status/progress. PHP exposes `POST /api/discovery/scan` as 202 plus
  `GET /api/discovery/scan/{id}`. The UI polls status, keeps the scan visible after
  navigation, and refreshes candidates only on a terminal state.
- Retain at most 1,000 terminal jobs and no more than 30 days of history.
  Request rows and worker memory remain bounded under repeated submissions.

This changes the control/API response contract, database schema, service
topology, and UI. C, PHP, service files, tests, and documentation land together.
Cross-lab review and lease-takeover rollback rehearsal remain mandatory before
the new API or migration ships.

## Verification gates

- Unit tests cover admission limits, status transitions, disconnect behavior, stale-worker recovery, shutdown, and lease loss.
- An integration test starts a deliberately slow scan while repeatedly exercising the lease tick and ordinary control service; the lease interval remains within its configured budget and the active role is not lost.
- A two-server test proves a standby cannot start a scan and a lease loser cannot continue committing results.
- DB fault injection covers job creation/result persistence failures; failed persistence must be visible as an error and must not produce a false completed result.
- Cross-lab review examines thread/process ownership, database safety, lease fencing, and API/UI compatibility before deployment.

## Release gate

Do not deploy this change until the concurrency model is proven against the real MariaDB client library and all role-loss, shutdown, recovery, and status paths pass. The current `serverDb` implementation is explicitly single-threaded and always returns its first connection, so simply wrapping `serverScanRun` in a thread is unsafe. No threaded implementation is approved by this contract alone.

## Verification status and remaining gates

- C build includes `solariScanWorker`; unit tests cover operator-only admission,
  standby rejection, durable job response, requester-scoped status, scan
  planning bounds, lease loss between connect batches, and cancellation without
  result writes.
- The focused scan suite passes 12 cases. Deterministic worker-clock tests
  verify the first completed-probe transition is written immediately, unchanged
  enrichment progress is throttled until the one-second heartbeat, and a new
  result count is written immediately. Cancellation rejects empty or invalid
  PID markers; subprocess timeout tests require observed output and elapsed
  monotonic time at least equal to the configured test timeout.
- PHP route tests cover HTTP 202 admission, queue handoff recovery, and status
  mapping; dashboard UI tests cover role gating and persisted progress display.
- **Verified:** `test_server_db_live` with `SOLARI_TEST_DB=1` on an isolated
  MariaDB schema migrated through 028 passes all 14 cases, including same-epoch
  orphan recovery and requester scoping. A separate disposable worker test
  completed a loopback scan against a ready listener and persisted the result.
- **Still required:** two-host lease takeover, SIGTERM through the actual
  systemd unit during an active scan, cancellation with deployed enrichment
  binaries, and DB fault injection during concurrent lease loss/result commit.
  The process test uses fake nmap to exercise worker cancellation and child-group
  termination against MariaDB; it does not replace those service-level
  rehearsals. Do not use production data for these tests.
- Cross-lab review of the DB lock order, worker fencing, API status scope, and
  worker service sandbox is mandatory before release.

## Unverified

- systemd-managed SIGTERM/TimeoutStopSec behavior, deployed enrichment binaries,
  and live lease-transition timing.
- Transaction lock order and takeover behavior against MariaDB under concurrent
  lease claim and host-result commit.
- Cross-lab review, production migration behavior, and deployed lease timing.
