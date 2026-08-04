# REVIEW-CP12 — adversarial cross-lab review of CP1 + CP2

`2026-08-04 · Reviewer: Claude Opus 4.8 (adversarial, cross-lab) · Author under
review: GPT-5.6-codex · Binding: CONTRACT-CP.md v1.1 (§10 wins), protocol.h,
CONTRACT.md §9`

Scope: `db/migrations/016_panel_control.sql`, `dashboard/api/routes/panel.php`
(CP1); `status-panel/protocol.c`, `status-panel/daemon/solariPanel.c`,
`status-panel/daemon/tests/codec_test.c` (CP2); the Lead's HELLOREQ edit.
Modified nothing. Read-only checks run: `php -l` (clean), `make -C
status-panel/daemon test` in a scratch copy (builds `-Werror` clean, passes),
plus a standalone probe of the CONTROL/STATE codec bounds behaviour.

Per the assignment, the protocol.h STATE byte-7 `dwellSec` amendment is KNOWN
fix-round work and is not reported below.

## Verdicts

| Component | Verdict |
|---|---|
| CP1 — `panel.php` + migration 016 | **FIX-THEN-SHIP** (0 MUST, 4 SHOULD) |
| CP2 — `protocol.c` codecs | **SHIP** (0 MUST, 1 SHOULD) |
| CP2 — `solariPanel.c` daemon | **FIX-THEN-SHIP** (1 MUST, 3 SHOULD) |
| CP2 — `codec_test.c` | **FIX-THEN-SHIP** (1 MUST) |
| Lead edit — HELLOREQ at link-up | **SHIP** (0 MUST, 1 NIT) |

Counts: **2 MUST-FIX, 8 SHOULD, 6 NIT.**

Headline: the two MUST-FIXes are both in CP2 and neither is a codec bug — the
codecs are correct. One is a vacuous test that certifies a bounds check it never
runs; the other is the daemon treating a *data*-validation failure as a *serial*
failure and tearing down the link in response.

---

## MUST-FIX

### M1 — `codec_test.c:67,69`: the CONTROL/STATE bounds assertions are inverted and dead; decode length-rejection is untested

Both new test lines end with a disjunction of this shape:

```c
require(panelEncodeControl(1u,1u,1u,control,PANEL_CONTROL_SIZE-1u)==0u
     || panelDecodeControl(control,PANEL_CONTROL_SIZE-1u,&cmdId,&kind,&arg)==0,
        "CONTROL bounds accepted")
```

Two defects compounded:

1. The first operand is **true** (`panelEncodeControl` with `cap-1` correctly
   returns 0), so `||` short-circuits and the decode call is **never executed**.
2. If it ever were executed, it asserts `== 0` — i.e. that decode **succeeds**
   on a short payload, the exact opposite of the required behaviour.

Verified by probe against the real codec:

```
encodeControl(cap=7)      -> 0    (1st disjunct TRUE -> short-circuits)
decodeControl(len=7)      -> -1   (test asserts ==0)
decodeControl(len=9 over) -> -1
decodeState(len=15)       -> -1   (test asserts ==0)
```

Failure scenario: a future edit to `panelDecodeControl`/`panelDecodeState`
relaxes `len != SIZE` to `len >= 1` (or drops the check while adding a field).
`make test` stays green and the daemon starts reading past the end of a
truncated payload from the serial link. The suite the flash gate depends on
(§10 acceptance 7) currently proves nothing about decode bounds.

Fix: `require(panelEncodeControl(...,PANEL_CONTROL_SIZE-1u)==0u && panelDecodeControl(control,PANEL_CONTROL_SIZE-1u,&cmdId,&kind,&arg)==-1 && panelDecodeControl(control,PANEL_CONTROL_SIZE+1u,&cmdId,&kind,&arg)==-1, "CONTROL bounds accepted")` — same shape for STATE.

Related gap in the same finding, answering the assignment's explicit question:
**no new test feeds CONTROL or STATE bytes through `panelParserFeed`.** All four
new assertions call the codecs directly. `receive()` (`codec_test.c:14-17`)
counts only `PANEL_FT_SNAPSHOT`, so the `knownType()` change at `protocol.c:148`
— the one line that admits 0x04/0x84 into dispatch — has **zero** coverage.
Add one byte-at-a-time `panelParserFeed` of an encoded STATE frame asserting the
callback fires with `type==PANEL_FT_STATE` and the payload round-trips.

### M2 — `solariPanel.c:118,137`: a rejected *command payload* is handled as a *serial write failure*, tearing down the link and closing the capability gate

`forwardCommands()` returns `-1` for six distinct conditions, only two of which
are serial failures. The other four are data-validation rejections: JSON parse
failure, `commands` not an array or `count > 16`, a malformed array element, and
an out-of-range `cmdId`/`kind`/`arg`. The caller cannot tell them apart:

