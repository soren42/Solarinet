# RETURN-LC1

## STATUS

partial

## ARTIFACTS

db/migrations/018_lifecycle_criticality.sql
src/server/server.h
src/server/serverDb.c
src/server/serverAlert.c
src/server/serverProvision.c
src/server/solariCtl.c
tests/unit/test_server_alert.c
tests/unit/test_server_db.c
tests/unit/server_stubs.c
tests/integration/test_server_db_live.c
status-panel/RETURN-LC1.md

## VERIFIED

`cmake -S . -B build -DSOLARI_BUILD_SERVER=ON -DSOLARI_WITH_IO=ON -DSOLARI_BUILD_TESTS=ON && cmake --build build -j2`

Full final build output:

```text
[  5%] Built target solariCtlClient
[ 12%] Built target solari
[ 17%] Built target unity
[ 18%] Built target clientcore
[ 23%] Built target monitorcore
[ 38%] Built target servercore
[ 42%] Built target test_tlv
[ 42%] Built target test_crypto
[ 46%] Built target test_msg
[ 46%] Built target test_frame
[ 50%] Built target test_json
[ 50%] Built target test_config
[ 54%] Built target test_net_loopback
[ 55%] Built target test_platlinux
[ 58%] Built target test_client_control
[ 58%] Built target test_watchdog
[ 61%] Built target test_probe
[ 61%] Built target test_hrw
[ 64%] Built target test_monitor_control
[ 64%] Built target test_peers
[ 69%] Built target test_server_ingest
[ 71%] Built target test_server_db
[ 75%] Built target test_server_provision
[ 77%] Built target test_server_alert
[ 81%] Built target test_server_topology
[ 83%] Built target test_server_topo_snmp
[ 86%] Built target test_server_discovery
[ 88%] Built target test_server_ctl
[ 90%] Built target test_server_db_live
[ 90%] Built target test_server_snmp
[ 94%] Built target test_server_discovery_enrich
[ 94%] Built target solariClient
[ 98%] Built target solariMonitor
[100%] Built target solariServer
[100%] Built target solariSnmpPoll
```

The final build emitted no warnings from the touched LC1 files. Earlier clean-build compilation exposed pre-existing warnings in `serverTopology.c` and `serverSnmp.c`; those files were outside scope and were not changed.

`ctest --test-dir build --output-on-failure`

```text
Test project /home/jason/Code/Solarinet/build
1/25 through 6/25: Passed
7/25 test_net_loopback: Failed — listen tcp://127.0.0.1:8799 failed: Permission denied
8/25 test_platlinux: Failed — host net/discovery collection returned -70
9/25 through 11/25: Passed
12/25 test_probe: Failed — sandbox TCP probe expectations could not connect
13/25 through 25/25: Passed

88% tests passed, 3 tests failed out of 25
```

All server tests passed: `test_server_db`, `test_server_ingest`, `test_server_provision`, `test_server_alert`, `test_server_topology`, `test_server_topo_snmp`, `test_server_discovery`, `test_server_ctl`, `test_server_db_live` (self-skipped without `SOLARI_TEST_DB`), `test_server_snmp`, and `test_server_discovery_enrich`.

`rg -n "serverDbWriteAlertEvent\\(" src/server/*.c`

```text
All alert-event writers route through serverDbWriteAlertEvent; its INSERT now binds eventKind, openedEventId, assetId, effectiveTier, and disposition. Fired, cleared, and audit call sites pass explicit values.
```

Lifecycle predicate verified in `serverDbUpsertProbeTarget` with SQL `EXISTS (SELECT 1 FROM asset WHERE assetId = ? AND lifecycle = 'active')`, and in alert evaluation through fresh `serverDbNodeAlertTier`/`serverDbProbeAlertTier` reads before every transition write. Discovery adoption checks the existing asset by IP and writes `ignored` for a non-active lifecycle.

Migration syntax was manually reviewed only. It follows the existing MariaDB additive idiom (`ADD COLUMN IF NOT EXISTS`, `CREATE TABLE IF NOT EXISTS`); no database was contacted or modified.

## UNVERIFIED

