# SolariNet — Findings and Recommendations

*Independent architecture and code review · 2026-08-09 · repository commit `586cac191eb6e33d77652d08669d66cb006eabe1`*

## 1. Executive verdict

SolariNet has a credible architectural core and several unusually good local engineering practices: a compact versioned protocol, bounded C parsers, prepared SQL in the principal data paths, mutual-authentication support, provenance-aware data modeling, fail-dark UI conventions, and meaningful unit and integration tests. The full dependency-enabled C build completed; CTest reported 30/30 non-failing registrations, of which 29 executed and passed while the live-DB registration self-skipped.

The project is nevertheless **not ready to expand its production control surface**. Feature breadth has outrun the enforcement and durability mechanisms that should make those features safe. The most important project claim—one honest, resilient control plane—is currently weakened by four classes of defect:

1. **Identity and authorization are not end-to-end.** A valid node certificate is not bound to the logical node identity in a frame. Dashboard viewers can invoke many writes. The C bridge treats an operator name as attribution, not authorization. PHP and the privileged server run under the same broad Unix account.
2. **Several “durable” paths can silently lose or misstate work.** SoR CDC is not actually at-least-once, monitor reports are persisted twice, config can be acknowledged before it is durable, and CONTROL results are accepted without adequate correlation.
3. **Long-running work is embedded in critical event loops.** A dashboard-triggered scan can block the single server thread that renews the active lease and ingests fleet data.
4. **Operational claims are not reproducibly proven.** Bootstrap and migration paths diverge, CI silently skips the live DB test and most operational suites, provisioning artifacts execute over unauthenticated HTTP, and backup existence is tested more strongly than restoreability.

**Recommendation:** declare a feature freeze and create a stabilization release. Do not make Tab5 approvals, one-button PXE provisioning, arbitrary MCP SQL, or Opie automation production-authoritative until their release blockers below are closed. Resume roadmap work only after authorization, identity binding, durable delivery, schema lifecycle, and end-to-end fault tests become enforced invariants.

## 2. Review method and confidence

The review used four independent read-only passes:

- C control plane and SQL lifecycle;
- PHP API and browser dashboard;
- operations, integrations, firmware, and CI;
- cross-cutting architecture and failure-chain analysis.

An additional sibling agent independently challenged the completed report and blocked its first draft on two precision errors, which were corrected before publication. The harness does not expose lab identity or a non-OpenAI model roster, so the operator standard's cross-lab review gate could not be satisfied; this report must not be represented as cross-lab-reviewed.

Claims use these labels:

- **[Observed]** directly supported by the cited repository state.
- **[Inferred]** follows from observed components but was not exercised live.
- **[Unverified]** requires a live service, database, broker, network, or hardware check.

Priority means:

| Priority | Meaning |
|---|---|
| P0 | Do not enable or rely on the affected production capability until fixed. |
| P1 | Required for a trustworthy stabilization release. |
| P2 | Required before the stated scale, HA, or maintainability claims are credible. |
| P3 | Important hardening or debt; schedule after the invariants above. |

No live services, network state, database state, firmware, deployment, git history, or external system were changed.

## 3. What the project has done well

These are foundations worth preserving rather than redesigning away:

- **Protocol discipline.** SCP has explicit framing, byte order, CRC validation, bounded TLVs, and forward-compatible unknown-field handling. The codec and control logic have meaningful tests.
- **Portable core.** The zero-dependency C build and feature-gated dependency model work. Default and full builds succeeded during this review.
- **Strong local input handling.** The PHP layer generally uses prepared statements, whitelists dynamic columns, emits consistent envelopes, and relies on React escaping rather than raw HTML.
- **Good failure instincts.** Certificate signing fails closed when its backend is unavailable; panel behavior is fail-dark; control epochs are intended to be monotonic; destructive UI actions use confirmation ladders.
- **A coherent SoR idea.** Provenance, adoption gates, outbox fan-out, and render-and-diff consumers are the right conceptual direction.
- **Testable subsystems.** C protocol/client/monitor/server logic, RackWire, dashboard layout/lifecycle behavior, alertbridge dispositions, UniFi parsing, and status-panel host code all have executable checks.

The problem is not that the project lacks good ideas. It is that system-level policy and failure semantics are weaker than the individual modules suggest.

## 4. Release blockers

### F-01 · Node certificates are not bound to frame identity — P0

**[Baseline observed in reviewed commit `586cac1`]** `serverIngestValidate()` derived a role from the peer certificate CN but did not bind that certificate to `sourceNodeId`. Dispatch then used the frame-supplied ID for telemetry, liveness, topology, survey, and control-result writes. HELLO persisted the header ID plus a self-declared FQDN and role. Monitor gossip accepted `sourceNodeId` into HRW membership without certificate-to-node binding.

**Impact.** A compromised enrolled client can impersonate another client, keep it falsely alive, or falsify its health/topology/convergence. A gossip peer can add invented monitor IDs that win HRW ownership, causing real monitors to relinquish targets to nonexistent owners.

**[Candidate status, observed 2026-10-20]** The enrollment script's CSR CN now uses the dotted `<role>.<fqdn>` form consumed by server role parsing; an executable CSR test checks both CN and DNS SAN. This closes the enrollment-format mismatch only. Certificate-to-node/FQDN binding, certificate fingerprint persistence, stale-certificate handling, and authenticated monitor membership remain open.