```c
if(sendFrame(fd,&latest,1)!=0||forwardCommands(fd,response.data,&link)!=0){
  logMessage("serial write failure; reopening");
  close(fd);fd=-1;nextSerial=now+serialDelay*1000u;
  serialDelay=serialDelay<30u?serialDelay*2u:30u;}
```

So one unacceptable command in the queue closes the serial fd, resets
`link.stateSeen`/`haveState`/`withholdingLogged` on reopen (line 142) — slamming
the §10 capability gate shut — and doubles the reopen backoff toward 30 s.
Because the offending command stays `pending` server-side and is **re-served
every poll** (§10, by design), this is a self-sustaining loop: reopen, poll,
reject, reopen. Snapshots stop reaching the panel for the duration, and *no*
command flows, not just the bad one.

Concrete failure scenario, and the reason this is MUST rather than SHOULD: the
kind range is duplicated in three places — `panel.php:146` (`$kind > 7`),
`protocol.h:84-92`, and `solariPanel.c:118`
(`kind>(int)PANEL_CTL_SLEEP`). The moment a `PANEL_CTL_*` kind 8 is added to
protocol.h, the firmware and the API — a normal staged rollout, and exactly the
forward-compatibility case protocol.h's header comment designs for — the
not-yet-updated daemon rejects it and flaps the panel link continuously until an
operator notices. The panel goes dark, not just unresponsive.

(Today's queue cannot produce a rejected command: `panelCommandInput` at
`panel.php:139-161` validates every field the daemon re-checks, and `cmdId==0`
is unreachable from AUTO_INCREMENT. The defect is the failure *mode*, which is
disproportionate and destructive, not a live-reachable trigger.)

Fix: give `forwardCommands` a distinct return code for data rejection (e.g. `-2`)
— or better, per the "unknown types are skipped, never desync" doctrine already
in protocol.h, log-and-skip the individual bad element and keep forwarding the
rest. Reserve `-1` for `writeAll` failure, and reopen the serial only for that.

---

## SHOULD

### S1 — `panel.php:198-203` vs `608-613`: a command applied in the last poll window before expiry is reported to the operator as failed

Both terminal transitions are guarded on `status = 'pending'`, so terminal-state
immutability itself is **sound in both directions** (checked — see clean list
C2). The defect is that expiry can win a race it should lose, because expiry is
a *guess* and STATE is *ground truth*:

- t=0 operator POSTs command 42.
- t≈118 daemon poll serves 42 (still `pending`, ~2 s of margin left), forwards it.
- t≈119 panel applies it, emits STATE with `lastCmdId=42`.
- t≈120.2 **any** authenticated GET `/api/panel` — including the daemon's own
  next poll — runs the sweep at line 198 → 42 becomes `expired`.
- t≈120.5 the daemon POSTs state; the UPDATE at line 608 matches
  `status='pending'` → **0 rows**. 42 stays `expired` forever.

Result: the page shows command 42 as failed with reason `expired`, while
`panelState` in the *same payload* shows the panel in the state that command
produced. The exposure window is roughly one poll interval out of 120 s, so on
the order of 4-5% of commands issued near a queue backlog.

Fix (one line, but it relaxes §10 and therefore needs a Lead ruling rather than
a silent patch — §0): in POST `/api/panel/state`, let a confirmation reclaim a
late-expired command — `WHERE status IN ('pending','expired') AND cmdId <= :lastCmdId`,
also clearing `expiredAt`. The alternative that preserves §10 verbatim is to
stop *serving* commands within one poll interval of expiry (`createdAt <
NOW(6) - INTERVAL 110 SECOND` excluded from the served set), so anything the
daemon forwards always has time to confirm. I recommend the first: STATE is the
only authority §10 recognises for completion, so it should also outrank a
timeout for the same command.

### S2 — `panel.php:177`: `lastCmdId` is validated to `PHP_INT_MAX` while the wire field is `u32`

`panelStateInput`'s range table bounds `ackedEpisodeId` correctly at
`4294967295` but bounds `lastCmdId` at `PHP_INT_MAX` (9.2e18). The value flows
straight into the `cmdId <= :lastCmdId` sweep at line 611.

Failure scenario: a single malformed or wedged STATE post carrying a large
`lastCmdId` marks **every** pending command `applied` in one statement,
irreversibly (they are terminal), with no command having reached the panel. The
page then reports success for all of them. Reachability is limited to the panel
service principal, so this is defence-in-depth rather than a privilege boundary
— but the correct bound is free and the field is a `u32` on both the wire
(`protocol.h:78`) and in the daemon (`PanelStateReport.lastCmdId`, `solariPanel.c:25`).

