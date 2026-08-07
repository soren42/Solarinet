# RETURN — SOLNET-AW1 · UniFi pipeline + daemon

`worker: sonnet-5 (host wrapper, Worker-authoring authorized) · task: SOLNET-AW1 · governed by CONTRACT-AW.md §2–§5, §9–§10`
`status: COMPLETE — no git operations, service restarts, or live-DB writes performed`

This supersedes the prior BLOCKED PARTIAL return. The first pass (codex, sandboxed, no network) correctly built and unit-tested the UniFi poller and daemon decode path, but could not reach the live production endpoint, the real UniFi gateway, or any MariaDB socket — so it correctly self-reverted its one `panel.php` attempt rather than ship an unverified edit against a live-served file. Team-lead reviewed that packet, confirmed the revert was the right call, and authorized me (the host-level wrapper, which has real network/DB access) to complete the remainder directly. Everything below was exercised for real: live production curl round-trips after every save, a live UniFi API dry-run poll, and a write-path proof against `solarinet_stage`.

Accepted pending cross-lab review after the initial completion pass. One fix round followed (see **FIX ROUND — A-4** below) for a cross-lane bug AW3 found in the gear-state mapping.

## Files authored / changed

| Path | Change |
|---|---|
| `deploy/unifi/unifipolld.py` | 15s fail-soft Integration API poller. Fixed post-review against the real API: (1) stopped auto-guessing SolariNet's own internal CA for the UniFi gateway's unrelated self-signed cert — was causing 100% SSL failures; (2) added the separate `.../statistics/latest` per-device fetch — the `/devices` list carries no rx/tx data, only that dedicated endpoint does; (3) fixed `operStatus` — the real API reports `state:"ONLINE"/"OFFLINE"` (a string), not `online`/`isOnline`/`connected` booleans, so every device was silently reporting "up" regardless of truth. |
| `deploy/unifi/test_unifipolld.py` | Unit coverage for role mapping, rate derivation, fail-soft API failure (unchanged from codex's version; still passes after the fixes above). |
| `deploy/unifi/unifipolld.service` | alertbridge-style unit. Fixed `ExecStart` to use the new dedicated `.venv` instead of bare `/usr/bin/python3` — the system interpreter lacks `pymysql`, so the daemon as originally shipped would have silently never persisted a single row (fail-soft catch-and-skip, no crash, no loud error). |
| `deploy/unifi/requirements.txt` | New. `pymysql==1.2.0`. |
| `deploy/unifi/.venv/` | New. Dedicated venv, pymysql installed, mirrors `deploy/alertbridge/.venv`. |
| `db/migrations/019_panel_screen_config.sql` | `panelScreenConfig` singleton + additive `networkGear.kind` enum widening. Applied to `solarinet_stage` only; verified via `DESCRIBE`. **Not applied to live `solarinet`** — that remains the Lead's action. |
| `status-panel/daemon/solariPanel.c` | (codex-authored, not touched by me) 0x85 CONFIG decode (`handleConfig`), POST to `/api/panel/config` (`postConfig`), dispatch in `panelFrame()`. Wire shape verified below to match `panelConfigInput()` on the server side exactly. |
| `dashboard/api/routes/panel.php` | **Authored directly by me this pass.** Gear section, kind 8/9 command validation, config validation, feature-detected `panelScreenConfig` GET, new `POST /api/panel/config` route. Detail and verification below. |

## panel.php changes, in detail

All edits made to the live-served file; each was `php -l`'d and curl-verified against production immediately after saving, per the mandatory discipline (a prior lane took production down for 5 hours by skipping this).

1. **Helpers** — `panelGearRole(string $kind): ?int` (maps `networkGear.kind` to the CONTRACT-AW §3.1 role byte: 0 router/gateway, 1 switch, 2 hub, 3 ap, 4 wanBackup; `null` for `'other'`, which drops those rows rather than mis-rendering them) and `panelGearLogScale(int $kbps): int` (0..7 log-scale bucketing, doubling roughly every level, saturating at 20 Mbps).
2. **Kind 8/9 command validation** — extended `panelCommandInput()`'s range from kinds 1..7 to 1..9, per CONTRACT-AW §3.2/§10 D2: kind 8 (`PANEL_CTL_SCREENEN`, arg = `(screenIdx<<1)|enabled`) validated `arg <= 23`; kind 9 (`PANEL_CTL_SCREENWT`, arg = `(screenIdx<<3)|weightCode`) validated as the two decomposed fields, `screenIdx <= 11` and `weightCode <= 5`.
3. **`panelConfigInput(array $body): array`** — validates a decoded CONFIG report body: `screenCfg` must be exactly 12 entries, each `{enabled: 0|1, weightCode: 0..5}`; `flags` an int 0..255. Returns `[screenCfgJson, flags]` or a 400.
4. **Gear section (GET)** — added inside the existing read-only transaction:
   ```sql
   SELECT g.gearId, g.kind, i.inRateKbps, i.outRateKbps, i.operStatus
     FROM networkGear g
     JOIN gearInterfaceCurrent i ON i.gearId = g.gearId AND i.ifIndex = 0
    WHERE i.sampledAt > DATE_SUB(NOW(6), INTERVAL 60 SECOND)
    ORDER BY g.gearId
   ```
   `networkGear`/`gearInterfaceCurrent` already exist live (migrations 002/006) — **no feature-detection needed for these two tables**, only for the new `panelScreenConfig` table (below). Rows are mapped through `panelGearRole()` (dropping `'other'`-kind rows — this also naturally excludes the two stale SNMP test-gear rows already sitting in live `networkGear`), sorted role-then-gearId, capped to 12 with an `error_log()` note on truncation (E3), and returned as `gear` (array of `{role,state,rxLevel,txLevel}`) + `gearCount`.
5. **`panelScreenConfig` feature-detection + GET** — `information_schema.TABLES` check (mirrors the existing `$lcCols` pattern for migration 018), `null` when absent, else the singleton row shaped as `{screenCfg, flags, reportedAt}`.
6. **`POST /api/panel/config`** — mirrors `POST /api/panel/state`'s shape/auth exactly: service-principal-only (`panelIsServicePrincipal` / `panelLogRejectedWrite` / 403 otherwise), `panelConfigInput()` validation, `INSERT ... ON DUPLICATE KEY UPDATE` upsert into `panelScreenConfig`. Feature-detected: when the table doesn't exist yet (live, today), it returns a **soft no-op 200** `{"panelScreenConfig": null}` rather than a 500 — matching the GET side's discipline exactly, so a firmware/daemon that starts POSTing CONFIG reports before migration 019 lands cannot 500-loop.

### Wire-shape cross-check against the daemon

Read `status-panel/daemon/solariPanel.c`'s `postConfig()`/`handleConfig()` directly (not touched by me, but verified): it POSTs `{"screenCfg":[{"enabled": byte&1, "weightCode": (byte>>1)&7} x12], "flags": N}` to `{apiBase}/api/panel/config`. This is byte-for-byte what `panelConfigInput()` expects and validates. No cross-lane mismatch.

## Commands exercised (all live/real, not sandboxed)

```
$ php -l dashboard/api/routes/panel.php
No syntax errors detected in dashboard/api/routes/panel.php
```
Run after each of the four edits (helpers, kind 8/9, gear+GET, POST route) — clean every time.

**Live production round-trips**, after every save (panel service principal, `run/panel.pass`):
```
LOGIN=200   PANEL=200      (after helpers + kind 8/9 + panelConfigInput)
LOGIN=200   PANEL=200      (after gear section + panelScreenConfig GET wiring)
LOGIN=200   PANEL=200      (after POST /api/panel/config route)
```
Payload check confirmed the new fields appear and behave correctly pre-migration:
```
gear: []  gearCount: 0            (no fresh gearInterfaceCurrent row inside 60s at check time — correct, not an error)
panelScreenConfig: null            (table absent live — correct, feature-detected)
```
POST exercised directly against production:
```
$ curl -X POST https://dashboard.akoria.net/api/panel/config  (valid 12-entry body, service principal)
200 {"ok":true,"data":{"panelScreenConfig":null}}              <- soft no-op, table absent, exactly as designed
$ curl -X POST https://dashboard.akoria.net/api/panel/config  (screenCfg:[], flags:1 — malformed)
400 {"ok":false,"error":{"code":"bad_request","message":"screenCfg (12 entries) and flags are required."}}
$ curl https://dashboard.akoria.net/api/panel   (immediately after both POSTs)
200                                                             <- production never degraded
```

**Real UniFi API dry-run** (`run/unifi.env`, gateway self-signed cert, skip-verify):
```
DRY-RUN networkGear unifi-9c:05:d6:e4:f1:ac kind=switch  name=laboratory
DRY-RUN networkGear unifi-a8:9c:6c:4c:9a:d0 kind=ap      name=beaker
DRY-RUN networkGear unifi-a8:9c:6c:6a:09:c0 kind=ap      name=flask
DRY-RUN networkGear unifi-58:d6:1f:20:5a:e9 kind=hub     name=Test Tube
DRY-RUN networkGear unifi-6c:63:f8:7f:90:3c kind=hub     name=pipette
DRY-RUN networkGear unifi-58:d6:1f:1e:ab:7d kind=hub     name=slide
DRY-RUN networkGear unifi-74:f9:2c:b2:6f:c7 kind=wanBackup name=covalent
DRY-RUN networkGear unifi-90:41:b2:c0:62:75 kind=ap      name=cyclotron
DRY-RUN networkGear unifi-74:fa:29:41:f0:4f kind=gateway name=chemistry  in=48 out=22
unifipolld: polled 9 UniFi devices (dry run)
```
All 9 real devices, correct role mapping (gateway/switch/hub/ap/wanBackup — no `'other'` fallthrough on the real fleet). Rates are 0 on the first poll of a fresh state file except the gateway (real uplink throughput observed); rate derivation requires two polls 15s apart to show non-gateway deltas, which is expected and matches the SNMP poller's own convention.

**Stage-DB write-path proof** (`run/db.env`, `127.0.0.1:3306`, `solarinet_stage` — never live):
```
$ .venv/bin/python3 unifipolld.py --once --database solarinet_stage --state /tmp/...
unifipolld: polled 9 UniFi devices

$ mariadb ... solarinet_stage -e "SELECT gearId,kind,inRateKbps,outRateKbps,operStatus FROM networkGear g JOIN gearInterfaceCurrent i ON ... ORDER BY gearId"
unifi-58:d6:1f:1e:ab:7d  hub        0   0  1
unifi-58:d6:1f:20:5a:e9  hub        0   0  1
unifi-6c:63:f8:7f:90:3c  hub        0   0  1
unifi-74:f9:2c:b2:6f:c7  wanBackup  0   0  1
unifi-74:fa:29:41:f0:4f  gateway   88  49  1
unifi-90:41:b2:c0:62:75  ap         0   0  1
unifi-9c:05:d6:e4:f1:ac  switch     0   0  1
unifi-a8:9c:6c:4c:9a:d0  ap         0   0  1
unifi-a8:9c:6c:6a:09:c0  ap         0   0  1
```
Exactly 9 rows, correct kinds, `operStatus=1` (up) for all — matches `device_online()`'s real-API behavior. Also directly exercised the exact `INSERT ... ON DUPLICATE KEY UPDATE` SQL that `POST /api/panel/config` issues, against `solarinet_stage.panelScreenConfig` — one row landed correctly with the JSON payload intact.

```
$ python3 -m unittest deploy/unifi/test_unifipolld.py
Ran 3 tests in 0.001s
OK
```

No secrets were printed in any command output above or in the daemon/poller logs (both intentionally log only counts/kinds/HTTP status, never the API key or DB password).

## Correction to a prior claim

The Lead's remainder-authorization message stated the daemon's 0x85 CONFIG decode is "dead code until AW2's `knownType()` admits 0x85." **This is no longer accurate and should not go forward as a finding.** I checked directly: `status-panel/protocol.c:184`
```c
static int knownType(uint8_t type) { return type >= PANEL_FT_SNAPSHOT && type <= PANEL_FT_CONTROL ? 1 : (type >= PANEL_FT_HELLO && type <= PANEL_FT_CONFIG ? 1 : 0); }
```
already admits the full `PANEL_FT_HELLO..PANEL_FT_CONFIG` range, and `protocol.h:82` defines `PANEL_FT_CONFIG = 0x85`. AW2's lane has already landed this in the shared working tree. The daemon's CONFIG decode path (`handleConfig`/`postConfig`, dispatched in `panelFrame()` on literal `0x85`) is live and reachable today, not dead code. Framed as an integration status note, not a defect: nothing on my side needs to change for this; it just means the decode path is exercisable now rather than pending AW2.

## Scope check

```
$ git status --porcelain
```
Modified within AW1 scope: `dashboard/api/routes/panel.php`, `status-panel/daemon/solariPanel.c`.
New within AW1 scope: `db/migrations/019_panel_screen_config.sql`, `deploy/unifi/` (poller, test, service, requirements.txt, .venv), this file.

Also present in the working tree but **not mine** (other lanes, already landed before/alongside this pass — not touched, not attributed to AW1): `dashboard/public/screens-panel.jsx`, `dashboard/public/styles.css`, `status-panel/daemon/tests/codec_test.c`, `status-panel/firmware/*` (CMakeLists.txt, main.c, panelCtl.c/.h, panelScreensA.c, panelScreenCfg.c/.h, test/*), `status-panel/fixtures/panel-snapshot.json`, `status-panel/protocol.c`, `status-panel/protocol.h`, `status-panel/RETURN-AW3.md`, `tests/dashboard/test_panel_aw.js`.

No `git add`/`commit` was run. No service was restarted. Migration 019 was applied to `solarinet_stage` only — live `solarinet` is untouched and still lacks `panelScreenConfig` (confirmed by the live `panelScreenConfig: null` response above).

## UNVERIFIED

1. **Cross-lane payload consumption** — I have not coordinated directly with AW3 (dashboard) to confirm the `gear`/`gearCount`/`panelScreenConfig` JSON shapes I chose are exactly what the SPA/panel screens expect to render. The shapes follow CONTRACT-AW §3.1/§3.3 as I read them, and the daemon-side wire cross-check above lines up, but I have not seen AW3's or AW2's actual consuming code.
2. **End-to-end CONFIG frame** — no physical panel or firmware build was exercised sending a real 0x85 frame through the daemon to `POST /api/panel/config` live; the daemon-to-panel.php contract is verified by direct code/curl inspection on each side, not a live serial-to-HTTP round trip.
3. **Live migration 019 application and post-migration behavior** — untested against production by design (out of scope; Lead's action). The feature-detected code paths on both GET and POST have only been exercised in their "table absent" branch live; their "table present" branch is proven correct only against `solarinet_stage`, not live.
4. **Gear section under real live traffic** — the `gear`/`gearCount` fields were verified structurally correct (200, `[]`/`0` when no fresh row) but not yet observed non-empty against production, since the UniFi poller unit is not deployed/running there (deployment was out of scope for this task).
5. **12-device truncation path** — the real UniFi fleet has 9 devices, so the `>12` truncation branch and its `error_log()` note were reviewed by inspection, not exercised against real over-cap data.

(UNVERIFIED item 1 above is now partially resolved: AW3 did read and exercise the `gear` payload shape — that is how the A-4 bug below was found — so the JSON shape itself is confirmed consumed correctly. What remains unverified is only the *value semantics* of fields AW3 hasn't yet driven through their full render path.)

## FIX ROUND — A-4 (IF-MIB → wire gear-state mapping)

Cross-lane bug found by AW3, ruling committed as CONTRACT-AW §9 A-4 (NORMATIVE): `panel.php`'s gear assembly was casting `gearInterfaceCurrent.operStatus` (raw IF-MIB: 1=up, 2=down, 3=testing, 4=unknown, 5=dormant, 6=notPresent, 7=lowerLayerDown, nullable) directly into the wire's `state` field (0=down, 1=up, 2=degraded) with no mapping. Effect: every down device rendered DEGRADED (amber-solid), wire state 0 was unreachable, and a down router defeated the A-3 internet-column ruling exactly as written.

**Fix** — added `panelGearState(?int $operStatus): int` next to the other gear helpers, called from the gear-assembly loop instead of the raw `(int) $row['operStatus']` cast:
```php
function panelGearState(?int $operStatus): int
{
    return match ($operStatus) {
        1 => 1,
        3, 5 => 2,
        default => 0,
    };
}
```
`default => 0` covers 2, 4, 6, 7, and `null` in one arm — this is deliberate FAIL DARK per A-4: every unmapped/unknown/null status resolves to down (0), never to degraded or up, so an unrecognised status can never render healthier than a confirmed-down device.

**Live-file discipline**: `php -l` clean, then production LOGIN/GET curl 200/200 immediately after the save — no downtime.

**Verification required beyond lint** — seeded `solarinet_stage.gearInterfaceCurrent` with four rows (`operStatus` 1, 2, 3, NULL), then drove them through the *exact* assembly-line expression used in `panel.php` (`$row['operStatus'] === null ? null : (int) $row['operStatus']` → `panelGearState()`) by requiring the live route file directly (defining functions only; the route file returns a closure and registers nothing until invoked with a router, so this has no side effects) against the seeded stage rows over a real PDO connection:
```
$ php -r 'require "dashboard/api/routes/panel.php"; /* query solarinet_stage, map each row */'
a4test-1 operStatus=1    -> wire=1
a4test-2 operStatus=2    -> wire=0
a4test-3 operStatus=3    -> wire=2
a4test-4 operStatus=NULL -> wire=0
```
Matches the required sequence (1, 0, 2, 0) exactly. Also independently swept the full IF-MIB domain against the bare function (not just the four seeded values), confirming every branch of the A-4 table:
```
operStatus=1    -> wire=1 (expect 1) OK
operStatus=2    -> wire=0 (expect 0) OK
operStatus=3    -> wire=2 (expect 2) OK
operStatus=5    -> wire=2 (expect 2) OK
operStatus=4    -> wire=0 (expect 0) OK
operStatus=6    -> wire=0 (expect 0) OK
operStatus=7    -> wire=0 (expect 0) OK
operStatus=NULL -> wire=0 (expect 0) OK
```
The four `a4test-*` seed rows were deleted from `solarinet_stage` after verification (`networkGear`/`gearInterfaceCurrent`, `gearId LIKE 'a4test-%'`) — stage left clean, live untouched throughout.

## Next action for the Lead

Review this packet and `dashboard/api/routes/panel.php`'s diff; apply migration 019 to live `solarinet` when ready; deploy `unifipolld.service` (dedicated `.venv` now wired in, no further dependency work needed) and enable it; then the gear section and `panelScreenConfig` GET/POST will go live in their "table present" branches for the first time outside of `solarinet_stage`.
