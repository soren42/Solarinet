# REVIEW-LC12 — adversarial cross-lab review of LC1 + LC2

`reviewer: claude/opus-5 · 2026-08-06 · lanes under review: gpt-5.6-codex`
`spec: status-panel/CONTRACT-LC.md (§7 dispositions + §8 amendments binding)`
`tree: /home/jason/Code/Solarinet @ feat/lifecycle-criticality, uncommitted`

No permanent source change was made. Three C mutations and two Python mutations were
applied and reverted; `git diff --stat` matches the pre-review state byte for byte.
No write touched the live `solarinet` database — SQL validation ran in an isolated
`solari_review_scratch` schema, since dropped.

---

## Verdict

| Component | Verdict | Blocking findings |
|---|---|---|
| `db/migrations/018_lifecycle_criticality.sql` | **SHIP** (with S8 folded in while additive) | — |
| C engine — `serverAlert.c` tier composition | **FIX-THEN-SHIP** | M7, M9 |
| C cascade — `serverDb.c` lifecycle/purge/clear | **FIX-THEN-SHIP** | M2, M3, M8 |
| C verbs — `solariCtl.c` | **FIX-THEN-SHIP** | M4 |
| C tests — `tests/unit/*` | **FIX-THEN-SHIP** | M8 |
| PHP routes — `assets.php`, `control.php` | **FIX-THEN-SHIP** | M1, M5 |
| PHP — `Operator.php`, `nodes.php`, `discovery.php` | **SHIP** | — |
| `alertbridge.py` | **FIX-THEN-SHIP** | M6 |