Fix: `'lastCmdId' => [0, 4294967295]`.

### S3 — `panel.php:198-203`: an unconditional write on the hot 5 s read path

The expiry sweep runs on **every** GET `/api/panel`, for every principal, before
the read-only snapshot — an autocommit write (binlog record, row locks) per poll
per open dashboard tab, on a route whose entire design premise is a cheap 5 s
read. It also puts the poll path into lock contention with POST `/api/panel/command`,
which holds `SELECT COUNT(*) ... FOR UPDATE` over the same
`panelCommandPending(status,cmdId)` index range (line 556). A poll that blocks
behind that lock burns the daemon's 10 s curl timeout and trips its exponential
backoff.

The sweep is also redundant on this path: the panel principal polls every 5 s
regardless, and POST `/command` sweeps independently (line 543), so gating it
costs at most 5 s of staleness in the operator's `recentCommands` view.

Fix: `if ($isServicePrincipal) { Db::exec("UPDATE panelCommand SET status='expired' ..."); }`.

### S4 — `solariPanel.c:110`: a failed STATE upload is recorded as delivered, defeating the §5 rate limit's own recovery

`handleState` updates `link->lastState`, `link->haveState` and
`link->lastStatePostMs` and then discards `postState`'s return value
(`(void)postState(...)`). A STATE that failed to upload is therefore
indistinguishable from one that succeeded, and is never retried — the next
upload happens only on the *next* change or at the 30 s heartbeat.

This matters because `postState` has no 401 re-login path. The 401 recovery at
`solariPanel.c:131` covers only the GET poll; a `postState` that meets an expired
session logs `panel state post failed (HTTP=401)` and returns -1. Recovery is
real but indirect (the next GET re-logins and replaces `link.curl`), so the
worst case is a confirmation delayed up to 30 s. Against the 120 s expiry that
survives — but it stacks directly on top of S1's race, and if `link->curl` is
NULL during login backoff (up to 60 s) the same 30 s hole reopens each time.

Fix: on `postState` failure, leave `lastStatePostMs` unchanged and do not set
`haveState`, so the next serial frame retries; optionally add the same
401-then-relogin retry the GET path already has.

### S5 — `solariPanel.c:110`: `memcmp` over a struct with padding silently defeats the change-detection rate limit

`changed = !link->haveState || memcmp(&state,&link->lastState,sizeof(state))!=0`
compares `PanelStateReport` (`solariPanel.c:25`) — seven `uint8_t` followed by
two `uint32_t`, so one byte of padding at offset 7. `state` is `memset` to zero,
but `link->lastState = state` is a struct assignment, and C does not guarantee
padding bytes are copied. Where the compiler does not copy them, `memcmp` sees a
difference on every frame, `changed` is permanently true, and the daemon POSTs
on **every** STATE frame instead of on-change — dropping the CONTRACT-CP §5
rate limit ("on change or ≥30 s") on the floor.

In practice gcc/clang copy the padding, which is why this has not been observed;
it is a latent, compiler-dependent contract violation.

Fix: compare field-wise, or add an explicit `uint8_t reserved;` at offset 7 (which
the dwellSec fix round will want anyway).

### S6 — `protocol.c:64,96`: `len != SIZE` rejects forward-compatible extensions of CONTROL/STATE

Both decoders require an exact length. `panelDecodeSnapshot` (line 138) takes the
opposite stance — `if (len < need) return -1`, trailing bytes ignored — and
protocol.h:257 documents that as deliberate forward compatibility.

Failure scenario: this is not hypothetical for these two frames specifically.
STATE byte 7 was *just* repurposed from `reserved` to `dwellSec` in the CP
amendment. The next such amendment that needs a byte beyond 16 will extend
`PANEL_STATE_SIZE`, and every deployed daemon built against the old header will
reject 100% of STATE frames — closing the capability gate and stalling the
entire command queue, rather than degrading to the fields it understands.

Fix: `len < PANEL_CONTROL_SIZE` / `len < PANEL_STATE_SIZE`, matching the snapshot
decoder's contract. (Note this is a protocol.h semantics question as much as a
protocol.c one — it should be documented alongside the codec declarations at
`protocol.h:97-112`, which are silent on the point.)

### S7 — `solariPanel.c:27-30` and `codec_test.c:6-9`: local redeclaration of the shared codec, contrary to protocol.h's explicit instruction

`protocol.h:97-98` states, normatively: *"CONTROL/STATE codecs — implemented in
protocol.c, shared by BOTH sides (declared here so neither side ever declares
them locally)."* Both files `#include` protocol.h **and** then redeclare all four
prototypes verbatim.