**[Candidate status, verified 2026-10-23]** The current server validates every ingested frame's `sourceNodeId` against the enrolled node row and exact registered certificate CN using `serverDbNodeCertMatches()`. HELLO role/FQDN must also match the authenticated CN. Monitor peers bind their deterministic node ID and monitor role to the verified peer CN. `test_server_ingest` and `test_peers` both pass. This closes the frame-ID/CN binding gap for the currently registered identity; it does not identify the exact presented leaf certificate, enforce the stored leaf fingerprint, or establish signed monitor membership. Certificate revocation distribution, activation/freshness, and fleet acknowledgements also remain open, so F-01 remains a release blocker.

**[Revocation lifecycle update, candidate verified 2026-10-23]** The CRL generator, issuer-scoped static publisher, and peer activation updater now exist with passing local OpenSSL-backed tests and systemd unit checks. The candidate publisher exposes CRLs for static hosting under HTTPS but no TLS vhost or endpoint is deployed. Missing-CRL rollout policy, fleet acknowledgement/convergence, and exact leaf-certificate enforcement remain open; F-01 remains a release blocker.

**Recommendation.** Define one canonical identity contract and enforce it on every ingress path: certificate SAN/CN, immutable node ID, role, and registered FQDN must agree. Persist certificate fingerprint/serial against the node. Reject HELLO and gossip mismatches. Add negative tests for cross-node, cross-role, stale-cert, wrong-CA, and wrong-name cases. A server-signed monitor-membership epoch is preferable to trusting peer advertisements as membership authority.

### F-02 · Dashboard authorization is systemically incomplete — P0

**[Observed]** The global API gate requires only a valid session (`dashboard/api/index.php:40-49`). Only `Operator::requireOperator()` checks a write-capable role (`dashboard/api/lib/Operator.php:44-62`), but at least thirteen mutation handlers omit it. A viewer can invoke active scan/adopt/ignore (`routes/discovery_mut.php:20-109`), enrollment rejection (`routes/enrollments_mut.php:35-45`), provision/survey (`routes/control.php:20-45,133-142`), global and per-node configuration, rule edits (`routes/config.php:34-162`), pool create/update (`routes/pools.php:43-74`), and asset patch/SoR mirror (`routes/assets.php:77-116`). The C bridge only checks that `op=` is nonempty (`src/server/solariCtl.c:389-402`).

**Impact.** The documented viewer role is not read-only. UI hiding gives a false sense of enforcement; direct requests can alter monitoring behavior and authoritative inventory.

**Recommendation.** Make non-GET authorization default-deny in one server-side policy layer. Declare route metadata for required role and service principal; require operator/admin for every human mutation unless an explicit reviewed exception exists. The C side must independently authorize verbs or accept a verifiable authorization envelope rather than a free-form name. Add a generated route × method × role test matrix to CI.

**[Candidate status, observed 2026-10-21]** The API front controller now installs a
default-deny policy guard before dispatch. The current route audit resolves all
139 routes and finds every write operator/admin-gated, except the explicit login,
logout, and enrollment bootstrap exceptions. Live enforcement tests reject a
viewer even with a valid CSRF token. The hardened C bridge also has a Linux
`SO_PEERCRED` class ACL and rejects direct privileged verbs from the dashboard
uid when `enforcePeer=true`; that setting is opt-in and defaults off for
compatibility. Queued privileged requests still carry the dashboard's
`requestedBy` name and depend on the PHP role gate, so C does not independently
verify the human's operator/admin role. The F-02 trust-boundary recommendation
is not fully closed. Cross-lab review and deployment remain outstanding.

### F-03 · Cookie-authenticated control requests have no CSRF defense — P0

**[Observed]** State-changing requests use the session cookie with no CSRF token or Origin enforcement. `solari_json_body()` accepts a JSON-looking body without requiring `application/json` (`dashboard/api/lib/bootstrap.php:47-62`). SameSite=Lax is the only browser-layer mitigation (`dashboard/api/lib/Auth.php:79-85`), and the vhost exposes multiple same-site aliases (`deploy/dashboard/apache-solarinet.conf:11-14`).

**Impact.** A compromised same-site origin can submit credentialed control requests using a simple `text/plain` POST containing JSON. Combined with F-02, even a viewer session can become a fleet-write capability.

**Recommendation.** Require a session-bound synchronizer token and strict Origin/Referer validation on every state-changing method. Require the expected Content-Type. Keep SameSite as defense in depth, not as the authority mechanism.

**[Candidate status, observed 2026-10-21]** `Policy::enforce()` now requires a
same-origin Origin/Referer and session-bound CSRF token on state-changing
requests. `solari_json_body()` accepts only the exact `application/json` media
type (with optional parameters); regression coverage rejects lookalikes such as
`application/jsonp`. A PHP HTTP-SAPI test exercises `php://input`, accepts a
valid JSON request, and verifies lookalikes receive HTTP 415. Default-deny
policy, guard dispatch, nine live allow/deny scenarios, and the full route audit
pass locally. This is candidate evidence; hosted CI, cross-lab review, and
deployed browser verification remain open.

### F-04 · The PHP/C “trust boundary” is not isolated — P0