- Migration 018 was not applied to any database, scratch or live.
- No lifecycle, purge, criticality, target-tombstone, or node-retire verb was exercised against a live server/database.
- The lifecycle cascade is not implemented as one explicit MariaDB transaction; it requires further work before the E6 guarantee can be claimed.
- `probeTargetTombstone` clearing after probe targets have been deleted is incomplete: the specified tombstone schema does not retain an asset ID, so restoration needs a durable asset-to-target lookup/cleanup design.
- `serverAlertDropAssetState()` conservatively clears target-scoped state rather than identifying only the passed asset; this needs a target-to-asset association in the in-memory state for exact behavior.
- D5 effective-tier SCP wire-byte support was not implemented or round-tripped; it needs the shared `solariMsg`/TLV surface, outside the requested server-file scope.
- The full CTest suite did not pass: sandbox restrictions caused `test_net_loopback`, `test_platlinux`, and `test_probe` failures as quoted above.
- PHP, Python, dashboard/public, bridge/MQ behavior, and live subscriber compatibility were not tested (out of scope).

## DEVIATIONS

- §7 D1 was followed over the superseded `clearedAt` cascade: clearing writes new `eventKind='cleared'` rows using `openedEventId` and inherited snapshots.
- I added small compatibility changes to `tests/unit/server_stubs.c` and `tests/integration/test_server_db_live.c`, despite their absence from the literal scope, because changing the required public alert-write signature otherwise made the mandatory project test build fail. No production behavior was added there.
- The id-space conversion helpers live privately in `serverDb.c`; tests were folded into the existing `test_server_db.c`, permitted by the request, rather than adding a new CMake target.
- `CREATE INDEX IF NOT EXISTS` was used in 018 because the requested MariaDB target supports it; this was not validated against a live target.

## FIX ROUND

### STATUS

complete — F1, F2, and F3 are implemented and compiled/tested as specified.

### ARTIFACTS

db/migrations/018_lifecycle_criticality.sql
src/server/server.h
src/server/serverDb.c
src/server/serverAssets.c
src/server/serverAlert.c
src/server/solariCtl.c
tests/unit/server_stubs.c
tests/unit/test_server_alert.c
status-panel/RETURN-LC1.md

### VERIFIED

F1 — `serverDbLifecycleTransition()` now owns the LIFECYCLE_SET SQL cascade on a single MariaDB connection: it disables autocommit, clears open asset alerts and deletes all listed targets (or clears restore tombstones), updates lifecycle, then commits; every failed transactional step rolls back, restores autocommit, and returns `ERR_DB`, including a failed commit. `solariCtl.c` lists target IDs before calling this helper and drops corresponding in-memory alert state only after a successful commit. `serverAssetsRemove()` did not previously transact its delete cascade, so its alert-clear/probe-state purge/target deletes/asset delete now run in `serverDbPurgeAsset()` under the same transaction idiom; ASSET_PURGE lists targets before removal and drops only those states after success. Static grep confirmed the helper calls and all `mysql_autocommit`/commit/rollback paths. The final source-state rebuild and server control tests passed.

F2 — migration 018 now creates/adds nullable `probeTargetTombstone.assetId`. `serverDbTombstoneProbeTarget()` resolves it from the live `probeTarget` row in its `INSERT ... SELECT` before that target is deleted and refreshes it on duplicate; `serverDbClearAssetTombstones()` now directly deletes by the stored asset ID. Static grep confirmed all three locations; build and server DB tests passed.

F3 — replaced the asset-wide alert-state clear with `serverAlertDropTargetState(const char *)`, which clears every matching target slot and leaves unrelated slots intact. Both lifecycle and purge paths call it once per pre-captured target after successful database work. `test_drop_target_state_is_scoped` creates fired states for two targets, drops one, and confirms the other's `inUse`, `fired`, `breachSinceMs`, and `haveLast` are unchanged.

`cmake -S . -B build -DSOLARI_BUILD_SERVER=ON -DSOLARI_WITH_IO=ON -DSOLARI_BUILD_TESTS=ON && cmake --build build -j2`

Full build output:

```text
-- solariServer: CSR signing enabled (mbedTLS x509)
-- solariServer: MariaDB libs=mariadb
-- SolariNet config: tests=ON io=ON sqlite=OFF server=ON
-- Configuring done (0.0s)
-- Generating done (0.1s)
-- Build files have been written to: /home/jason/Code/Solarinet/build
[  2%] Built target solariCtlClient
[ 12%] Built target solari
[ 14%] Built target unity
[ 18%] Built target clientcore
[ 23%] Built target monitorcore
[ 24%] Building C object src/server/CMakeFiles/servercore.dir/serverContext.c.o
[ 25%] Building C object src/server/CMakeFiles/servercore.dir/serverDb.c.o
[ 25%] Building C object src/server/CMakeFiles/servercore.dir/serverIngest.c.o
[ 26%] Building C object src/server/CMakeFiles/servercore.dir/serverLease.c.o
[ 27%] Building C object src/server/CMakeFiles/servercore.dir/serverMaster.c.o
[ 28%] Building C object src/server/CMakeFiles/servercore.dir/serverAlert.c.o
[ 30%] Built target test_crypto
[ 31%] Building C object src/server/CMakeFiles/servercore.dir/serverControl.c.o
[ 32%] Building C object src/server/CMakeFiles/servercore.dir/serverProvision.c.o
[ 34%] Built target test_tlv
[ 36%] Built target test_frame
[ 37%] Building C object src/server/CMakeFiles/servercore.dir/serverDiscovery.c.o
[ 38%] Building C object src/server/CMakeFiles/servercore.dir/serverScan.c.o
[ 39%] Building C object src/server/CMakeFiles/servercore.dir/serverAssets.c.o
[ 40%] Building C object src/server/CMakeFiles/servercore.dir/serverTopology.c.o
[ 42%] Built target test_msg
/home/jason/Code/Solarinet/src/server/serverTopology.c:797:15: warning: ‘topoLldpFinalize’ defined but not used [-Wunused-function]
/home/jason/Code/Solarinet/src/server/serverTopology.c:765:12: warning: ‘topoLldpMergeLocPort’ defined but not used [-Wunused-function]
/home/jason/Code/Solarinet/src/server/serverTopology.c:744:12: warning: ‘topoLldpMergeRecord’ defined but not used [-Wunused-function]
/home/jason/Code/Solarinet/src/server/serverTopology.c:621:12: warning: ‘topoLldpParseLocLine’ defined but not used [-Wunused-function]
/home/jason/Code/Solarinet/src/server/serverTopology.c:589:12: warning: ‘topoLldpParseRemLine’ defined but not used [-Wunused-function]
[ 44%] Built target test_config
[ 46%] Built target test_json
[ 48%] Built target test_net_loopback
[ 48%] Building C object src/server/CMakeFiles/servercore.dir/serverSnmp.c.o
[ 49%] Building C object src/server/CMakeFiles/servercore.dir/solariCtl.c.o
/home/jason/Code/Solarinet/src/server/serverSnmp.c:145:12: warning: ‘snmpPrevSample’ defined but not used [-Wunused-function]
/home/jason/Code/Solarinet/src/server/serverSnmp.c:99:13: warning: ‘snmpSanitize’ defined but not used [-Wunused-function]
[ 52%] Built target test_platlinux
[ 54%] Built target test_watchdog
[ 55%] Built target test_client_control
[ 57%] Built target test_hrw
[ 58%] Linking C static library libservercore.a
[ 59%] Built target test_probe
[ 61%] Built target test_peers
[ 62%] Built target test_monitor_control
[ 63%] Building C object tests/CMakeFiles/test_server_db.dir/unit/server_stubs.c.o
[ 65%] Built target servercore
[ 66%] Building C object tests/CMakeFiles/test_server_db.dir/unit/test_server_db.c.o
[ 67%] Building C object tests/CMakeFiles/test_server_ingest.dir/unit/server_stubs.c.o
[ 68%] Building C object tests/CMakeFiles/test_server_ingest.dir/unit/test_server_ingest.c.o
[ 69%] Linking C executable test_server_ingest
[ 69%] Built target test_server_ingest
[ 70%] Linking C executable test_server_db
[ 71%] Building C object tests/CMakeFiles/test_server_provision.dir/unit/server_stubs.c.o
[ 71%] Built target test_server_db
[ 72%] Building C object tests/CMakeFiles/test_server_provision.dir/unit/test_server_provision.c.o
[ 73%] Building C object tests/CMakeFiles/test_server_alert.dir/unit/server_stubs.c.o
[ 74%] Building C object tests/CMakeFiles/test_server_alert.dir/unit/test_server_alert.c.o
[ 75%] Linking C executable test_server_provision
[ 75%] Built target test_server_provision
[ 76%] Building C object tests/CMakeFiles/test_server_topology.dir/unit/server_stubs.c.o
[ 77%] Linking C executable test_server_alert
[ 78%] Built target test_server_topology
[ 78%] Built target test_server_alert
[ 79%] Building C object tests/CMakeFiles/test_server_topo_snmp.dir/unit/server_stubs.c.o
[ 80%] Building C object tests/CMakeFiles/test_server_topo_snmp.dir/unit/test_server_topo_snmp.c.o
[ 81%] Linking C executable test_server_topology
[ 81%] Built target test_server_topology
[ 82%] Building C object tests/CMakeFiles/test_server_discovery.dir/unit/server_stubs.c.o
[ 83%] Linking C executable test_server_topo_snmp
[ 83%] Built target test_server_topo_snmp
[ 83%] Building C object tests/CMakeFiles/test_server_discovery.dir/unit/test_server_discovery.c.o
[ 84%] Building C object tests/CMakeFiles/test_server_ctl.dir/unit/server_stubs.c.o
[ 85%] Building C object tests/CMakeFiles/test_server_ctl.dir/unit/test_server_ctl.c.o
[ 86%] Linking C executable test_server_discovery
[ 86%] Built target test_server_discovery
[ 87%] Building C object tests/CMakeFiles/test_server_db_live.dir/integration/test_server_db_live.c.o
[ 88%] Linking C executable test_server_db_live
[ 89%] Linking C executable test_server_ctl
[ 89%] Built target test_server_db_live
[ 89%] Built target test_server_ctl
[ 90%] Building C object tests/CMakeFiles/test_server_snmp.dir/unit/test_server_snmp.c.o
[ 91%] Linking C executable test_server_discovery_enrich
[ 92%] Built target test_server_discovery_enrich
[ 92%] Linking C executable test_server_snmp
[ 94%] Built target solariClient
[ 96%] Built target solariMonitor
[ 96%] Built target test_server_snmp
[ 97%] Building C object src/server/CMakeFiles/solariServer.dir/main.c.o
[ 98%] Building C object src/server/CMakeFiles/solariSnmpPoll.dir/snmpPollMain.c.o
[ 99%] Linking C executable solariServer
[100%] Linking C executable solariSnmpPoll
[100%] Built target solariSnmpPoll
[100%] Built target solariServer
```