The duplicates are currently identical, so this compiles. The hazard is the one
the header is guarding against: if a signature changes in protocol.h, these
copies are three more sites that must change in lockstep, and a mismatched copy
in a `.c` that does not include the header would link against the wrong ABI
silently. It also directly contradicts a binding instruction in the normative
header, which is a review-gate issue independent of impact.

Fix: delete `solariPanel.c:27-30` and `codec_test.c:6-9`.

### S8 — §10 semantics gap: a firmware-*rejected* command is reported to the operator as `applied`

§10 defines `lastCmdId` as the highest cmdId **consumed** — "applied, or rejected
as invalid/unknown — both consume." The server-side sweep (`panel.php:608`)
cannot distinguish the two and marks the whole `cmdId <=` range `applied`. So a
command the panel refused shows a green "applied" in the operator's
`recentCommands`.

Today the API's own validation (`panel.php:139-161`) makes a rejection mean
"daemon/firmware version skew", so this surfaces exactly when the operator most
needs to see it. This is a contract-level gap rather than a CP1 coding defect —
raising it to the Lead per §0 rather than proposing a silent code change. The
cheap mitigation if it is not worth a protocol change: label the state `consumed`
rather than `applied` in the page, so the UI does not claim more than the wire
proves.

---

## NIT

- **N1 — `016_panel_control.sql:6` / `29`:** `cmdId` and `lastCmdId` are `BIGINT
  UNSIGNED` while the wire field is `u32`. Past 4294967295 the daemon's
  `u32Value` clamp (`solariPanel.c:66,118`) silently pins every cmdId to
  `UINT32_MAX` and the firmware's `cmdId > lastCmdId` dedupe stops advancing —
  the queue would deadlock rather than misfire. Unreachable at this fleet's rate,
  but `INT UNSIGNED` would make the wire type structural.
- **N2 — `016_panel_control.sql:16`:** `KEY panelCommandRecent (cmdId DESC)` is
  redundant with `PRIMARY KEY (cmdId)`, which already serves `ORDER BY cmdId DESC
  LIMIT 16` (line 298). MariaDB additionally parses `DESC` and ignores it.
- **N3 — `panel.php:556`:** the race-free pending cap depends on the server
  running `REPEATABLE READ` (nothing in `Db::pdo()` sets an isolation level). Under
  `READ COMMITTED` InnoDB disables gap locks and two concurrent POSTs can both
  observe 15 and both insert. Worth an explicit `SET TRANSACTION ISOLATION LEVEL
  REPEATABLE READ` or a comment recording the dependency. The `>= 16` boundary
  itself is correct — max 16 pending, 17th → 409 (checked).
- **N4 — Lead edit, `solariPanel.c:146`:** `if(n>0u)(void)writeAll(fd,frame,n);`
  discards the result. A `writeAll` against a CDC endpoint with no reader blocks
  for the full 2000 ms deadline (line 108) inside the main loop before returning
  -1, delaying the poll by up to 2 s, and the failure is then invisible. The
  design is sound and the failure is self-correcting — the very next `readSerial`
  or `sendFrame` detects the dead fd and reopens, so the gate is not left
  falsely open. Recommend logging the failure rather than acting on it, to keep
  the acceptance-5 journal trace ("HELLOREQ gate-opening") honest when it fails.
- **N5 — `solariPanel.c:118`:** `forwardCommands` re-parses the entire poll body
  with cJSON, having already parsed it in `parseSnapshot` — a second full parse of
  a snapshot document each poll to read a ≤16-element array. Negligible at 0.2 Hz;
  noted only because the assignment asked about cost creep.
- **N6 — `panel.php:143`:** `is_int($kind)` rejects JSON `1.0` and `"1"`. Correct
  and deliberate, but it couples CP4 to sending true integers; worth one line in
  the CP4 handoff so a slider emitting a float does not produce a mystery 400.

---

## Checked, clean

Classes actively hunted and found absent — recorded so the fix round does not
re-litigate them:

- **C1 — SQL injection, `panel.php`:** every new statement is either a string
  literal or fully parameterised. The one dynamically-built parameter array
  (line 606, `array_combine` over `array_keys($state)`) draws its keys from the
  hard-coded `$ranges` table at line 173, never from the request body — a body key
  outside that set is discarded by `panelStateInput`'s loop. No interpolation of
  request data anywhere in the diff. `ATTR_EMULATE_PREPARES => false`.
- **C2 — terminal-state immutability:** both transitions are guarded on
  `status = 'pending'` (lines 201, 546, 611). `applied` can never become
  `expired` and `expired` can never become `applied`. The failure in S1 is that
  the *wrong* terminal state is reached first, not that a terminal state mutates.
