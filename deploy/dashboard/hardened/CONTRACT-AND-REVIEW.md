# Task #1 — PHP→host security boundary (boundary + request queue)

Build as reviewable files; NOTHING deployed to xenon. Mirror Phase-0 pattern
(pure decision core + thin effect shim + tests + config).

## Verified current state (the hole)
- `solariCtl.c:1043-1063` binds the AF_UNIX socket, **no fchmod / no group** →
  world-connectable per umask; `accept(fd,NULL,NULL)` at 1128 **discards the
  peer — no SO_PEERCRED**.
- `ctlCheckRbac` (392-403): destructive verbs only require `op=` NON-EMPTY. No
  proof the caller is that operator.
- PHP pool `user=jason` (php-fpm-solarinet.conf:16) == dev server `User=jason`
  (solarinet-server.service:22) → **same uid**; PHP reads CA key/auth file
  directly, socket irrelevant. (Prod systemd server = `solari`, but prod PHP
  pool still `jason`.)

## Design (4 parts)
1. **Separate PHP uid** (config): dedicated `www-solari` user, member of group
   `solari-ctl`. Cannot read CA key / auth file / server runtime. New/edited:
   php-fpm pool user+group; a `solari-ctl` group; file-ownership notes; tmpfiles.
2. **SO_PEERCRED on accept** (C, solariCtl.c): after accept(), getsockopt
   SO_PEERCRED → (uid,gid,pid). Pure decision `ctlPeerClass(peerUid,cfg)` →
   {OPERATOR(solari uid), DASHBOARD(www-solari uid), UNKNOWN}. Plus fchmod 0660
   + chown group solari-ctl on the socket after bind (defence in depth).
3. **Verb-class ACL keyed on peer class** (C, pure + unit-tested):
   `ctlVerbPrivClass(verb)` → {PUBLIC, ENQUEUE, PRIVILEGED};
   `ctlPeerMayInvoke(peerClass, verbClass)`:
     - PRIVILEGED (SIGN/DEPLOY/FLEET_PROVISION/FLEET_IMAGE/RETIRE/DECOMMISSION/
       ASSET_*/TARGET_REMOVE/LIFECYCLE_SET/POOL_DEL/RULE_DEL) → OPERATOR only.
       **DASHBOARD (PHP) is REFUSED even with op=.**
     - ENQUEUE (new REQUEST_SUBMIT + REQUEST_GET) → DASHBOARD or OPERATOR.
     - PUBLIC (PING, read-only) → any known peer.
   Enforced BEFORE ctlCheckRbac.
4. **Request queue** (monitoring-DB migration 020 + in-process consumer):
   - `ctlRequest` table: id, verb, argsWire, requestedBy (operator str from PHP
     session), peerUid, state ENUM(pending/claimed/done/failed), attempts,
     claimedBy, result, error, idempotencyKey (UNIQUE→NULLs allowed),
     createdAt(6)/claimedAt(6)/completedAt(6)/updatedAt. State machine.
   - PHP → socket `REQUEST_SUBMIT verb=SIGN op=<user> args=<pct-enc wire> [idem=]`
     (peer-cred: DASHBOARD ok) → C validates inner verb is PRIVILEGED, inserts a
     `pending` row, returns `request=<id> status=pending`. PHP never executes the
     privileged verb. `REQUEST_GET request=<id>` polls state/result.
     PHP side: SolariCtl::enqueue()/poll() (dashboard/api/lib/SolariCtl.php).
   - **Consumer = solariServer's own main loop** (runs as `solari`, sole CA-key
     holder): serverCtlQueuePoll(ctl) beside serverCtlPoll at main.c, drained up
     to SERVER_CTL_QUEUE_BATCH per tick, ONLY on SRV_ACTIVE. Atomically claims
     the oldest `pending` row (UPDATE ... state='claimed' WHERE state='pending'
     ORDER BY id LIMIT 1 → affected_rows CAS), replays "<verb> <argsWire>" via
     ctlHandleLine with OPERATOR authority in-process, writes done/failed.
   - **DEVIATION 1 — in-process consumer (vs literal "separate process"):** an
     in-process poll in the already-privileged solariServer is MORE secure than a
     separate worker — a separate worker would ALSO need the CA key → two holders.
     One privileged process = one CA-key holder. "Separate from PHP, runs as
     solari" is satisfied. Flagged.
   - **DEVIATION 2 — queue in monitoring DB (vs literal "SoR table"):** the C
     bridge (writer) and the in-process consumer both use the server's serverDb
     handle, which connects to `dbName` (solarinet), NOT the SoR on cesium.
     Co-locating the queue with its single consumer avoids a cross-host
     connection; the SoR stays the CMDB source of truth, this is operational
     state owned by the server. Home = db/migrations/020 + db/schema.sql. Flagged.