**[Historical baseline; candidate state]** The legacy root-level PHP-FPM/server templates still run as `jason` and serve code from the checkout. The candidate now includes a hardened UID split, immutable `/opt/solarinet` release paths, socket `SO_PEERCRED` ACLs, a durable privileged-request queue, scoped dashboard DB grants, and a disposable end-to-end boundary harness. `deploy/dashboard/hardened/CONTRACT-AND-REVIEW.md` records passing C/DB/build and boundary checks plus cross-lab review. These changes have not been applied to Xenon; the deployed boundary is therefore **unverified** and F-04 remains open until the full host cutover passes.

**Impact.** “PHP holds no CA material” is a code convention, not a privilege boundary. Compromised PHP code inherits the operator account’s filesystem, repository, socket, and potentially private-key reach. Serving API code from a writable checkout also turns source write access into persistent production code execution.

**Recommendation.** Keep the hardened candidate and its release package as the only supported cutover path. F-04 closes only after Xenon is migrated to the separate service identities, immutable package paths, enforced peer ACL and queue, scoped grants, and the verifier passes against the running host. Do not infer deployment from candidate tests.

### F-05 · A viewer-triggerable scan can starve the leadership lease — P0

**[Observed]** `/api/discovery/scan` has no operator gate (`dashboard/api/routes/discovery_mut.php:20-38`). `DISCOVER` executes `serverScanRun()` synchronously inside solariCtl (`src/server/solariCtl.c:455-466`). solariCtl is polled by the same single thread that renews the lease and drains ingest (`src/server/main.c:292-313`). A maximum scan covers up to 4,095 computed addresses × 31 default ports in 256-job batches with a 300 ms batch deadline (`src/server/serverScan.c:40-50,410-508`), giving roughly 149 seconds of timeout batches before reverse lookup/enrichment in the worst case. The default lease TTL is much shorter.

**Impact.** An authenticated viewer can block fleet ingest, control replies, and lease renewal long enough for another server to claim leadership while the old process still owns listeners. This is both an availability flaw and an HA correctness flaw.

**Recommendation.** Move discovery, provisioning, deployment, and every other long-running operation to a bounded job runner. The server loop should enqueue, return a job ID, and remain deadline-driven. Accepted Unix clients must be nonblocking with read deadlines; today a connect-and-stall can also block the loop (`src/server/solariCtl.c:1120-1147`). Add a test that continuously renews the lease while slow/malformed operator connections and maximum scans run.

**[Candidate status, observed 2026-10-21]** Active discovery has moved to a
bounded durable worker with lease-fenced writes, persisted progress, requester-
scoped status, and the RackWire admission path. The real worker/API/SoR browser
flow, directly connected `/24`, cancellation, takeover, and lease-renewal tests
pass against disposable local databases. This closes the synchronous scan
starvation path in the candidate. Provision/deploy operations, slow or malformed
Unix control clients, systemd-managed stop behavior, cross-lab review, and
production deployment remain unverified; F-05 as a whole stays open. Detailed
acceptance evidence is in [the scan-worker contract](rework/F05-scan-worker-contract.md).

### F-06 · Tab5 approval display is not cryptographically bound to approval — P0 when enabled

**[Observed at baseline review]** Authbroker sent unsigned approval requests and the device response did not bind the displayed fields. The local candidate now signs the complete v2 request, verifies it before display, binds the response to the canonical request digest, requires TLS/CA configuration, and imports broker trust by USB serial. Twenty broker unit tests and the C++ host canonical-vector check pass. Full MQTT round-trip, pinned-core firmware build, and hardware acceptance have not been run.

**Impact.** A LAN/MQTT-path attacker can preserve a legitimate ID and nonce while changing the displayed action to something benign; the signed approval then authorizes the broker’s original sensitive request.

**Recommendation.** Keep Tab5 explicitly non-production until the remaining acceptance gates pass. The selected protocol and USB key-provisioning contract, current evidence, and release gates are in [the F-06 approval-binding contract](rework/F06-approval-binding-contract.md).

### F-07 · PXE provisioning executes unauthenticated artifacts as root — P0 when enabled

**[Observed]** Provisioning defaults to HTTP (`deploy/fleet/fleet-lib.sh:14-19`). Debian/Ubuntu unattended installers fetch and execute firstboot over HTTP as root; firstboot downloads and installs the client without a signature or digest (`deploy/fleet/netboot/configs/debian.preseed.tmpl:110-130`; `ubuntu.autoinstall.tmpl:104-117`; `deploy/fleet/netboot/installers/solari-firstboot.sh:58-101`). Per-MAC disk-wipe instructions are also served over HTTP (`deploy/fleet/netboot/ipxe/mac-profile.ipxe.tmpl:29-75`).

**Impact.** Provisioning-network tampering yields root on new machines or can alter disk targets and persistent credentials.

**[Candidate status]** The [F-07 signed-provisioning contract](rework/F07-signed-provisioning-contract.md) defines the trust chain and acceptance gates. A fail-closed guard blocks the five current provisioning entry points by default. A local CMS signer/verifier stages read-only release trees with a canonical manifest binding paths, sizes, executable modes, hashes, target, validity, and a pinned signer; offline tests cover trust, expiry, target, path, read-only modes, and tampering. It is not wired into iPXE, installer configs, firstboot, or release activation, and the isolated-lab override still permits unsafe provisioning. The end-to-end finding remains open.