- **C3 — service-principal predicate exactness:** traced every path that can
  populate `source`. Only `Auth::verifyLocal` (`Auth.php:125`) emits `'local'`;
  `Oidc.php:310` emits `'oidc'`; `Auth::establishSession` (`Auth.php:194`)
  defaults anything else to `'directory'`. A Keycloak/OIDC identity named `panel`
  with role `viewer` therefore carries `source='oidc'` and is correctly rejected.
  No API route creates or renames local users (the store is a file managed
  out-of-band), so a renamed local user requires filesystem access on the web
  host — not an in-band escalation. The triple is exact.
- **C4 — service principal cannot issue commands:** the explicit deny at
  `panel.php:531-534` precedes the role check, as §10 requires. Operator/admin
  cannot POST state (line 585). Both rejections log principal + IP without body
  content (`panelLogRejectedWrite`, line 124).
- **C5 — `recentCommands` leak:** gated on `$isOperator` at both the query (line
  296) and the payload (line 505). A viewer session — including the panel service
  principal, which is `role=viewer` — receives neither the key nor the query cost.
  `createdBy` is deliberately not exposed. Symmetrically, `commands` is gated on
  `$isServicePrincipal` at both query (line 300) and payload (line 515), so a
  dashboard user polling `/api/panel` never sees or consumes the queue (§10).
- **C6 — 16-cap off-by-one:** `$pending >= 16` permits exactly 16 pending and
  409s the 17th, matching §10. (Lock-mode caveat in N3.)
- **C7 — transaction consistency:** `Db` holds a per-request PDO singleton
  (`Db.php:23`), so `Db::exec`/`Db::scalar` share the connection with
  `$pdo->beginTransaction()`. The new POST writes each run in their own
  transaction with a `finally`-equivalent rollback; `Response::error` exits after
  the explicit `rollBack()` at line 559, so no transaction is abandoned open.
  The GET's expiry write is committed before the consistent-snapshot read begins,
  so the snapshot cannot show a half-swept queue.
- **C8 — CSRF on the new POSTs:** session cookie is `SameSite=Lax`
  (`Auth.php:84`) and `solari_json_body()` reads only `php://input` with no
  `$_POST` fallback, rejecting a non-JSON body with 400. A cross-site form POST
  can neither carry the cookie nor set `application/json`. Consistent with every
  other POST route in the app.
- **C9 — codec offsets vs protocol.h:** CONTROL — `u32 cmdId` LE at 0, `kind` at
  4, `arg` at 5, bytes 6-7 zeroed, total 8. STATE — bytes 0-6 as documented, byte
  7 untouched, `ackedEpisodeId` LE at 8, `lastCmdId` LE at 12, total 16. Both
  match `protocol.h:60-95` exactly. All access is byte-wise through
  `readLe32`/`writeLe32`, so there is no unaligned access and no
  endianness/padding exposure — the header's central rule holds.
- **C10 — decode bounds:** both decoders NULL-check every out-parameter and
  reject short *and* oversized payloads (verified by probe: -1 at len 7/9 for
  CONTROL, -1 at len 15 for STATE). Both encoders reject insufficient capacity
  and `memset` the full payload before writing, so reserved bytes are never
  uninitialised. The behaviour is correct; only its *test* is not (M1), and only
  its forward-compat stance is debatable (S6).
- **C11 — `knownType()` admits exactly the defined set:** `0x01..0x04` covers
  SNAPSHOT/PING/HELLOREQ/CONTROL and `0x81..0x84` covers HELLO/EVENT/LOG/STATE,
  with no undefined value inside either range. Exactly the eight types in
  `PanelFrameType`, no more. (Range comparison rather than a switch means any
  future non-contiguous type would need care; not a defect today.)
- **C12 — command forward ordering:** the server serves `ORDER BY cmdId ASC`
  (line 302) and `forwardCommands` iterates the array in order, aborting on the
  first `writeAll` failure rather than skipping ahead. No gap can be created by
  the daemon, which is what makes the `cmdId <= lastCmdId` sweep sound.
- **C13 — capability-gate state machine across reopen:** `stateSeen`, `haveState`
  and `withholdingLogged` are all reset at `solariPanel.c:142` on every successful
  reopen, and only there; the gate correctly re-closes on a new link and the
  withholding message is re-armed once per link-up. The gate check (`count!=0 &&
  !link->stateSeen`) returns 0, not -1, so withholding does not trigger the
  reopen path.
- **C14 — no regression to the seq fix or FORBID_REUSE:** `staged.seq =
  snapshot->seq` is preserved at `solariPanel.c:99` with its comment intact;
  `latest.seq++` still occurs at both send sites (137, 149). `postState` issues
  through `request()` → `setCurlCommon()`, so it inherits
  `FORBID_REUSE`/`FRESH_CONNECT`, the CA pinning, `VERIFYPEER`/`VERIFYHOST`, and
  the timeouts unchanged.