## Acceptance (observable) — STATUS
- Unit (test_server_ctl.c, Unity): ALLOWLIST verb-class map (exhaustive ordinary
  set; formerly-destructive + APPROVE/PROVISION/CONFIG_SET/CONTROL/REJECT/DEPLOY/
  FLEET_* PRIVILEGED; unknown/empty verb default-deny → PRIVILEGED); ctlClassifyPeer;
  ctlPeerMayInvoke truth table; DASHBOARD refused every PRIVILEGED verb; UNKNOWN
  refused all. ✅ PASS.
- DB (test_server_db_live.c, gated on SOLARI_TEST_DB): submit, status read,
  race-free claim (LAST_INSERT_ID, FIFO), completion routing (result vs error),
  F8 settled-row 2nd-complete → ERR_TLV_END, F9 claim returns per-row requestedBy,
  F10 same key from a different requester does NOT collapse (composite unique),
  empty-queue + unknown-id ERR_TLV_END. ✅ VERIFIED against real MariaDB 11.8
  (throwaway instance, full migration chain incl. F7/F10 020 applied) — PASS.
- Build: cmake build green (solariServer + all tests); ctest 25/25 PASS.
- Config (deploy/dashboard/hardened/): sysusers (solari/www-solari/solari-ctl),
  hardened php-fpm pool (www-solari + open_basedir), hardened server.service
  (solari + sandbox), server.conf [ctl] fragment, tmpfiles, PERMISSIONS.md
  matrix, verify-boundary.sh (asserts www-solari cannot read CA key + socket ACL).
- **End-to-end (verify-boundary.sh against a live enforcing server): ✅ PASS.**
  Exercised on a fully-disposable local instance — throwaway MariaDB (full
  migration chain) + throwaway `solari`/`www-solari`/`solari-ctl` uids + a real
  `solariServer --config` running as `solari` with `enforcePeer=true`,
  `dashboardUid=www-solari`, `socketGid=solari-ctl`; all users/DB/dirs removed on
  teardown. Observed, from the www-solari uid: CA key + server.env UNREADABLE;
  socket 0660 solari:solari-ctl and CONNECTABLE via group; **PING allowed**;
  **RETIRE refused (`ERR -40 privileged verb must be queued via REQUEST_SUBMIT`)**;
  REQUEST_GET reachable. Nothing touched cesium/xenon.
  - **verify-boundary.sh fix found during this run:** the layer-2 reachability
    sub-check used `test -r && test -w` on the socket path, but `access(2)`
    disagrees with `connect(2)` on a socket inode — it reported no r/w for a
    group member whose `connect()` succeeded (false FAIL that would block a real
    cutover). Replaced with an actual `connect()` probe (socat PING).
- Route rewiring (F9): control.php's six privileged routes now enqueue via
  `SolariCtl::callQueued()` (enqueue → poll-to-completion within the request,
  preserving each route's existing JSON response shape so the browser UI is
  unchanged); DECOMMISSION runs BOTH token steps through the queue; op= is bound
  server-side, never passed in args. `php -l` clean on control.php + SolariCtl.php.
- Backward compat: enforcePeer defaults false → un-migrated host unchanged; the
  enqueue path also works with enforcement OFF (one code path, no PHP branching).
- Cross-lab review (GPT-5.5/codex): COMPLETE — 12 findings, all dispositioned
  (table below). Fixes applied; re-verified: ctest 25/25 PASS, live DB suite PASS
  against MariaDB 11.8 with the full migration chain incl. the F7/F10 schema
  changes. Nothing deployed.