**Recommendation.** Keep PXE and firstboot provisioning isolated and disabled until the contract is implemented end to end. The trust root must authenticate the initial loader as well as every later script, kernel, initrd, installer config, and binary; a manifest plus checksum served over the existing HTTP channel is insufficient.

## 5. High-priority correctness and security findings

### F-08 · SoR CDC can permanently lose events — P1

**[Observed at baseline]** `sor_emitd` advanced its DB checkpoint after ordinary `basic_publish()` without publisher confirms; consumers acknowledged before applying and lacked periodic reconciliation.

**Impact.** Broker or target failures can leave downstream state indefinitely stale while the pipeline appears healthy. The documented “at-least-once” property is false.

**[Candidate status]** The outbox emitter now enables publisher confirms and mandatory routing before checkpoint advancement; DNS, Pi-hole, and AD consumers acknowledge only after successful apply and run periodic full reconciliation. Local boundary tests cover broker failure/replay, delayed ack, failed and partial applies, periodic reconcile, and loss during in-flight apply (11 passed). Disposable RabbitMQ and full DNS-chain integration tests exist, but deployed daemon restart and multi-target recovery remain unverified. See [SoR sync status](../deploy/sorsync/README.md).

**Recommendation.** Preserve the confirm/mandatory, apply-before-ack, and periodic-reconcile invariants. Complete the isolated-broker failure drills and deployed restart/multi-target verification before closing F-08.

### F-09 · Server HA semantics are internally inconsistent — P1

**[Observed]** The operator bridge is opened and polled even on standby servers (`src/server/main.c:387-395,312-313`), allowing standby writes. Losing standby lease claims increment `leaseEpoch` because the UPSERT changes epoch when the proposed holder differs without first requiring expiry (`src/server/serverDb.c:1804-1814`). A bind failure is retried but the winner can continue renewing while not serving traffic.

**Impact.** Multiple servers can accept conflicting commands; config/failover epochs churn without takeover; lease ownership can say active while endpoints are unavailable.

**[Candidate status]** The standby operator bridge permits only `PING`, scoped request status, and discovery status; every other command is rejected before dispatch. Unit coverage verifies a standby cannot edit asset metadata, and a two-process test verifies standby claims do not change the active holder or epoch. Listener bind failures release the exact owned fencing token and enforce a one-TTL local retry cooldown. The process test passed against a disposable local MariaDB schema: the healthy standby took over after bind failure, remained active across the failed server's cooldown, renewed its lease during slow scan enrichment, and recovered an orphaned scan after the original worker lost its lease. The test database was dropped after the run. Production HA behavior remains unverified.

**Recommendation.** Choose and enforce one model: only the active server exposes authoritative writes, or every command transaction verifies current lease holder and epoch. Make lease acquisition, epoch increment, and endpoint activation one observable state machine. Exercise it with two real DB connections and bind-failure/failover tests.

### F-10 · Health and convergence records can lie — P1

**[Observed]** A normal MONITOR_REPORT is written once in ingest and again during reconcile (`src/server/serverIngest.c:315-323`; `serverMaster.c:283-295`), doubling history. Client/monitor control handlers advance the applied epoch before durable save and still send success when persistence fails (`src/client/clientControl.c:198-213,454-471`; `src/monitor/monitorControl.c:186-204,442-455`). Missing `TLV_ERROR_CODE` is treated as success and CONTROL_RESULT correlation is not matched to an outstanding directive (`src/server/serverControl.c:235-292`).

**Impact.** Trend data is distorted, DB load doubles, and the server can report convergence that will disappear on restart or belongs to a stale/spoofed command.

**[Candidate status]** Monitor reports are persisted once; reconciliation now updates derived health only. Client control config is staged and saved before its epoch/config become active. Monitor control now follows the same order, writes and fsyncs a unique temporary state file, atomically renames it, syncs the parent directory, and returns a failed result with the prior epoch if persistence fails. Five fault tests cover restart restoration, file/directory sync failure, rename failure, and absent state path. Server-side result handling requires verb, error, epoch, and nonzero correlation fields. Migration 034 persists each published generic directive by node/correlation/verb/target epoch; a matching result consumes it in the same transaction as the convergence update, successful config results must report the target epoch, and duplicate, stale, wrong-verb, or wrong-epoch results are rejected. Migration/export and live MariaDB coverage pass on an isolated scratch schema. Decommission acknowledgements retain their separate generation-matched lifecycle path. F-10 candidate implementation is complete; full platform integration and release remain open.

**Recommendation.** Preserve the one-write report path and durable config-before-ack behavior. Persist directives keyed by node, correlation, verb, and epoch, then consume only one exact matching pending directive before recording convergence.

### F-11 · The solariCtl wire protocol is unsafe on both ends — P1

**[Observed]** C performs one blocking 4 KiB `read()` on a stream socket, with no read-to-newline loop or overlength error (`src/server/solariCtl.c:1120-1147`). PHP performs one `fwrite()` and treats any positive partial write as success, with no request-size preflight (`dashboard/api/lib/SolariCtl.php:78-126`).

**Impact.** Fragmented or large config/control payloads can truncate, desynchronize, or be processed as complete. A stalled client can block the fleet loop as described in F-05.

**Recommendation.** Replace the line protocol with a length-prefixed bounded frame, or implement looped partial I/O, explicit newline completion, deadlines, and hard request/reply caps. Test fragmentation at every byte boundary, partial writes, oversized requests, slowloris behavior, and malformed percent encoding.