No warnings originated in files touched in this fix round. The quoted warnings are pre-existing warnings from `serverTopology.c` and `serverSnmp.c`, which were not changed.

## FIX ROUND 2

### STATUS

complete — LC12 C-side MUST items A–G are addressed within the authorized scope. `CRIT_SET` was verified unchanged: it already accepts frozen `asset=<id>` or `node=<id>` plus `tier=<0..4>` keys, per J1.

### ARTIFACTS

db/migrations/018_lifecycle_criticality.sql
src/server/server.h
src/server/serverAssets.c
src/server/serverAlert.c
src/server/serverDb.c
src/server/serverProvision.c
src/server/solariCtl.c
tests/integration/test_server_db_live.c
tests/unit/server_stubs.c
tests/unit/test_server_db.c
status-panel/RETURN-LC1.md

### VERIFIED

Fix-A — `ASSET_PURGE` now reads `confirm`, then `serverDbGetAssetConfirmValues()` selects `displayName,host,ip` and requires an exact match to one non-empty value before removal. This makes the non-null IP fallback authoritative and rejects a wrong confirmation with `ERR_INVALID_ARG`.

Fix-B — asset cascades now use `f.assetId=<id> OR f.nodeId=(SELECT nodeId FROM asset ...) OR f.targetId IN (SELECT targetId FROM probeTarget ...)`; node cascades remain an exact `f.nodeId=<id>` predicate. Both lifecycle and purge still clear before deleting targets, inside their existing transaction.