- Cross-lab review ROUND 2 (rewire + verify layer-2): COMPLETE — 5 findings
  (R1–R5 below), all dispositioned; fixes applied to control.php +
  verify-boundary.sh; re-verified via the disposable boundary harness → PASS.

## Phase-3 review disposition (cross-lab: opus-4.8 authored → GPT-5.5/codex reviewed)

Every finding is fixed, accepted-with-reason, or deferred-with-owner. Code fixes
carry an inline `Fxx` reference; config/doc fixes live in the hardened/ set. No
Rev-3 design doc — findings are carried as tracked code/schema/doc work per steer.

| # | Finding | Disposition | Where |
|---|---------|-------------|-------|
| F1 | Verb ACL was a **denylist** — an unknown/new verb defaulted to ORDINARY and was reachable by PHP | **FIXED** — allowlist; `ctlVerbPrivClass` default-denies (unknown → PRIVILEGED) | solariCtl.c `ctlVerbIsDashboardOrdinary`/`ctlVerbPrivClass`; unit test |
| F2 | No per-request second-human **operator-approval gate** between enqueue and execute | **ACCEPTED (Jason) + deferred** — intranet-contained behind IDS; approval gate = over-engineering now; revisit if exposure widens | PERMISSIONS.md “Tracked follow-ups” |
| F3 | Dashboard **db-wide grant** (`ON solarinet.*`) gave PHP write on `ctlRequest`; a table REVOKE can't mask a db GRANT (additive) | **FIXED** — per-table grant *generator* that omits `ctlRequest`; verify query asserts zero ctlRequest privs | PERMISSIONS.md “DB grants” |
| F4 | Specific privileged verbs (APPROVE/PROVISION/CONFIG_SET/CONTROL/REJECT) were ORDINARY under the denylist | **FIXED** — subsumed by F1 allowlist; unit test asserts each is PRIVILEGED & DASHBOARD-refused | solariCtl.c; test_server_ctl.c |
| F5 | Claim CAS **race**: `UPDATE … LIMIT 1` then a separate `SELECT oldest` could read a different row under concurrency | **FIXED** — `id=LAST_INSERT_ID(id)` + `mysql_stmt_insert_id()` + `SELECT … WHERE id=?` (connection-local, race-free) | serverDb.c `serverDbCtlRequestClaim` |
| F6 | No startup invariant: `dashboardUid` unset (0) or colliding with `operatorUid` would silently mis-classify peers | **FIXED** — under `enforcePeer`, refuse to start (log + unlink + `ERR_INVALID_ARG`) on unset/collision | solariCtl.c startup |
| F7 | `argsWire TEXT` allowed a stored row to exceed the replay buffer → **silent truncation-on-claim**, replayed as a valid-looking prefix | **FIXED** — `VARCHAR(4096)` (= CTL_REQ_CAP); claim truncation fails the row closed (`ERR_BUFFER_FULL`) | migration 020 + schema.sql; serverDb.c |
| F8 | `serverDbCtlRequestComplete` returned OK when it affected **zero rows** (already-settled), hiding a lost update | **FIXED** — `affected_rows==0 → ERR_TLV_END` | serverDb.c; live DB test |
| F9 | Consumer replayed the **PHP-supplied `op=`**; and control.php routes still call privileged verbs directly | **FIXED** — op= bound server-side from stored `requestedBy` (injected first); **route-rewiring now DONE**: all six privileged routes (PROVISION/DECOMMISSION/RETIRE/DEPLOY/FLEET_PROVISION/FLEET_IMAGE) go through `SolariCtl::callQueued()` (enqueue→poll), passing no `op=` in args; ordinary CRIT_SET/SURVEY stay direct | solariCtl.c consumer; control.php; SolariCtl.php `callQueued` |
| F10 | **Global** idempotency key let one caller pre-seed a key so another's submit returned the first's row | **FIXED** — composite `UNIQUE(requestedBy, idempotencyKey)`; scoped SELECT | migration 020 + schema.sql; serverDb.c; live DB test |
| F11 | `verify-boundary.sh`: decisive checks **SKIP** (still PASS) when a client/secret is absent; CA key path hardcoded | **FIXED** — CA key read from `[ca] keyFile` in server.conf; unrunnable decisive checks now FAIL | verify-boundary.sh |
| F12 | `RuntimeDirectoryMode=0750` on `solari:solari` dir denies `www-solari` (in solari-ctl, not solari) the traverse → socket unreachable | **FIXED** — `0751` (other gets `--x`, traverse-only); matrix + footnote reconciled | solarinet-server.service; PERMISSIONS.md |