- **C15 — memory/fd hygiene in the new paths:** `forwardCommands` calls
  `cJSON_Delete(root)` on all eight exit paths (verified individually).
  `postState` frees the cJSON object, the printed body and the header slist on
  every path including both early-return error branches, and restores
  `CURLOPT_HTTPHEADER` to NULL after the request. `handleState` allocates
  nothing. No fd is leaked in the new code: the only `close()` sites are the
  pre-existing failure paths, each followed by `fd=-1`.
- **C16 — JSON injection into `/api/panel/state`:** every field is emitted via
  `cJSON_AddNumberToObject` from a `uint8_t`/`uint32_t`; no string field crosses
  the boundary, so there is nothing to escape. Values are structurally within the
  server's declared ranges except `lastCmdId` (see S2).

---

## Process note

The recorded incident — CP1 referencing `panelCommand` before migration 016 was
applied, 500ing the live endpoint for ~80 s — is a sequencing failure, not a code
defect, and the current code state is what I judged above. Worth one line in the
CP handoff regardless: a migration and the code that reads its tables are a single
deployable unit, and the API is live during the build. Per the assignment this
did not weigh on any verdict.

---

## RE-CHECK

Round-2 adversarial re-check of the fix round (`## FIX ROUND CP12` in RETURN-CP1.md /
RETURN-CP2.md). Method: fresh scratch copy, `make clean && make test`, two mutation
tests against the repaired suite, byte-level codec probes, and full end-to-end trace
of the dwellSec chain. Nothing outside this file was modified.

### R0. Round-1 findings — resolution status

| # | Finding | Status | Evidence |
|---|---------|--------|----------|
| M1 | codec_test assertions inverted; no parser-path coverage | **RESOLVED** | Assertions split into separate `require()` calls with correct polarity; `panelParserFeed` byte-at-a-time dispatch tests added for CONTROL and STATE. Mutation-verified below. |
| M2 | `forwardCommands` returned -1 on bad data → link teardown loop | **RESOLVED** | `solariPanel.c` — data-rejection paths now `continue` after a log; `-1` reserved for encode/`writeAll` failure. See R3 for the trade this introduced. |
| S1 | Late STATE lost to expiry | **RESOLVED in principle, DEFECTIVE in implementation** | See **R1** — the `expired → applied` transition landed with no eligibility bound. |
| S2 | u32 fields unclamped | **RESOLVED** | `panelStateInput` ranges include `ackedEpisodeId`/`lastCmdId` `[0, 4294967295]`, `dwellSec [0,255]`. |
| S3 | Expiry sweep ran on every principal's GET | **RESOLVED, with a display regression** | `panel.php:198` gates the sweep on `$isServicePrincipal`. See **R6**. |
| S4 | `lastState` cached before POST confirmed | **RESOLVED** | `handleState` assigns `link->lastState`/`haveState`/`lastStatePostMs` only inside `if (postState(...) == 0)`. |
| S5 | `memcmp` over a struct with padding | **RESOLVED (structurally)** | dwellSec filled the 8th byte slot: `sizeof(PanelStateReport) == 16`, exactly 8×u8 + 2×u32. Verified by compiled probe. No padding bytes exist for `memcmp` to read. |
| S6 | Decoders rejected trailing bytes | **RESOLVED** | Both `panelDecodeControl` and `panelDecodeState` use `len < SIZE`, matching `panelDecodeSnapshot`. Parser test with trailing bytes passes. |
| S7 | Duplicate local codec declarations | **RESOLVED** | No local `panelEncode*`/`panelDecode*` declarations remain in `solariPanel.c` or `codec_test.c`; all come from `protocol.h`. |
| S8 | applied-vs-rejected indistinguishable | **ACCEPTED** by Lead as a documented residual in CONTRACT-CP §10. Not re-litigated. |

**Test-suite mutation verification.** A repaired test suite is only worth what it
catches, so I broke the code twice and confirmed the suite fails:

1. Relaxed both decoder bounds to `len < 1u` (kept the parameter used so `-Werror`
   would not mask the result as a build failure) → `make test` fails with
   `CONTROL short decode accepted`.
2. Reverted `knownType()` to the pre-fix ranges (`type <= PANEL_FT_HELLOREQ` /
   `type <= PANEL_FT_LOG`, excluding CONTROL/STATE) → `make test` fails with
   `CONTROL parser dispatch missing`.

Both mutations were reverted; the scratch tree was discarded. Clean run:
`make -C status-panel/daemon clean && make test` → `codec tests passed`.

### R0b. dwellSec chain — end-to-end, checked clean

Traced every layer; the chain is complete and internally consistent at byte 7:

- `protocol.h` — `dwellSec` at payload offset 7, `PANEL_STATE_SIZE` 16, reserved byte consumed (no size change, no version bump needed).
- `protocol.c` — `panelEncodeState`/`panelDecodeState` signatures carry `dwellSec`; offset 7 read/written; u32 fields still LE at 8 and 12.
- `codec_test.c` — round-trip asserts `state[7] == 30`.
- `firmware/main.c:392`, `firmware/test/panelCtlTest.c:495,518,544` — callers updated; the firmware is the producer, so the chain has a real source.
- `daemon/solariPanel.c` — `PanelStateReport.dwellSec` decoded and serialised into the STATE POST body.
- `db/migrations/017_panel_dwell.sql` — `panelState.dwellSec TINYINT UNSIGNED NOT NULL DEFAULT 0 AFTER alarmAcked`.
- `panel.php` — validated `[0,255]`, threaded through the panelState SELECT, upsert, GET payload and POST response.

**Deploy-ordering note (process, not code):** RETURN-CP1.md states 017 is "not
applied"; the Lead states it was applied live. The Lead is authoritative and the
discrepancy is stale documentation — but note the shape: `panel.php` now
`SELECT`s `dwellSec` unconditionally, so shipping the PHP ahead of 017 reproduces
the exact CP1 incident (a 500 on the live endpoint). **017 and this panel.php must
land together, migration first.** Please correct the RETURN packet.

### R1. MUST-FIX — the late-confirmation sweep has no eligibility bound

`dashboard/api/routes/panel.php:613-614`

```sql
UPDATE panelCommand SET status = 'applied', appliedAt = NOW(6)
 WHERE status IN ('pending', 'expired') AND cmdId <= :lastCmdId
```

CONTRACT-CP §10 authorises "a confirmation arriving **after expiry** wins" — a
late confirmation *of that command*. The implementation instead resurrects **every
expired command in table history** below the reported `lastCmdId`, on **every**
STATE post (every change, plus the 30 s heartbeat, forever).

**Concrete failure, reachable from the §10 acceptance suite itself.** Acceptance
test 5 says: unplug the panel USB, post a command, observe `expired` at 120 s. Now
plug the panel back in. Firmware boots with `lastCmdId = 0`; the operator issues
one new command; it applies; STATE reports the new `lastCmdId`. The sweep's
predicate matches the *previously expired* command too — it flips to `applied`
with `appliedAt = NOW(6)`, seconds ago, having never been delivered. Run the
acceptance test and then use the panel normally, and the test's own evidence
rewrites itself. At scale: 50 commands expired over a weekend outage all resurrect
in one statement.

Secondary: this is an unbounded, monotonically growing index scan on the hottest
write path in the feature.

**Fix (one line):**

```sql
WHERE cmdId <= :lastCmdId
  AND (status = 'pending'
       OR (status = 'expired' AND expiredAt > DATE_SUB(NOW(6), INTERVAL 15 SECOND)))
```

15 s comfortably covers the 5 s poll plus serial and POST latency — the window an
honest late confirmation can occupy — while making a weekend-old row ineligible.
CONTRACT-CP §10 should gain the same bound in words, since the current text reads
as if unbounded resurrection were intended.

### R2. SHOULD — resurrected rows break the recentCommands display contract

`dashboard/api/routes/panel.php:509-515` (asked about directly by the Lead)

Once `expired → applied` is legal, a row can carry **both** `appliedAt` and
`expiredAt` non-NULL — §10 explicitly retains `expiredAt` "for the record". Two
consequences:

1. `failureReason` is derived as `$row['status'] === 'expired' ? 'expired' : null`,
   so a resurrected row reports `failureReason: null` and renders green. Correct
   for a genuine late confirmation; with R1 unfixed it is how the false "applied"
   reaches the operator's eyes.
2. **CP4 must not treat `appliedAt` and `expiredAt` as mutually exclusive**, and
   must not infer failure from `expiredAt !== null`. `status` is the only
   authority. Worth stating in the contract before CP4 is written against it.

Also note the blast radius is partly *hidden*: `ORDER BY cmdId DESC LIMIT 16` means
resurrected old rows mostly fall outside the operator's view. The database record
is falsified for rows the operator cannot see to question. That makes R1 worse, not
better.

### R3. SHOULD — the M2 skip path converts a loud failure into a silent lie

`status-panel/daemon/solariPanel.c`, `forwardCommands`

**Answering the Lead's two questions directly, both clean:**

- *Does it still preserve ordering?* **Yes.** The loop walks the array in server
  order (`ORDER BY cmdId ASC`) and `continue` does not reorder surviving elements.
- *Can a skipped command block later ones?* **No.** `continue` advances; each
  element encodes and writes independently; there is no shared accumulator that a
  skip poisons.