### F-12 · Schema lifecycle is not reproducible — P1

**[Observed]** There is no migration ledger/runner. Two control-plane migrations use ordinal 016. `solariServer --init-db` applies only `db/schema.sql` (`src/server/main.c:375-378`), which is an ad hoc stale hybrid rather than baseline or current state. `serverDbApplyScript()` can ignore a later multi-statement error (`src/server/serverDb.c:436-451`). Several migrations described as rerunnable contain unguarded `ADD COLUMN`. Dashboard deployment documentation applies only a hand-selected subset (`deploy/dashboard/README.md:12-24`).

**Impact.** Fresh installs, upgraded installs, test schemas, and documented schemas can differ. Partial failure may be reported as success.

**Recommendation.** Create one migrator per database with immutable unique IDs, checksums, a schema-version ledger, transactions where supported, and explicit failure reporting. Generate bootstrap snapshots from the applied migration set. Test clean install, every supported upgrade path, idempotent rerun where claimed, and downgrade/restore procedures.

### F-13 · Browser freshness mechanisms violate the honesty goal — P1

**[Historical observation, Aug 2026]** The original reviewed revision stored successful authenticated `/api/` GETs in the service worker and returned them after network failure; `loadLive()` then labeled the response `source: "live"`. SSE requests could hold php-fpm workers and had cursor/replay defects. See the historical line references in that review revision.

**[Candidate status, observed 2026-10-20]** These first three browser-boundary defects have been addressed in the current candidate: `sw.js` sends `/api/*` GETs directly to the network with `cache: no-store` and never reads/writes API cache entries; `api.jsx` marks a retained real snapshot `stale` on failed refresh and blocks mutations; `app.jsx` shows the age and pause state; `/api/stream` immediately returns 204. The service-worker, browser-adapter, and route tests pass locally. The per-endpoint resume cursor is no longer active because the route is retired.

**Impact.** Logged-out or later local users can see cached private state; stale fleet data is presented as live; a few tabs can starve the API; events can be missed or replayed.

**[Candidate status, observed 2026-10-20]** Core network-only fetch, stale snapshot labeling, mutation pause, and SSE retirement are implemented. Independently tolerated optional endpoints now carry per-source `fresh|stale|unknown` state and `lastSuccess`/`lastAttempt`; the app shows a partial-data banner when any source is stale or unknown and retains each endpoint's last successful payload through an outage. Browser-adapter coverage exercises optional endpoint outage, retained values, and recovery. Service-worker, route, and adapter tests pass locally.

**Remaining recommendation.** Complete authenticated browser acceptance across multiple tabs/sessions and verify the status banner against the deployed session and endpoint configuration. Keep SSE retired until a durable event bridge is available.

### F-14 · Authentication/session controls are incomplete — P1

**[Observed]** OIDC accepts signed ID tokens without requiring `exp`; state creation time is stored but not enforced (`dashboard/api/lib/Oidc.php:111-115,146-155,227-246`). Local login remains callable even when its UI is disabled and has no rate limiting (`dashboard/api/routes/auth.php:15-25,71-77`). Session roles are copied at login and can remain valid for the eight-hour GC lifetime without revalidation (`dashboard/api/lib/Auth.php:187-219`; `deploy/dashboard/php-fpm-solarinet.conf:40-41`).

**Impact.** Missing-expiry tokens can be accepted, abandoned auth transactions do not expire, credential guessing/CPU denial is unbounded, and revoked roles persist.

**Recommendation.** Require the standard OIDC claims and bounded state age; use a maintained OIDC/JWT implementation when compatible with project policy. Independently disable the break-glass endpoint when unused, rate-limit by account and source, add idle/absolute session expiry, and revalidate role/revocation.

**[Candidate status, observed 2026-10-23]** OIDC ID-token validation requires
issuer, audience, subject, `exp`, `iat`, and nonce, and auth transactions have a
bounded one-shot state. Local password login is opt-in and requires a private
rate-limit directory; failed attempts are capped per account and source. Idle
and absolute session limits apply, local sessions reload roles, and disabling
local login invalidates existing local sessions. OIDC sessions now refresh
group-derived roles through HTTPS UserInfo every 60 seconds, bind the returned
subject to the original identity, refresh access tokens, and downgrade to viewer
when the provider is unavailable. Discovery issuer identity is matched and
metadata endpoints are restricted to the issuer origin or explicitly configured
HTTPS origins, and OIDC response bodies are limited to 1 MiB. Subject mismatch
revokes the session. Focused claim, session, and TLS-backed UserInfo tests pass.
**Open:** deployed provider
group claims, refresh-token behavior, and configuration remain unverified.

### F-15 · Opie’s “read-only” model is not confidentiality-safe — P1

**[Observed]** Alert DB text is interpolated into the model prompt, which receives generic Read/Grep/Glob tools plus a diagnostic wrapper (`deploy/opie/opied.py:186-252`). The service runs as `jason` with the operator home/CLI authentication (`deploy/opie/opied.service:10-16`). The probe accepts broad `10.*` targets, auto-suffixes hostnames, accepts new SSH keys, and exposes logs, process arguments, unit config, and SMART output (`deploy/opie/opie-probe.sh:18-57`).

**Impact.** Prompt injection can cause disclosure of local or fleet secrets into reports/notifications even if direct writes are unavailable.