## Round-2 review disposition (rewire + verify layer-2; opus-4.8 authored → GPT-5.5/codex)

Scope: the callQueued rewire (control.php, SolariCtl.php) + the verify-boundary.sh
layer-2 fix. 5 findings, all dispositioned. Fixes are in control.php +
verify-boundary.sh only — SolariCtl.php was untouched (it already returned the
queued sentinel and already accepts an idem key). Re-verified end-to-end via the
disposable boundary harness: **RESULT PASS** (all layers, incl. the three
tightenings; server log confirms `queued RETIRE request=1 op=verify peer=992`).

| # | Sev | Finding | Disposition | Where |
|---|-----|---------|-------------|-------|
| R1 | P1 | Privileged-verb probe accepted **any** `ERR` as "boundary held" — a bad node / DB error past a broken ACL would false-PASS, and `node=1` could actually retire a real node if enforcement were off | **FIXED** — require the specific peer refusal `ERR -40 … must be queued via REQUEST_SUBMIT` (emitted pre-dispatch); any other ERR now FAILs loudly; probe uses a nonexistent node id so a broken boundary touches nothing | verify-boundary.sh layer 3 |
| R2 | P1 | Queued-timeout returned a synthetic success shape; RETIRE + DECOMMISSION-step2 routes reported `status:retired` when the action was merely **queued**, not done | **FIXED** — `ctrl_is_queued()`; routes surface `status:queued` + `request` id instead of a false `retired`; DECOMMISSION step-1 timeout now returns 503 (queued) rather than finalizing | control.php |
| R3 | P1 | No idempotency key → a retried POST (lost response / double-click) could **double-execute** DEPLOY/PROVISION/FLEET_* | **FIXED (partial)** — `ctrl_idem_key()` threads an `Idempotency-Key` header through callQueued; DECOMMISSION gets distinct per-stage keys (`.1`/`.2`). **Residual:** full protection needs the UI to send the header — tracked follow-up (dashboard JS, browser-only) | control.php |
| R4 | P2 | `--enqueue` only exercised REQUEST_GET (read); an ACL allowing GET but rejecting SUBMIT would pass while every route is unusable | **FIXED** — probe now enqueues a privileged RETIRE against a nonexistent node (SUBMIT accepts privileged verbs only, and enqueues without executing), asserting the write path; leaves one benign no-op request | verify-boundary.sh |
| R5 | P2 | Socket owner check accepted any `*:solari-ctl` — a `www-solari:solari-ctl` socket (dashboard owning its own gate) would pass | **FIXED** — require operator uid (`[ctl] operatorUid`, default solari) `:solari-ctl` exactly | verify-boundary.sh layer 2 |

## Files (reviewable)
- db/migrations/020_ctl_request_queue.sql, db/schema.sql (ctlRequest)
- src/server/server.h (config fields + 4 queue decls + serverCtlQueuePoll)
- src/server/serverContext.c ([ctl] parse, uid/gid resolvers)
- src/server/solariCtl.c (peer-cred, verb-class ACL, REQUEST_SUBMIT/GET, consumer)
- src/server/serverDb.c (4 queue fns), src/server/main.c (wire + batch)
- tests/unit/test_server_ctl.c (+3 tests), tests/unit/server_stubs.c (4 stubs)
- tests/integration/test_server_db_live.c (+queue test)
- dashboard/api/lib/SolariCtl.php (enqueue/poll/callQueued)
- dashboard/api/routes/control.php (privileged routes rewired to callQueued)
- deploy/dashboard/hardened/* (uid split, perms, verify)