**Do not deploy.** M1 alone means criticality (task #10) does not work at all;
M2 + M3 mean lifecycle (task #9) does not fix the pihole case that motivated it,
and makes the panel alert count worse rather than better.

---

## Mandatory check outputs

### 1. Rebuild + C test suite

```
$ cmake -S . -B build -DSOLARI_BUILD_SERVER=ON -DSOLARI_WITH_IO=ON -DSOLARI_BUILD_TESTS=ON
$ cmake --build build -j4
[100%] Built target solariSnmpPoll
(no warnings from any file in LC1 scope)

$ ctest --test-dir build
100% tests passed, 0 tests failed out of 25
```

All 25 passed in this environment — the three sandbox-environmental failures
quoted in RETURN-LC1 (`test_net_loopback`, `test_platlinux`, `test_probe`) did not
reproduce here, so the suite is genuinely green. Nothing else failed.

### 2. C mutation probes

**(a) lifecycle cascade transaction — NOT CAUGHT.** Replaced the rollback path of
`serverDbLifecycleTransition()` with a commit (`fail_nostmt: mysql_commit(conn)`):

```
100% tests passed, 0 tests failed out of 25
```

See **M8**.

**(b) id-space mapping helper — CAUGHT.** Flipped tcp from proto 2 to 3 in both
directions:

```
tests/unit/test_server_db.c:176:test_probe_target_id:FAIL:
    Expected 'tcp:10.0.0.1:443' Was 'udp:10.0.0.1:443'
7 Tests 1 Failures 0 Ignored / FAIL
```

Caught — but attributed to the wrong test name; see **S7**.

**(c) F3 scoped state drop — CAUGHT.** Made `serverAlertDropTargetState()` clear
every in-use slot:

```
18/25 Test #18: test_server_alert ................***Failed    0.00 sec
96% tests passed, 1 tests failed out of 25
```

### 3. alertbridge

```
$ python3 -m py_compile deploy/alertbridge/alertbridge.py     → OK
$ python3 deploy/alertbridge/test_dispositions.py
Ran 4 tests in 0.001s / OK
```

**E9 mutation 1 — checkpoint advanced without persisting (suppress branch) — CAUGHT:**

```
FAIL: test_a11_suppression_persists_before_later_publish_failure
AssertionError: 1 != 2
```

**E9 mutation 2 — `persisted_cp` advanced before `save_state` (publish branch) — CAUGHT:**

```
FAIL: test_a11_suppression_persists_before_later_publish_failure
AssertionError: 1 != 2
```

**Loss analysis for `[tier1-suppressed, tier4-published]` in eventId order.** The
query is `WHERE e.eventId > %s ORDER BY e.eventId LIMIT %s` bound to `persisted_cp`
(`alertbridge.py:301`), and `persisted_cp` is assigned *only* after `save_state`
returns true, on both the suppress and the publish path. A crash therefore leaves
the on-disk checkpoint at or below the last durably-terminal event, and every higher
eventId — including the tier-4 row — is re-selected on restart. The tier-4 row cannot
be lost; the worst case is a duplicate publish, which `test_a11` asserts explicitly
(`['eventId=2', 'eventId=2']`). **E9 is correctly resolved.** This is the strongest
part of either lane.

### 4. Migration 018 (validated by application to an isolated scratch schema)

```
$ mysqldump --no-data solarinet asset node alertEvent probeTarget ... > scratch schema
$ mysql solari_review_scratch < db/migrations/018_lifecycle_criticality.sql
018 APPLIED OK
$ mysql solari_review_scratch < db/migrations/018_lifecycle_criticality.sql
018 RE-APPLY OK          (idempotent; CREATE INDEX IF NOT EXISTS works on MariaDB 11.8.6)
```

Additive-only and fully `IF NOT EXISTS`-guarded — confirmed by reading and by the
clean re-apply. Column/table names and ENUM values cross-checked against the live
`SHOW CREATE TABLE` output and against every C bind and PHP `information_schema`
probe: `asset.lifecycle` ENUM('active','decommissioned','deleted'),
`asset.criticality`, `node.criticality`, `alertEvent.{eventKind,openedEventId,assetId,
effectiveTier,disposition}`, `probeTargetTombstone.{targetId,removedAt,removedBy,assetId}`
all match exactly.

**Old-binary tolerance — verified:**

```sql
INSERT INTO alertEvent (ruleId,nodeId,targetId,firedAt,severity,detail)
  VALUES (NULL,NULL,NULL,FROM_UNIXTIME(1700000000),'warn','legacy binary row');
-- eventId 161 | eventKind 'fired' | disposition NULL | assetId NULL | effectiveTier NULL
```

An unmodified C binary inserts successfully post-018. The bridge treats
`disposition IS NULL` as legacy. D6 step (1) is safe.

I also executed the two rewritten statements I most suspected of a syntax error and
was wrong on both — recording that explicitly:

- `serverDbUpsertProbeTarget`'s `INSERT ... SELECT <consts> WHERE <pred> ON DUPLICATE
  KEY UPDATE ... VALUES(col)` **parses and behaves correctly** on MariaDB 11.8.6. I
  expected `SELECT` without `FROM DUAL` to fail and expected `VALUES()` to be
  undefined for `INSERT...SELECT`; neither is true here. The ODKU branch carried the
  SELECT row correctly (`host` → `10.9.9.9`, `label` → `CHANGED`).
- Behavioral confirmation of both new predicates:
  - tombstoned targetId → insert refused, existing row untouched (no resurrection);
  - `assetId` whose asset is `decommissioned` → `should_be_0_for_decommissioned = 0`.

So D3 (service tombstone) and D2 (write-path predicate) are genuinely enforced at the
DB layer. Note this is a *substitution* for the contract's literal wording — §3.1 asked
for the precondition in `assetSyncHeartbeat()`/`assetAddService()`, and instead both
reach it through `serverDbUpsertProbeTarget`. That is a better place for it; recording
it as a sound deviation, not a finding.

### 5. PHP

```
$ php -l  → No syntax errors detected in Operator.php, assets.php, control.php,
            discovery.php, nodes.php
```

- **Feature detection complete.** `asset_lifecycle_columns()` (assets.php:246) gates
  both the list and detail SELECTs; `node_criticality_present()` (nodes.php:234) gates
  both node SELECTs; discovery.php:52 gates the tombstone join. `asset_row()` and the
  node mappers use `array_key_exists`, so pre-migration responses omit the fields
  rather than emitting a wrong default — which is what §8 A-3 requires of consumers.
  Pre-migration 200s confirmed by construction and consistent with the Lead's live check.
- **`requireAdmin` gates purge correctly.** `Operator::requireAdmin()` requires
  `self::role() === 'admin'` exactly; an operator resolves to `'operator'` and gets 403.
  It cannot be satisfied by an operator. A5 holds for purge.
- **Discovery tombstone pre-migration.** Emits the key *absent*, not `null`. §8 A-2
  states "absent field = no notice (graceful pre-deploy)", so this is conformant —
  flagging only because the A-2 prose also says the route "computes ... `| null`".
  LC3 renders on non-null, so no behavioral gap.

### 6. Wire/authz coherence

`ASSET_PURGE` and `LIFECYCLE_SET` are both present in the destructive list at
`solariCtl.c:335` (inside the 331-334 region), so both require `op=` via
`ctlCheckRbac`, and PHP supplies it. `LIFECYCLE_SET` payload matches exactly
(`asset`, `to`). **`CRIT_SET` does not match at all — see M1.** `ASSET_PURGE`'s
`confirmName` is transported but never checked — see M4.

### 7. Carry-over items

- **Purge confirm semantics server-side: FAILS** — the check exists only in PHP, and
  the PHP chain omits `ip`. See **M4**.
- **Nameless-asset purge confirmable end-to-end: FAILS.** See **M5**.

---

## MUST-FIX

### M1 — `CRIT_SET` payload mismatch: criticality is 100% non-functional end-to-end

`dashboard/api/routes/assets.php:191` — `['scope' => 'asset', 'id' => $aid, 'tier' => $tier, 'op' => $op]`
`dashboard/api/routes/control.php:126` — `['scope' => 'node', 'id' => $nodeId, 'tier' => $tier, 'op' => $op]`
`src/server/solariCtl.c:750-757` — reads `asset=` and `node=`, never `scope=`/`id=`.

**Failure scenario.** Every criticality write. `ctlArgU64(args,"asset",…)` and
`ctlArgU64(args,"node",…)` both leave their outputs at 0, so the guard
`((assetId==0)==(nodeId==0))` evaluates true and the verb returns `ERR_INVALID_ARG`
("one entity and tier 0..4 required"), which `SolariCtl::call` surfaces as a 400
`control_error`. There is no code path by which a tier can ever be set. Feature #10's
entire write path is dead, and A4 cannot pass.

This is exactly the class of defect the §6 interface freeze was meant to prevent — the
two lanes coded against different readings of §3.3, which names the routes but not the
verb argument keys.

**Minimal fix** (PHP side; leaves the C interface as frozen):

```php
// assets.php:191
SolariCtl::call('CRIT_SET', ['asset' => $aid, 'tier' => $tier, 'op' => $op]);
// control.php:126
SolariCtl::call('CRIT_SET', ['node' => $nodeId, 'tier' => $tier, 'op' => $op]);
```

### M2 — The lifecycle cascade cannot clear a single alert that exists today

`src/server/serverDb.c` — `dbClearOpenAlerts()` matches `WHERE f.assetId=<id>`;
`serverDbClearOpenAssetAlerts()` passes the literal `"f.assetId"`.

**Failure scenario.** `alertEvent.assetId` is introduced by 018 and is NULL on every
pre-existing row. The live database has **64 open rows** (`clearedAt IS NULL`), all with
`assetId` NULL. Decommissioning the pihole asset clears none of them — the exact L2
evidence in §1 that motivated the feature.

Reproduced in scratch by running the cascade statement verbatim against a mixed row set:

```
### T2b: does the cascade clear a LEGACY (assetId NULL) open row? ###
legacy_rows_left_open    1
```

**Worse, this is not only a legacy problem.** Host-scoped alerts written by the *new*
engine never get an assetId either: `alertApply()` takes the
`serverDbNodeAlertTier()` branch for host scope, and that function selects a literal
`0` for its third column and passes `assetId = NULL` through `dbLifecycleQuery`, so
`assetId` stays 0 and binds NULL at insert. Decommissioning an asset will therefore
never clear its host-scoped alerts, before or after migration.

**Minimal fix.** Widen the predicate to the identifiers that actually exist on the
rows, evaluated before the targets are deleted (the transaction already orders
clear-then-delete, so the subqueries still resolve):

```sql
WHERE ( f.assetId = <id>
     OR f.nodeId  = (SELECT nodeId FROM asset WHERE assetId = <id>)
     OR f.targetId IN (SELECT targetId FROM probeTarget WHERE assetId = <id>) )
  AND f.eventKind = 'fired' AND NOT EXISTS (…)
```

`asset.nodeId` already exists (verified in the live schema).

### M3 — The cascade increases the open-alert count instead of dropping it

`src/server/serverDb.c` — `dbClearOpenAlerts()` inserts a new row with the fired row's
`severity` copied verbatim and `clearedAt` left NULL, and never sets `clearedAt` on the
fired row.

Every reader in the tree defines "open" as `clearedAt IS NULL`:

- `dashboard/api/routes/summary.php:45` — `SELECT COUNT(*) FROM alertEvent WHERE clearedAt IS NULL`
- `dashboard/api/routes/panel.php:280, 285, 304` — panel score, crit count, alarm
- `dashboard/api/routes/alerts.php:40` — the "active" filter

**Failure scenario.** Decommission an asset with one open crit. The cascade adds a
second crit row that is also `clearedAt IS NULL`. Open crits go 1 → 2. The panel alarm
that A1/E1/A6 promise will fall silent instead gets louder, and `topAlert` does not
advance. Confirmed in scratch:

```
eventId  eventKind  openedEventId  assetId  detail
162      fired      NULL           1        new engine fired row
163      cleared    162            1        CLEARED lifecycle new engine fired row
   -- both rows have clearedAt NULL, both count as "open" to every reader above
```

D1 reclassifies `clearedAt` as the ack field, which is a defensible model — but D1 did
not migrate the four readers that still use it, and nothing in LC1/LC2 does either.

**Minimal fix**, inside `dbClearOpenAlerts` and inside the same transaction:

1. insert the cleared row with `clearedAt = UTC_TIMESTAMP()` rather than NULL, and
2. add `UPDATE alertEvent SET clearedAt = UTC_TIMESTAMP() WHERE <same predicate>`.

**Lead adjudication required** on whether to instead migrate summary/panel/alerts to
`eventKind` semantics first. Either is fine; shipping neither is not. Note the same
gap exists on the engine's ordinary recovery path (a `cleared` row has never set
`clearedAt`) — that part is pre-existing, not a regression, but M3 makes it acute.

### M4 — `ASSET_PURGE` never validates `confirmName` server-side

`src/server/solariCtl.c:738-741` requires `confirmName` to be present and non-empty,
then never compares it to anything.

**Failure scenario.** The typed-name confirm exists only in `assets.php:172-176`. Any
caller reaching the ctl socket directly — a future route, a script, a bug in a another
lane — purges an asset with `confirmName=x`. A3's "wrong name → 400/409" is a
property of one PHP function, not of the system. Everywhere else in this file the C
bridge is the authority (`ctlCheckRbac`, the DECOMMISSION token); purge is the one
destructive verb that trusts its caller.

Related, same fix: the PHP chain is `displayName` else `host` — **`ip` is missing**,
contrary to the displayName-or-host-or-ip semantics this review was asked to confirm.

**Minimal fix.** In the `ASSET_PURGE` branch, before calling `serverAssetsRemove`,
`SELECT displayName, host, ip FROM asset WHERE assetId = ?` and require `confirmName`
to equal one of the three non-empty values; return `ERR_INVALID_ARG` otherwise. Add
`ip` to the PHP fallback chain so the two agree.

### M5 — A nameless asset cannot be purged end-to-end

`dashboard/api/routes/assets.php:174-176` / `src/server/solariCtl.c:740`.

**Failure scenario.** `asset.displayName` and `asset.host` are both nullable (verified
in the live schema); a discovery-only asset can have neither. `$expectedName` then
resolves to `''`, PHP accepts `{"confirmName": ""}` (it is a string and it matches),
and forwards it. C rejects it at `|| !confirmName[0]` with `ERR_INVALID_ARG` → 400.
The asset is permanently un-purgeable, and the error message ("asset and confirmName
required") gives the operator no way to work it out.

**Minimal fix.** Fall back to `ip`, which is `NOT NULL` and `UNIQUE` on `asset`. This
closes M5 and M4's `ip` gap in one change:

```php
$expectedName = (string) ($cur['displayName'] ?: $cur['host'] ?: $cur['ip']);
```

### M6 — Maintenance-window suppression is silently disabled for every new engine row

`deploy/alertbridge/alertbridge.py` — `resolve_outcome()` returns `"publish"` on
`row["disposition"] == "publish"` before `_suppressed()` is ever consulted.

**Failure scenario.** Tier 2 is the default for every entity, and the engine writes
`disposition='publish'` for every tier ≥ 2. So from the moment the new C server starts
(D6 step 3), the `maintenanceWindow` table stops suppressing anything at all. Put a
host into a maintenance window, reboot it, and it pages. Per §1 this is currently the
*only* suppression mechanism that exists.

D4 says the bridge decides "purely from disposition/eventKind" — but that sentence is
about removing the tier lookup and the id-space parsing, not about deleting a
production feature. Nothing in §7 proposes dropping maintenance windows.

**Minimal fix** — keep maintenance as an independent, later gate:

```python
if row.get("disposition") == "publish":
    return "suppress" if _suppressed(row.get("nodeId"), row.get("targetId"), maint) else "publish"
```

### M7 — Cleared rows do not inherit the fired row's disposition (engine path)

`src/server/serverAlert.c`, clear branch of `alertApply()` — passes
`tier <= 1 ? "suppress" : "publish"` recomputed from the *current* tier.

§7 D4 is explicit and binding: "cleared rows INHERIT the fired row's disposition via
openedEventId."

**Failure scenario.** An incident fires at tier 3 and pages. An operator lowers the
entity to tier 1 while it is still down. On recovery the cleared row is written
`suppress`, the bridge suppresses it, and the operator is never told the incident
ended — A12 ("recovery published exactly once and only for a previously published
incident") fails in both directions: the mirror case (tier 1 → 3 mid-incident)
publishes a recovery for an incident nobody ever saw.

The cascade path gets this right (`dbClearOpenAlerts` selects `f.disposition`); only
the engine path recomputes.

**Minimal fix.** `alertBreachState` already carries `openedEventId` (serverAlert.c:57).
Add `char openedDisposition[16]` beside it, set it on the fired write, and pass it
verbatim on the clear.

### M8 — No test covers the cascade transaction; the mutation was not caught

Mandatory mutation (a): the failure path of `serverDbLifecycleTransition()` was changed
from `mysql_rollback(conn)` to `mysql_commit(conn)` and the whole suite rebuilt.

```
100% tests passed, 0 tests failed out of 25
```

`tests/unit/server_stubs.c:61-66` provides `WEAK` stubs for
`serverDbLifecycleTransition`, `serverDbPurgeAsset`, `serverDbClearOpenAssetAlerts`,
`serverDbClearOpenNodeAlerts`, `serverDbClearAssetTombstones` and
`serverDbSetAssetLifecycle`; nothing anywhere links the real implementations.
`tests/integration/test_server_db_live.c` — the one DB-backed target — was touched only
to fix the `serverDbWriteAlertEvent` signature.

**Failure scenario.** A partially-applied transition ships undetected: alerts cleared
and probe targets deleted, but `asset.lifecycle` still `'active'`. The asset then looks
live in every list while having no probes and no alerts — a silent monitoring blind
spot, which is the failure mode this whole feature exists to eliminate. F1 is the
headline claim of the FIX ROUND and it is entirely unexercised; RETURN-LC1's own
UNVERIFIED section concedes "transaction rollback/commit-error paths … remain
unexercised", and per the review mandate an uncaught mutation is itself a MUST-FIX.

**Minimal fix.** Add lifecycle + purge cases to `tests/integration/test_server_db_live.c`
(it already self-skips without `SOLARI_TEST_DB`, so CI is unaffected): seed an asset with
two targets and one open fired row; force a failure mid-cascade; assert lifecycle is
unchanged, both target rows still present, and no cleared row was written. Then re-run
mutation (a) and confirm it fails.

### M9 — SCP PUB is now filtered, contrary to D5

`src/server/serverAlert.c`, top of `alertApply()`:

```c
if (st != SOLARI_OK || !active || !alertComposeSeverity(r->severity, tier, &severity))
    return SOLARI_OK;
```

This sits above everything, so tier-0 and non-active entities skip `alertPublish()`
along with the DB insert.

§7 D5 is binding: "alertPublish() (SCP PUB) also carries alarm data to opied
(remediation) — **it must keep seeing everything**. SCP PUB stays unfiltered (machine
consumers decide themselves)."

**Failure scenario.** opied loses remediation visibility for precisely the tier-0
("ignore") entities where unattended auto-remediation is most appropriate — the tier
means "don't page me", not "don't fix it". §3.2 also defines tier 0 as suppressing
*the insert*, not the publish. The deferral of F4 (the effectiveTier wire byte) removed
opied's ability to filter for itself, which makes dropping the publish strictly worse
than the status quo.

**Minimal fix.** Gate only `serverDbWriteAlertEvent` on the tier/lifecycle decision;
leave `alertPublish` on the pre-existing unconditional path.

---

## SHOULD

### S1 — `alertApply` fails silently and permanently open on a DB error
`src/server/serverAlert.c`, same early return as M9. A transient `ERR_DB` from
`serverDbNodeAlertTier`/`serverDbProbeAlertTier` disables all alerting for that report,
with no log line and an `SOLARI_OK` return. Before this change alerting had no
dependency that could fail this way. Log the error, and make the policy explicit —
recommend degrading to `tier=2, active=true` on lookup failure so a DB hiccup cannot
black out monitoring.

### S2 — Probe alerting dies silently for any probeTarget with no asset
`src/server/serverDb.c`, `serverDbProbeAlertTier`: `FROM probeTarget p JOIN asset a ON
a.assetId = p.assetId`. `probeTarget.assetId` is nullable (confirmed in the live
schema, and `serverDbUpsertProbeTarget` binds NULL when `assetId == 0`). No row → the
fetch does not fire → `active` stays false → no alerts, no log. Live DB today: 0 of 23
probeTargets have a NULL assetId, so this is **latent, not active** — but the first
manually-added external service check silently turns off its own alerting. Use
`LEFT JOIN` and default to active/tier-2 when there is no owning asset.

### S3 — Node retire clears alerts before the retire can fail
`src/server/solariCtl.c:966-968` runs `serverDbClearOpenNodeAlerts()` and only then
`serverProvisionRetire()`. If retire fails, the node stays live with its open alerts
already cleared. Reorder, or wrap both in the transaction idiom F1 introduced.

### S4 — `CRIT_SET` is not in the RBAC list
`ctlVerbRequiresOperator()` (`solariCtl.c:339-342`) omits `CRIT_SET`, so `op=` is
unenforced at the C layer. Setting an entity to tier 0 silences it completely — a
security-relevant mutation. PHP does gate it with `requireOperator()`, so A5 passes
today, but every other mutation in this file is authoritative at the bridge. Add
`CRIT_SET` to `ctlVerbRequiresOperator` (not to `ctlVerbIsDestructive` — it wants the
named operator, not a confirm token).

### S5 — Audit rows are written with `disposition='publish'`
`serverAssets.c:262`, `serverProvision.c:344, 388, 502, 558`, `solariCtl.c:707, 799, 860`.
A13 holds only because `resolve_outcome()` happens to test `eventKind == "audit"` first.
The row's own column asserts the opposite of the intent, and A13 is one refactor away
from an audit-row leak to MQ. Write `NULL` or `'suppress'` for audit rows and let
defence in depth do its job.

### S6 — The id-space helpers are dead code
`src/server/serverDb.c:184-210` — `dbNumericAlertIdToProbeId` and
`dbProbeIdToNumericAlertId` are both `__attribute__((unused))` with zero callers; D4
correctly replaced their intended use with `alertEvent.assetId`. §3.4/A9's "ONE shared
helper … for the cascade" now describes code nothing calls. Delete them with their
tests, or document in the header comment that they are retained for the deferred F4
work. Also note the IPv6 form is ambiguous by construction — `"3:2001:db8::2:53"` is
indistinguishable from a portless IPv6 address, and only proto 1's special case saves
the icmp path. Harmless while unused; a trap if ever wired up.

### S7 — Unit assertions run outside a Unity test frame
`tests/unit/test_server_db.c:169-181` — the id-helper assertions sit in a bare block in
`main()` rather than inside a `RUN_TEST`. Mutation (b) *was* caught, but Unity reported
it as `test_probe_target_id:FAIL` at line 176, because `TEST_ASSERT` longjmps to
whichever `AbortFrame` the previous `RUN_TEST` left behind. The suite is also one
reordering away from that frame being invalid. Wrap them in a named test function and
`RUN_TEST` it, so A9 has a correctly-attributed test.

### S8 — 018 adds no index for the columns the cascade queries
`dbClearOpenAlerts` scans `alertEvent` on `assetId`/`nodeId` with a correlated
`NOT EXISTS` on `openedEventId`; none of the three is indexed (confirmed against the
live `SHOW CREATE TABLE alertEvent` — only `firedAt` and `nodeId` have keys). 159 rows
today, so it is invisible now. Add `KEY (assetId)` and `KEY (openedEventId)` to 018
while the migration is still unapplied and the table is small.

---

## NIT

**N1 — `ctlHandleLine` stack frame grew to ~72 KB.** `objdump` shows
`lea -0x12000(%rsp),%r11` (73,728 bytes), from the two `char targets[512]
[SERVER_TARGETID_MAX]` arrays (64 KB each; the compiler kept both live).
`serverAssetsRemove` adds another 64 KB (`lea -0x10000(%rsp)`) nested under
ASSET_PURGE — ~137 KB peak. It runs on the main loop (`solariCtl.c:1133`; no
`pthread_create` in this file), so the 8 MB main stack absorbs it comfortably. Worth
heap-allocating or sharing one buffer if the ctl loop is ever threaded.

**N2 — RETURN-LC1's `CREATE INDEX IF NOT EXISTS` caveat is now closed.** Listed as
UNVERIFIED in the packet ("not validated against a live target"); verified here —
applies and re-applies cleanly on MariaDB 11.8.6, which is the version the live server
runs.

**N3 — `serverDbUpsertProbeTarget` no-ops silently when refused.** When the lifecycle or
tombstone predicate rejects, the statement affects 0 rows and returns `SOLARI_OK`. That
is the intended behavior, but the heartbeat gets no signal it was refused. A debug log
line would make A1 and A8 far easier to observe in the field.

---

## Observations outside the review scope

- **`dashboard/api/routes/panel.php` changed during this review window.** It was clean
  in `git status` when I started and now carries a 36-line diff — the tier-weighting
  patch re-applied by someone else mid-review. Per instructions I did not review or
  modify it. One courtesy check only: the `information_schema` guard is present this
  time (`SELECT COUNT(*) … COLUMN_NAME = 'criticality'`), so the failure mode that
  caused the 5-hour live 500 appears addressed. Someone who owns the file should still
  verify it, and note that **M3 lands directly on its `clearedAt IS NULL` queries at
  lines 280/285/304** — panel tier weighting cannot deliver A1/A6 until M3 is resolved.
- The `feat/lifecycle-criticality` tree also carries uncommitted LC3 changes
  (`dashboard/public/*`) and `status-panel/RETURN-CP1.md`; not in scope, not examined.

## Verification hygiene

- Mutations applied and reverted: 3 in C (`serverDb.c` ×2, `serverAlert.c` ×1), 2 in
  Python (`alertbridge.py`). After each revert the tree was rebuilt; final
  `git diff --stat` for every in-scope file matches its pre-review state exactly.
- Database: reads only against live `solarinet` (`SHOW CREATE TABLE`, five `COUNT(*)`
  metrics). All writes went to `solari_review_scratch`, created from a `--no-data`
  dump and dropped at the end of the review.
- No git operations, no deployment, no service restart.

---

# RE-CHECK (round 2) — after LC1 FIX ROUND 2 + Lead close-out

`reviewer: review-lc12 (Claude) · 2026-08-06 · tree: feat/lifecycle-criticality, uncommitted`
`hygiene: 6 mutations applied and reverted (md5-verified byte-identical); scratch schema
solari_review_r2 created and dropped; live solarinet read-only; no git ops, no deploy; panel.php untouched.`

## Verdicts

| Component | Round 1 | Round 2 | Note |
|---|---|---|---|
| `db/migrations/018` | SHIP | **SHIP** | S8 indexes present; no FK on `alertEvent.assetId` (purge cannot be blocked) |
| `serverDb.c` — cascade | FIX-THEN-SHIP | **FIX-THEN-SHIP** | R2-M1: Fix-B's legacy arm is a no-op (below) |
| `serverDb.c` — rest | FIX-THEN-SHIP | **SHIP** | transaction now mutation-caught; confirm-values getter correct |
| `serverAlert.c` | FIX-THEN-SHIP | **SHIP** | Fix-D and Fix-E verified behaviorally |
| `solariCtl.c` | FIX-THEN-SHIP | **SHIP** | wire + RBAC verified both directions |
| `test_server_db_live.c` | (new) | **SHIP** | production refusal verified both negative cases |
| `alertbridge.py` | SHIP | **SHIP** | M6 fix correct; regression test missing (R2-S1) |
| PHP routes | mixed | **SHIP** | keys frozen to the C shape; purge chain includes `ip` |

**Deployment blocker: one — R2-M1.** Everything else from round 1 is closed.

## R2-M1 (MUST-FIX) — the M2 "legacy suffix-match arm" is not a suffix match; it matches nothing

`src/server/serverDb.c:944-947` (`serverDbClearOpenAssetAlerts`)

The predicate's third arm is an exact-equality `IN`:

```
f.targetId IN (SELECT targetId FROM probeTarget WHERE assetId=<id>)
```

`alertEvent.targetId` and `probeTarget.targetId` are the two DIFFERENT id-spaces of
§3.4. Live `solarinet`, read-only:

```
alertEvent.targetId (open):   2:10.0.0.1:53    1:10.0.0.1:0
probeTarget.targetId:         tcp:10.0.0.1:53  icmp:10.0.0.1
```

The arm therefore never matches a legacy row. The other two arms are dead as well on
today's data: `f.assetId` is NULL on all 64 currently-open rows (pre-018 writes), and
`f.nodeId=(SELECT nodeId FROM asset ...)` compares against NULL — `asset.nodeId` is NULL
for all 9 live assets, so that arm yields UNKNOWN, never true.

Measured on live (read-only, arms 2+3 only, since 018 is not applied there):

```
arm3 match asset 3 (has targets)      0
open alerts for asset3 ip            18
arm2+arm3 match, asset 8              0
```

Repro on a scratch clone of the stage schema with 018 applied, running the shipped
INSERT+UPDATE pair verbatim for assetId=3:

```
BEFORE                                    AFTER
eventId targetId        assetId kind      eventId targetId        kind     stillOpen
1       2:10.0.0.1:53   NULL    fired  →  1       2:10.0.0.1:53   fired    1   <-- legacy, still open
2       1:10.0.0.1:0    NULL    fired  →  2       1:10.0.0.1:0    fired    1   <-- legacy, still open
3       tcp:10.0.0.1:53 3       fired  →  3       tcp:10.0.0.1:53 fired    0
                                          4       tcp:10.0.0.1:53 cleared  0
still-open count after cascade: 2
```

**Failure scenario.** Decommissioning any asset that has open alerts written before the
new C server starts leaves those alerts open forever — the exact M2 failure, unfixed. It
does not bite the pihole pair (assets 8/9 have no targets and no open rows left after the
interim `monitorHost=0`), so DEPLOY-LC step 7 will *look* like it proves A1. It bites
asset 3 (chemistry), which has 18 open rows in the numeric space right now.

**Minimal fix** — add a fourth arm reconstructing the numeric id from `probeTarget`
(validated on the fixture; matches both rows above, including the `icmp …:0` form):

```sql
OR EXISTS (SELECT 1 FROM probeTarget p WHERE p.assetId=<id> AND f.targetId = CONCAT(
     CASE p.proto WHEN 'icmp' THEN '1' WHEN 'tcp' THEN '2' ELSE '3' END,
     ':', p.host, ':', COALESCE(p.port,0)))
```

`serverDbPurgeAsset()` shares `serverDbClearOpenAssetAlerts()`, so the same fix covers purge.

## Verified fixed

**M8 / cascade transaction — now genuinely caught.** `test_lifecycle_transition_rolls_back_cascade`
against `solarinet_stage`; baseline `8 Tests 0 Failures OK`. With `mysql_rollback` →
`mysql_commit` at `serverDb.c:882`:

```
serverDb: execute failed: Data truncated for column 'lifecycle' at row 1
test_lifecycle_transition_rolls_back_cascade:FAIL: Expected 2 Was 0
8 Tests 1 Failures 0 Ignored / FAIL
```

The `Data truncated` line confirms Fix-F's invalid-ENUM injection actually fires —
`@@sql_mode` on this server includes `STRICT_TRANS_TABLES`, so the test is not vacuous.
(Flagged as a risk before running; empirically not one here. It *is* an environmental
dependency: on a non-strict server this case would pass without testing anything.)

**Production-schema refusal — verified in code and both negative directions**, and it
precedes `serverDbOpen` (guard at `tests/integration/test_server_db_live.c:268`, open at :282):

```
SOLARI_TEST_DB=1                        → FATAL … refusing the production schema "solarinet"   rc=1
SOLARI_TEST_DB=1 SOLARI_DB_NAME=solarinet → FATAL … (same)                                     rc=1
(no env)                                → SKIP test_server_db_live: SOLARI_TEST_DB unset       rc=0
```

**M3 / clearedAt — closed.** In the fixture above, the cascade's cleared row (eventId 4)
is born with `clearedAt` set and the fired row it closes is stamped in the same statement
pair. No row is open under the dashboard's `clearedAt IS NULL` definition. Fix-C correct.

**M1 / CRIT_SET wire — closed, both directions.** PHP `SolariCtl::frame()` output fed
through the real C parser (`ctlSplitVerb` + `ctlArgU64`/`ctlArgStr`, compiled against
`solariCtl.c`):

```
CRIT_SET asset=9 tier=4 op=jason          → requiresOp=1 asset=9 tier=4 op=[jason]
CRIT_SET node=12 tier=0 op=jason          → requiresOp=1 node=12 tier=0 op=[jason]
LIFECYCLE_SET asset=9 to=decommissioned…  → destructive=1 asset=9 to=[decommissioned]
ASSET_PURGE asset=9 confirm=My%20Router%202 op=jason → confirm=[My Router 2]
ASSET_PURGE asset=9 confirm=10.1.0.254 op=jason      → confirm=[10.1.0.254]
ASSET_PURGE asset=9 confirm=100%2520pct op=jason     → confirm=[100%20pct]
```

`tier=0` survives the round trip (not confused with "absent"), `CRIT_SET` is
operator-gated, and `LIFECYCLE_SET`/`ASSET_PURGE` are in the destructive set. The
`rawurlencode`/`ctlPctDecode` pair is symmetric including a literal `%`. No collision with
the DECOMMISSION `confirm=<token>` protocol — that key is parsed inside the DECOMMISSION
branch only, as a u64.

**Purge confirm carry-over items — closed.** `assets.php:172-177` chains
displayName ‖ host ‖ ip; `asset.ip` is `NOT NULL UNIQUE` on live, so a nameless asset is
confirmable by IP end-to-end (wire round trip above). `solariCtl.c:745-748` re-verifies
server-side against all three, accepting any of them.

**Fix-D and Fix-E — verified behaviorally**, not just by reading. Probe linking the real
`serverAlert.c` against instrumented DB stubs:

```
Fix-E tier 0:        fire dbWrites=0 publishes=1 · clear dbWrites=+0 publishes=+1   (D5 holds)
Fix-E tier 1:        kind=fired sev=warn tier=1 disp=suppress publishes=1
Fix-E tier 4:        kind=fired sev=crit tier=4 disp=publish  publishes=1   (rule severity was "info")
Fix-D inheritance:   fire  kind=fired  tier=3 disp=publish  opened=0
                     clear kind=cleared tier=1 disp=suppress opened=4242
                     (tier raised 3→4 between fire and clear; the cleared row did NOT recompute)
D2 inactive entity:  fire dbWrites=0 publishes=0
```

**M6 / bridge maintenance gate — fix correct.** Own probe, 8/8:

```
tier4 publish row + fleet maintenance   → suppress      cleared row inherits publish + maint → suppress
tier4 publish row + node maintenance    → suppress      legacy NULL row + node maintenance   → suppress
tier4 publish row, no maintenance       → publish       legacy NULL row, no maintenance      → publish
suppress row → suppress                                 audit row → suppress
```

## Mutation results (6 applied, all reverted)

| # | Mutation | Caught? | By |
|---|---|---|---|
| 1 | `serverDb.c:882` `mysql_rollback` → `mysql_commit` | **yes** | `test_lifecycle_transition_rolls_back_cascade` |
| 2 | `serverAlert.c` scoped drop → drop-all | **yes** | `test_drop_target_state_is_scoped` |
| 3 | id helper: `strrchr` → `strchr` | n/a | equivalent mutant — split/rejoin is lossless, not a gap |
| 4 | id helper: `dbProbeIdToNumericAlertId` tcp → proto 3 | **no** | R2-S2 |
| 5 | bridge: restore the M6 disposition short-circuit | **no** | R2-S1 |
| 6 | bridge: advance `persisted_cp` before `save_state` | **yes** | E9 checkpoint test |
| 7 | bridge: drop the `eventKind=='audit'` force-suppress | **yes** | A13 |

## SHOULD / NIT (round 2)

- **R2-S1 (SHOULD)** — no test covers the M6 property. Mutation 5 restores the exact bug
  round 1 found and `test_dispositions.py` stays 4/4 green. Six lines: a publish-disposition
  row under a node maintenance window must resolve to `suppress`.
- **R2-S2 (SHOULD)** — `tests/unit/test_server_db.c:121-134` asserts the reverse conversion
  only for the `icmp` round trip; mutating the `tcp` mapping to proto 3 is invisible. Both
  helpers are `__attribute__((unused))` dead code held for deferred F4, so this is not a
  live-path gap — but if F4 lands on this coverage the mapping bug ships silently.
- **R2-S3 (SHOULD)** — `serverAssetsRemove()` writes its audit row (`serverAssets.c:262`)
  *before* `serverDbPurgeAsset()` opens its transaction. A rolled-back purge leaves a
  durable "asset removed by …" audit row for a purge that did not happen.
- **R2-N1 (NIT)** — `serverDbGetAssetConfirmValues()` returns `SOLARI_OK` with empty
  strings for a nonexistent assetId, so the caller reports "purge confirmation does not
  match asset" rather than "no such asset". Fails closed; the message misleads.
- **R2-N2 (NIT)** — the human-readable `alert: FIRED …` log line carries the *rule's* raw
  severity while the DB row and PUB frame carry the tier-composed one (`sev=crit` logged
  for a tier-1 alert persisted as `warn`).

---

## RE-VERIFY (round 3) — R2-M1 + R2-S1 + R2-S3 · **SHIP**

`reviewer: review-lc12 · 2026-08-06 · fixture rebuilt and dropped; alertbridge.py mutated and reverted (md5 identical)`

**R2-M1 closed.** The four-arm predicate as shipped (`serverDb.c` `serverDbClearOpenAssetAlerts`,
buffer 1024 — max rendered length ~410 bytes, and `dbClearOpenAlerts`'s `sql[2048]` holds
the largest composed statement at ~890) run verbatim against the same legacy fixture:

```
BEFORE                                        AFTER
1  2:10.0.0.1:53    NULL  fired  open      1  2:10.0.0.1:53    fired    closed
2  1:10.0.0.1:0     NULL  fired  open      2  1:10.0.0.1:0     fired    closed
3  tcp:10.0.0.1:53  3     fired  open      3  tcp:10.0.0.1:53  fired    closed
                                           4  2:10.0.0.1:53    cleared  openedEventId=1
                                           5  1:10.0.0.1:0     cleared  openedEventId=2
                                           6  tcp:10.0.0.1:53  cleared  openedEventId=3
still-open count after cascade: 0   (was 2)
```

Both legacy id-space forms now clear, including the `icmp …:0` form; every cleared row is
linked to its fired row. Re-running the cascade a second time adds nothing (the
`NOT EXISTS` guard holds) — 6 rows, still-open 0.

**R2-S1 closed.** `test_dispositions.py` 5/5. Restoring the round-1 short-circuit in
`resolve_outcome()` now fails the suite (`FAILED (failures=1)`) — the regression is guarded.

**R2-S3 closed.** `serverAssets.c` writes the audit row after `serverDbPurgeAsset()`
commits, with log-loudly-but-succeed on audit failure. Correct: the purge did happen.

**Gates:** C rebuild 0 warnings/errors · ctest 25/25 · stage suite 8/8 · bridge 5/5.

**Verdict: SHIP — all components.** No open MUST-FIX. Remaining from round 2: R2-S2
(dead-code id-helper coverage, deferred with F4) and the two NITs.

Deploy note carried forward: DEPLOY-LC step 7 should exercise A1 on asset 3 (chemistry,
18 open legacy rows), not the piholes — assets 8/9 are already quiet and would pass
vacuously.