**Recommendation.** Run under a dedicated sandboxed account with no operator home. Remove generic file tools. Resolve targets from an inventory allowlist, pin SSH host keys, expose structured narrowly redacted probes, and treat all alert/probe text as untrusted data.

### F-16 · Fleet provisioning is both incomplete and leaves key material staged — P1

**[Observed]** `fleet-provision.sh` mints/stages a node private key, certificate, CA, and token (`deploy/fleet/fleet-provision.sh:75-119`), but firstboot never consumes them and writes `PENDING_ENROLLMENT` for manual completion (`deploy/fleet/netboot/installers/solari-firstboot.sh:185-223`). The staging/profile cleanup is not completed automatically.

**Impact.** The advertised one-button path does not reach a reporting client; private key material remains on the provisioning server; reboot can repeat unattended installation.

**Recommendation.** Generate private keys on nodes and redeem a short-lived one-time token for CSR signing. Remove server-minted key staging. On success atomically delete token material and the per-MAC boot profile. Prove PXE-to-first-report and failure recovery in disposable VMs.

### F-17 · DNS and AD application paths lack safe deployment semantics — P1

**[Observed]** Zones are overwritten directly and serial monotonicity depends on the previous generated file (`netdb/gen-zones.py:47-67,130-146`). The DNS applier copies files individually and reloads without `named-checkzone`/`named-checkconf` or rollback (`deploy/sorsync/sor_apply_dnsd.py:72-104`). AD tooling passes `administrator%password` in process arguments (`deploy/sorsync/sor_apply_ad.py:48-60`; `netdb/sync-net-to-org.py:7-25`).

**Impact.** Partial/malformed releases can take DNS down or stop secondary transfer; a domain-wide credential is exposed to process inspection.

**Recommendation.** Render and validate a complete temporary DNS release, atomically switch it, health-check primary/secondary serials, and roll back on failure. Store serial state durably. Use Kerberos or a protected credential channel and a least-privilege DNS identity for AD.

### F-18 · Arbitrary MCP SQL is not safely constrained — P1

**[Observed]** The guard is a regex over selected SQL tokens rather than a MariaDB grammar/AST and recognizes only simple identifiers immediately after FROM/JOIN (`integrations/mcp/solarinet_mcp/db.py:29-71,115-131`). It does not establish a complete policy for parenthesized table expressions or SELECT functions, and the expected read-only grants are not verified in code.

**Impact.** An LLM-facing query can reach unadvertised tables, consume excessive resources, or read DB-host files if grants permit.

**Recommendation.** Prefer fixed tools/views over arbitrary SQL. Otherwise use a real MariaDB parser with a narrow AST allowlist, a dedicated per-view account, no FILE/routine privileges, read-only transactions, statement timeout, row/byte caps, and concurrency limits.

**2026-10-23 implementation update.** The MCP `DbClient` now accepts only three enum-selected statements (node summary, active alerts, host history); caller-supplied SQL and identifiers have been removed. Host-history values are bound and checked against the `BIGINT UNSIGNED` range. Query and result caps have direct tests. The local MCP suite passed 48 tests. This closes the arbitrary SQL path in application code. **UNVERIFIED:** the deployment's MariaDB account grants and server-side resource/concurrency limits have not been checked, so the broader least-privilege and resource-control recommendation remains open.

## 6. Structural findings to address next

| ID | Priority | Finding | Recommended direction |
|---|---:|---|---|
| F-19 | P2 | Monitor probing is sequential and capped at 64 targets; a 30-second round can overrun by minutes, delaying gossip and control (`src/monitor/main.c:54-86,156-180`). | Bounded concurrency, deadline-based rounds, explicit skipped/overrun telemetry, and a 300-host load test. |
| F-20 | P2 | HRW is coded but real multi-monitor membership, identical views, churn, and partition behavior are not proven. The server/DB remains a central authority despite broad “coordinator-free” wording. | Narrow the claim to monitor ownership; run multi-monitor chaos/failure tests before relying on k-of-n. |
| F-21 | P2 | Spool storage has no quota/age policy, deletes on transport acceptance rather than durable server ack, and reuses sequence numbers after restart. | Quotas and health metrics, boot/session identity, persisted sequence state, and application acknowledgement where durability is required. |
| F-22 | P2 | Direct SoR commands perform several multi-step authoritative mutations without transactions; per-node epoch allocation occurs in concurrent PHP workers. | Put domain commands and epoch allocation inside authoritative DB/server transactions with idempotency keys and constraints. |
| F-23 | P2 | Server confirmation persists a single-use, expiring challenge and publishes the wipe directive only after matching token consumption. The directive now includes its target node ID. There is no client receiver, durable delivery retry, or node acknowledgement; the queued control request also retains the raw confirmation token in its args/result. | Add an authenticated node receiver, durable outbound state and acknowledgements, and redact sensitive queue material after the confirmation exchange. |
| F-24 | P2 | Tab5 secret-at-rest claims depend on uncommitted secure-boot/flash-encryption state; approval publish requests QoS 1 but the library sends QoS 0. | Keep non-production; commit reproducible security configuration and use a real QoS-1/persistent decision path. |
| F-25 | P2 | UniFi can leave missing/failed polls falsely green and TLS verification is opt-in. | Explicit freshness/unknown state, integration-health telemetry, and pinned verification by default. |
| F-26 | P2 | Authbroker fails open when its token is empty and accepts unbounded body/TTL/thread use. | Refuse startup, cap all resources, rate-limit, and prefer a Unix socket. |
| F-27 | P2 | Backups are accepted when nonempty; no restore drill, integrity catalog, or off-host recovery is proven. | Compression/checksum validation, scheduled disposable restore, verification queries, encrypted off-host copy, and documented drills. |
| F-28 | P2 | Referential integrity is sparse; lifecycle/retention can leave orphan current/config/topology rows. | Add FKs where feasible or transactional cleanup procedures with orphan-count tests. |
| F-29 | P2 | Fleet mTLS is configurable rather than invariant and transport tests are plaintext. | Make plaintext test-only and add real certificate/identity integration tests. |
| F-30 | P3 | Full builds emit warning noise; protocol reserved bits/flags/count invariants are not strictly validated. | Warning-clean CI, ASAN/UBSAN, fuzzing, and strict envelope validation while retaining unknown-TLV compatibility. |
| F-31 | P3 | The deployed Apache policy remains incomplete; in-browser Babel makes a strict CSP difficult. | Candidate config now defines CSP/HSTS/referrer/permissions headers and static tests cover the policy, including same-origin RackWire camera. Candidate-only: full vhost syntax and deployed response behavior remain unverified because an unrelated enabled vhost references a missing Let's Encrypt certificate. |