The regression is elsewhere. The daemon skips a command whose `kind` falls outside
`PANEL_CTL_SETTHEME..PANEL_CTL_SLEEP`, so it is **never delivered** — but it stays
`pending` server-side, and the moment any *later* command applies and advances
`lastCmdId` past it, R1's sweep marks the undelivered command `applied`. Under
server/daemon version skew (server knows a new `kind`, daemon does not — the exact
scenario a range check exists for) the operator is told an action succeeded that
never reached the panel. M2 correctly removed the destructive link-flap, but the
replacement failure mode is quieter than the bug it replaced.

**Fix:** delete the daemon's `kind` range check entirely and forward the frame.
`protocol.h` §3 states firmware ignores unknown kinds, and §10 makes firmware
rejection a *consuming* path — so forwarding is both safe and semantically
correct, and the daemon has no business being a third validator. This also deletes
the third copy of the kind range (server, daemon, firmware), which is what made M2
possible. Keep the `cmdId == 0`, `arg` range and encode-failure guards; those
defend real frame invariants.

### R4. SHOULD — S3's gating leaves the operator view stale when the daemon is down

`dashboard/api/routes/panel.php:198`

Gating the sweep on the service principal was right for cost, but it removed the
only sweep that ran when the panel daemon is **not** polling. If the daemon is dead
— precisely when commands are guaranteed to fail — nothing marks them `expired`
except a subsequent `POST /command` (line 545, whose comment already anticipates
this). A pure observer watching the page sees `pending` indefinitely, past 120 s,
forever. CONTRACT-CP §10 requires expiry be "visible in the page as failed, never
silently dropped."

**Fix (read-side, keeps S3's write-path win):** in the `recentCommands` map,
present `status` as `expired` when `status === 'pending' && createdAt < now - 120s`,
without writing. Display truth without a write on the operator path.

### R5. NIT — skip path has no one-shot log guard

`forwardCommands` logs `skipping invalid command id=…` on **every** poll for the
same command. At 5 s polling over a 120 s life that is ~24 identical lines per bad
command. The file already has the right idiom in `withholdingLogged`; mirror it.
(Moot if R3 is taken — there would be no skip path.)

### R6. NIT — postState failure loop is noisy but bounded

While `postState` persistently fails, `haveState` never sets, so `changed` stays
true and every STATE frame logs and re-attempts a full TLS handshake. Bounded by
the firmware's 1 s change floor and the connect timeout; not a defect, noted so it
is not mistaken for one during an API outage.

### Checked, clean (re-check scope)

- **No regression to the wire seq fix or `FORBID_REUSE`** — both unchanged; diffed the surrounding blocks.
- **Lead's HELLOREQ edit** — still correct in the reopen path, with `stateSeen` / `haveState` / `withholdingLogged` all reset alongside. Capability gate re-arms properly across a serial reopen.
- **Terminal-state immutability** — `applied` remains terminal; the sweep's `WHERE status IN ('pending','expired')` cannot touch it. Only the one transition §10 newly authorises is reachable.
- **SQL injection** — all new dwellSec and sweep statements are bound parameters; no interpolation introduced.
- **Service-principal predicate** — unchanged from round 1 (`local && panel && viewer`); re-confirmed no new caller weakens it.
- **16-cap** — unchanged; still `SELECT COUNT(*) … FOR UPDATE` inside the transaction.
- **Codec bounds / LE / unaligned access** — re-probed byte-for-byte after the signature change; offsets match `protocol.h`, no UB.
- **`knownType()`** — admits exactly the defined set; mutation-verified.
- **Memory / fd hygiene** — every `cJSON_Delete` path in the rewritten `forwardCommands` accounted for, including the new `continue` (nothing is allocated per element).

### RE-CHECK verdicts

| Component | Verdict | Gate |
|---|---|---|
| CP1 — `panel.php` + migrations 016/017 | **FIX-THEN-SHIP** | R1 (must), R2, R4 |
| CP2 — `protocol.c` codec | **SHIP** | — |
| CP2 — `daemon/solariPanel.c` | **FIX-THEN-SHIP** | R3 (should), R5 |
| CP2 — `daemon/tests/codec_test.c` | **SHIP** | mutation-verified |
| Lead edit — HELLOREQ at link-up | **SHIP** | — |
| dwellSec chain | **SHIP** | deploy 017 before the PHP |

Round-1 findings: 2 MUST-FIX, 8 SHOULD, 6 NIT — all resolved or accepted.
Round-2 new: **1 MUST-FIX, 3 SHOULD, 2 NIT.** One MUST-FIX is a defect *in* the S1
fix; one SHOULD is a trade *in* the M2 fix. The codec and test work is done.