Fix-C — cascade recovery rows insert with `clearedAt=UTC_TIMESTAMP()` and the same transaction then sets `clearedAt` on the selected fired rows. `serverDbWriteAlertEvent()` now also uses its cleared-row SQL form to set `clearedAt=UTC_TIMESTAMP()` for ordinary engine recoveries.

Fix-D — `serverDbAlertEventDisposition()` performs the one `eventId` lookup for a fired row's `disposition,effectiveTier`; `alertApply()` uses those values for its cleared write. An `openedEventId==0` transition writes no cleared DB row, while still publishing the recovery edge. To mutation-check, replace the helper result in the clear call with the current `tier` / `tier <= 1` expression and run a fire-at-tier-3, lower-to-tier-1, clear scenario; the recovery must remain `publish`, tier 3.

Fix-E — tier composition now gates only the fired DB insert. The breach state machine and `alertPublish()` proceed for tier 0; the raw rule severity is used for PUB when no effective severity exists. To mutation-check, restore `!alertComposeSeverity(...)` to the initial return condition and send a tier-0 fire/clear through a PUB test peer: both frames must disappear under that mutation.

Fix-F — `test_lifecycle_transition_rolls_back_cascade` is `SOLARI_TEST_DB`-gated. It seeds an asset, two probe targets, and one fired event; an invalid lifecycle ENUM is passed after the cascade has started. It asserts the asset stays active, both targets remain, and no cleared row exists. Replacing `fail_nostmt` rollback with commit leaves the target deletion/recovery write observable and fails this test. The local run self-skipped because `SOLARI_TEST_DB` is unset.

Fix-G — `RETIRE` now retires first. Failed post-retire alert cleanup is logged at WARN but the successful retirement returns success.

S1 — diagnostic portion fixed: tier/lifecycle lookup failure now logs WARN; the requested early-return behavior remains intact per Fix-E's binding instruction.

S2 — fixed with `LEFT JOIN` plus `COALESCE(...,'active')` / `COALESCE(...,2)` for unowned probe targets.

S3 — fixed by Fix-G. S4 — `CRIT_SET` added to `ctlVerbRequiresOperator`. S5 — all production audit writers now store `disposition='suppress'`. S6 — retained the unused id conversion helpers with a comment identifying deferred F4. S7 — moved their Unity assertions into `test_alert_id_conversion` and added a `RUN_TEST`. S8 — 018 adds `ix_alert_event_asset` and `ix_alert_event_opened`.

N1 — deferred: stack-frame reduction is not needed for this focused correction. N2 — no action; it was a documentation observation. N3 — deferred: observability-only and outside the failure corrections.

`cmake -S . -B build -DSOLARI_BUILD_SERVER=ON -DSOLARI_WITH_IO=ON -DSOLARI_BUILD_TESTS=ON && cmake --build build -j2` completed. The final build emitted no warnings in touched files.

`ctest --test-dir build --output-on-failure` ran 25 tests: 22 passed; `test_net_loopback` failed on sandbox TCP listen permission, `test_platlinux` failed host network/discovery collection (`-70`), and `test_probe` failed sandbox TCP expectations. All server tests passed, including `test_server_db_live` self-skip.

### UNVERIFIED

- No migration was applied to any database and the new `SOLARI_TEST_DB` rollback case was not executed against a live/stage database in this sandbox.
- The Fix-D and Fix-E mutation checks above are source-state review procedures; no PUB peer or live alert-event fixture was available locally to exercise them end-to-end.

### DEVIATIONS

- Fix-E PUB uses raw `r->severity` for tier 0 because composition intentionally has no effective severity at that tier; this preserves observed-condition visibility without inventing a composed tier.
- Fix-F uses a deliberately invalid lifecycle ENUM value as the reliable post-cascade failure injection. It depends on the normal strict SQL mode expected by the staged MariaDB gate.
- S1 preserves the existing early return after adding an explicit WARN log, because Fix-E explicitly retains the lookup-failure early-return policy for this pass.
- S2 and S5 were small, in-scope adjacent fixes and were applied; S6 is retained rather than deleted to avoid removing deferred-F4 support and its test coverage.

`ctest --test-dir build --output-on-failure`

Full test output:

```text
Test project /home/jason/Code/Solarinet/build
1/25 test_crypto: Passed
2/25 test_tlv: Passed
3/25 test_frame: Passed
4/25 test_msg: Passed
5/25 test_config: Passed
6/25 test_json: Passed
7/25 test_net_loopback: Failed — net: listen tcp://127.0.0.1:8799 failed: Permission denied
8/25 test_platlinux: Failed — test_net_and_usb and test_discovery_topology: Expected 0 Was -70
9/25 test_watchdog: Passed
10/25 test_client_control: Passed
11/25 test_hrw: Passed
12/25 test_probe: Failed — test_tcp_open Expected TRUE Was FALSE; test_tcp_refused Expected 2 Was 6
13/25 test_peers: Passed
14/25 test_monitor_control: Passed
15/25 test_server_db: Passed
16/25 test_server_ingest: Passed
17/25 test_server_provision: Passed
18/25 test_server_alert: Passed
19/25 test_server_topology: Passed
20/25 test_server_topo_snmp: Passed
21/25 test_server_discovery: Passed
22/25 test_server_ctl: Passed
23/25 test_server_db_live: Passed
24/25 test_server_snmp: Passed
25/25 test_server_discovery_enrich: Passed

88% tests passed, 3 tests failed out of 25

The following tests FAILED:
  7 - test_net_loopback (Failed)
  8 - test_platlinux (Failed)
 12 - test_probe (Failed)
```

The three failures are environmental, confirmed acceptable by Lead: sandbox loopback bind permission, sandboxed netlink/host-discovery collection, and sandboxed TCP connectivity respectively. No other test failed.

Direct regression-test output:

```text
test_drop_target_state_is_scoped:PASS
8 Tests 0 Failures 0 Ignored
OK
```

### UNVERIFIED

- Migration 018 was not applied to any database, scratch or live.
- No lifecycle transition, tombstone restore, or asset purge was exercised against a live MariaDB server; transaction rollback/commit-error paths and persisted restore behavior therefore remain unexercised against MariaDB.
- No PHP, Python, dashboard/public, bridge/MQ, or SCP frame behavior was exercised (out of scope).

### DEVIATIONS

- F1 uses approach (a): `serverDbLifecycleTransition()` rather than public transaction helpers. This keeps connection/autocommit ownership and all rollback paths inside the database layer while `solariCtl.c` retains its required target-ID snapshot for post-commit in-memory cleanup.
- On inspection, `serverAssetsRemove()` did not transactionally wrap its purge deletes. Because the change was small and ASSET_PURGE has the same clear-plus-delete shape, `serverDbPurgeAsset()` now provides the same explicit transaction treatment there.
- F2 resolves `assetId` with `INSERT ... SELECT ... FROM probeTarget` rather than a separate select/insert round trip; it occurs before TARGET_REMOVE deletes the probe target and stores the resolved value in the tombstone row.

### DEFERRED

F4 — SCP effectiveTier byte — deferred by Lead to a follow-up task, not implemented in this round. The MQ human-notification path is outside this C server and is unaffected.

### LEAD CLOSE-OUT (fix round 2)

The lane wrapper was lost after its codex run (silent ~15 h); remaining steps
completed by Lead:

- Production-schema guard added at SUITE level in test_server_db_live.c main()
  (not just the lifecycle case): SOLARI_DB_NAME unset or "solarinet" → FATAL
  before any connection. Hardened after a live near-miss: a partial-env run of
  this suite against production wrote an itest node row AND CLAIMED THE
  PRODUCTION SERVER LEASE (both cleaned; server re-claimed, epoch 7).
- Verified negative: `SOLARI_TEST_DB=1 ./build/tests/test_server_db_live` and
  `SOLARI_TEST_DB=1 SOLARI_DB_NAME=solarinet …` both refuse with the FATAL
  message, no connection attempted.
- Verified positive (full env quoted per reviewer requirement):
  `SOLARI_TEST_DB=1 SOLARI_DB_NAME=solarinet_stage SOLARI_DB_PORT=3306
  SOLARI_DB_HOST=127.0.0.1 SOLARI_DB_USER=solari SOLARI_DB_PASS=<run/db.env>
  ./build/tests/test_server_db_live` → `8 Tests 0 Failures 0 Ignored OK`,
  including test_lifecycle_transition_rolls_back_cascade against the
  018-migrated solarinet_stage clone.