## 7. What is missing architecturally

SolariNet does not primarily need more features. It needs explicit, executable definitions of these cross-cutting contracts:

1. **Threat model and trust-zone map.** Enumerate human principals, service accounts, node identities, CA/signing authority, browser origins, message brokers, provisioning networks, and physical-device attackers.
2. **One authorization matrix.** Every API route, solariCtl verb, SCP message, MQTT topic, and database role needs a named principal, allowed actions, and enforcement point. Generate tests from it.
3. **Command lifecycle.** A durable command needs an idempotency key, authorization record, target, epoch/version, delivery state, correlated acknowledgement, failure state, and audit record. Current one-shot broadcasts and ad hoc confirm tokens are insufficient.
4. **Durability semantics.** Define where “accepted,” “persisted,” “published,” “applied,” and “converged” differ. Make the distinctions visible in schemas, APIs, UI, and alerts.
5. **Schema/release lifecycle.** One migration tool per DB, immutable artifacts, promotion/rollback, and a release manifest tying binary/config/schema versions together.
6. **Background-job boundary.** Scans, deploys, provisioning, DNS renders, SNMP walks, and AI diagnostics must not execute inside request or lease-renewal loops.
7. **Freshness and integration health.** Every external source/render target needs last-success, age, stale/unknown behavior, backlog, and reconciliation status. Cached data must never masquerade as live.
8. **Recovery evidence.** Restore drills, broker-loss tests, split-brain tests, CA/key recovery, database promotion, DNS rollback, and firmware rollback must be executable and scheduled.
9. **Capability maturity model.** Mark each feature prototype, lab-only, production-ready, or production-authoritative. Tab5, PXE, Opie, LDAP, MCP SQL, remote CA, and monitor mesh currently blur these states.

## 8. Recommended workflow

### Phase 0 — Contain unsafe capabilities

1. Disable Tab5 approval authority and root PXE flows until F-06/F-07 are fixed.
2. Remove arbitrary SQL and generic file tools from LLM-facing integrations.
3. Make every human mutation operator/admin-only; add CSRF and Origin checks.
4. **Candidate done:** stop API service-worker caching and retire SSE; remaining work is the per-source freshness contract and authenticated browser acceptance.
5. Restrict dashboard exposure and local login while the boundary is being rebuilt.

**Exit gate:** automated route/role/CSRF tests pass; unsafe prototype features cannot be enabled accidentally.

### Phase 1 — Establish real trust boundaries

1. Split service accounts and deploy immutable artifacts.
2. Bind certificate identity to node ID/FQDN/role on SCP and gossip.
3. Add explicit socket permissions, peer credentials, framed I/O, and timeouts.
4. Isolate the CA signer; replace free-form operator attribution with verifiable authorization.
5. Harden OIDC/local sessions and secret transport.

**Exit gate:** negative identity/authorization suites prove that the wrong user, process, certificate, role, node ID, origin, or stale token cannot act.

### Phase 2 — Make state transitions durable

1. Implement the migration ledgers and verified bootstrap path.
2. Fix outbox confirms/consumer acknowledgement and add periodic reconciliation.
3. Build a durable command/result state machine with idempotency and atomic epochs.
4. Remove duplicate report persistence and false config acknowledgement.
5. Put multi-step SoR writes inside transactions.

**Exit gate:** crash/fault tests prove no silent event loss, false convergence, partial domain mutation, or schema drift.

### Phase 3 — Decouple and prove operations

1. Add a bounded background-job subsystem.
2. Make discovery/provision/deploy/DNS/AI work asynchronous and observable.
3. Complete node-generated enrollment and signed provisioning artifacts.
4. Add restore, DNS rollback, broker-loss, and two-server failover drills.

**Exit gate:** the control server renews leases and ingests telemetry throughout worst-case jobs and induced downstream failures.

### Phase 4 — Prove the intended architecture

1. Deploy at least three monitors in a lab topology.
2. Test HRW convergence under join, loss, partition, stale membership, forged membership, and uneven target latency.
3. Run capacity tests at the stated 300-host ceiling.
4. Establish SLOs for sample freshness, alert latency, command convergence, outbox lag, DNS convergence, and recovery time.

**Exit gate:** measured results support the resilience, honesty, frugality, and portability claims.

Only after these gates should feature work such as first-class topology, broader probes, or additional device integrations resume.

## 9. CI and review gates to add

The current CI is a useful compile gate but not a release gate. MCP, alertbridge,
UniFi, sorsync, and dashboard authorization tests now run in CI; remaining additions:

- Set `SOLARI_TEST_DB=1` against the existing scratch MariaDB; fail if the live DB suite skips.
- Make TLS loopback required and test certificate identity failures.
- Run every dashboard test, not only the bridge and RackWire subset.
- Lock Python dependencies. Build a local broker fixture before running the authbroker loopback test (it currently defaults to the live broker). Add backup, provisioning, and notification suites when those tests exist.
- Run status-panel host tests under ASAN/UBSAN and compile Tab5 in its supported toolchain.
- Add shellcheck, static analysis, C sanitizers, protocol fuzzers, PHP security/route-matrix checks, migration clean-install/upgrade checks, and DNS validators.
- Add RabbitMQ failure injection, two-server MariaDB lease races, partial Unix-socket I/O, concurrent config writes, backup restore, and service-worker offline browser tests.
- Pin actions, runners, toolchains, and dependencies sufficiently to reproduce release artifacts.

## 10. Corrections to the supplied dossier

The dossier is valuable, but these claims should be corrected in its next revision:

- RackWire migration 020 **is present** at `netdb/sor/migrations/020_rackwire.sql`; it correctly belongs to the SoR migration tree, not `db/migrations`. CI applies it (`.github/workflows/ci.yml:112-123`).
- No compiled test binaries are tracked in the current git index.
- Deploy Python is not wholly untested: alertbridge, UniFi, MCP, and sorsync suites now run in CI. Several services remain uncovered; the authbroker smoke test still targets a live broker by default.
- The duplicate `surveyUrl`/`controlUrl` default does not currently cause two binds: `surveyUrl` is unused. It is dead/misleading configuration, not a demonstrated bind conflict.
- The lease bind-failure path retries; the defect is a renewable lease-held outage, not a permanent no-reconciliation orphan.
- Current code does not confirm `node.state` being overloaded with active/standby server role.
- `db/schema.sql` is not simply migration 001; it is a stale hybrid containing selected later changes.
- “All sockets use mTLS” is too strong: plaintext endpoints remain configurable and are what transport integration tests exercise.
- The monitor CONTROL receivers reject unknown verbs 10–255, although the send-side range remains misleadingly broad.
- The dossier understates dashboard authorization gaps: the problem affects a broad family of write routes, not only provision, survey, and enrollment rejection.
- Status-panel unknown command handling is intentionally terminally rejected by firmware and covered by tests; observability of rejected versus applied remains the concern.
- SSE does not reliably provide duplicate-free resume, and its FPM occupancy can exhaust the configured worker pool.

## 11. Verification performed

Observed checks during this review:

- Default C build: configured, compiled, and **13/13 tests passed**.
- Full C build with I/O, SQLite, JSON, server, system nng/mbedTLS/MariaDB: compiled; CTest reported **30/30 non-failing registrations**. Of these, 29 executed and passed.
- `test_server_db_live`, the thirtieth registration, was run directly and reported **SKIP** because `SOLARI_TEST_DB` was unset; CTest records that zero-exit skip as a pass.
- PHP syntax: **50 files checked, 0 failures**.
- Dashboard PHP/JS tests invoked in the checkout passed, including solariCtl, RackWire API/UI, JSX parsing, layout, lifecycle, panel gear, and panel renderer suites.
- Alertbridge: **5 tests passed**. UniFi: **3 tests passed**.
- Status-panel firmware host suites and daemon codec suite passed in the specialist pass.
- Shell syntax: **20 tracked shell scripts checked, 0 failures**.
- Combined Python pytest collection was **not reproducible** in the base workspace because `paho` and `pydantic` were absent; no packages were installed.

### Unverified

- Live MariaDB migration, grant, replication, and concurrent lease behavior.
- Deployed systemd units, socket/directory permissions, service accounts, and actual TLS configuration.
- Live RabbitMQ confirms/redelivery, DNS/AD apply and rollback, PXE lifecycle, or backup restoration.
- Authenticated browser freshness behavior under multiple sessions/tabs; service-worker privacy, stale adapter behavior, and 204 stream retirement have local automated coverage only.
- Multi-monitor HRW behavior, 300-host capacity, long outage spooling, or network partitions.
- Tab5 build, flash security, hardware cryptographic flow, MQTT tamper, and firmware update/rollback.
- Opie prompt-injection exploitability and live secret exposure.

## 12. Final recommendation

SolariNet should proceed, but as a **stabilization program, not a feature program**. Preserve the C/TLV core, provenance-oriented SoR, and quiet-healthy interface. Rebuild the system around enforceable identity, centralized authorization policy, durable command/event semantics, isolated service accounts, a real migration/release lifecycle, and fault-based acceptance tests.

The project’s stated first principle is honesty. The next release should apply that principle to the control plane itself: a successful response must mean authorized, durably persisted, correctly delivered, actually applied, and currently fresh—not merely that a socket accepted a message.
