# SolariNet Handoff — 2026-08-08

`Author: Claude (Fable 5, Lead) · Session: panel program + lifecycle/criticality + docs`
`State at close: ALL WORK HALTED · fleet in maintenance window · monitoring stack gracefully stopped`

## 1. Where everything stands

### Shipped and LIVE this program (all on branch `feat/flow-gates-weights` at close, see §4)

| Feature | State |
|---|---|
| Status panel (task #7 era) | Galactic Unicorn on lithium; software control page; STATE/CONTROL/CONFIG wire protocol |
| Lifecycle + criticality (#9/#10) | Tombstone delete/rework + 5-tier criticality → engine severity, MQ gating, panel alarm. Migrations 016–019 applied live |
| A1 flow gates (#11) | UniFi-fed live topology screen; unifipolld.service on xenon polls the Integration API q15s |
| Per-screen enable/weights (#12) | kinds 8/9, CONFIG 0x85, RP2350 flash persistence (needs `PICO_FLASH_ASSUME_CORE1_SAFE` — see §6 gotchas) |
| Docs suite (#13) | 23-page manual (`/docs/panel/SolariNet_Panel_Manual.html`), Quickref section, page blurbs, **on-panel Vol+ help** |
| Firmware at close | **d86f11e3** (help + B1/C2 layout fixes), archived `lithium:~/fw-archive/`. Config persistence proven across reboot AND reflash |

### Task register (persistent task list is authoritative; summary)
- **#7** in_progress: only the audible alarm-ACK acceptance leg + `optest` principal removal remain (see §5 Deferred)
- **#9–#13** completed
- **#14** pending: prompt-stack post-mortem — the failure inventory lives in the task description; RICH: live-repo hazard ×3, live-binary hazard (build-io), silent lane stalls ×3, crossed-messages ×4, model-identity drift, unreviewed destructive invocations (SOLARI_TEST_DB near-miss + lease theft), SDK-silent-refusal (pico_flash)
- **#15** ~completed this close-out: mains reconciled (see §4)
- **#8** pending: standalone WiFi mode — arch constraints in task description (config on RP2350 ✅ already built in #12; machine-agnostic config API partially exists via /api/panel/config)

## 2. How to resume efficiently

1. Read the memory files (`status-panel.md`, `lifecycle-criticality.md`) — they carry the wire formats, verb keys, deploy runbooks, and every gotcha.
2. Contracts are the law: `status-panel/CONTRACT{,-CP,-AW,-LC}.md` — amendments/dispositions (§7/§8/§9/§10 blocks) override earlier sections. RETURN-*/REVIEW-* packets record who built/verified what.
3. Restart sequence (reverse of §7 shutdown): MariaDB+Apache never stopped → `systemctl start solarinet-server solarinet-client solarinet-monitor unifipolld alertbridge notify solarinet-snmp.timer` on xenon; `systemctl start solari-panel` on lithium; wake panel (CONTROL kind 7 arg 0 via POST /api/panel/command as an operator); `arduino-app-cli app start user:solari-glance` as arduino on lithium; close the maintenance window (§7).
4. Close/cancel the maintenance window: `UPDATE maintenanceWindow SET status='completed', endsAt=NOW() WHERE reason LIKE 'Session close-out%' AND status='active';`

## 3. Infrastructure state at close

- **xenon** (this host): dashboard Apache+php-fpm **left running** (serves the manual + dashboard read-only views of last data); MariaDB **left running**; all Solari* monitoring services **stopped** (§7 list)
- **lithium**: panel put to SLEEP then daemon stopped; glance app stopped; board keeps firmware d86f11e3 + persisted config
- **radium**: up, Keycloak/SSO healthy (post-storm recovery)
- **benzene**: powered but NO ROUTE from xenon (10.5.2.50 unreachable since the 2026-08-06 storm) — needs console/switch-port attention; RabbitMQ therefore unverified. alertbridge checkpoint deliberately advanced 83→178 on 2026-08-07 to skip 77 stale would-page rows
- **steel, helium**: down since the storm (physical attention)
- **DNS note**: lithium's wired NIC = 10.6.4.33 (Jason-assigned) + a stray DHCP 10.6.102.248 that may linger; `/etc/sysctl.d/90-multihome-arp.conf` on lithium fixes the ARP-flux false-conflicts
- **Parallel workstream**: RackWire (connection/capacity planner) — Jason's other session; owns the docroot sync (www-data). Coordinate docroot deploys via sudo. Its zip is committed at repo root

## 4. Git state (this close-out's reconciliation)

- `feat/flow-gates-weights` merged → `main`.
- **Diverged mains reconciled** (task #15): github main (panel/lifecycle lineage, production) merged with Forgejo-only July work (webpush sender, nmap discovery enricher, SMTP notify, Identity panel, static hosts file). Conflicted shared files resolved keeping the production (github) versions; **follow-up: port origin's webpush FRONTEND wiring** (sw.js push/notificationclick handlers + api.jsx subscribe flow) onto the v5 service worker — backend `deploy/notify/senders/webpush.py` arrived intact. Search `git log --grep=reconcile` for the merge commit.
- Both remotes pushed: `origin` (Forgejo cesium) and `github`, all branches + main.
- **Secrets red line**: `solari-auth.json.bak-*` and poller/bridge state files are gitignored — they contain the OIDC client secret / runtime state. Never commit.

## 5. Deferred items (each small, none blocking)

- Audible legs: panel alarm-ACK acceptance under a synthetic crit (sounds the wall tone), tier-4 "vital down" live page test — run when the household wants noise
- Remove temp `optest` operator principal (solari-auth.json + run/optest.pass) after those legs
- Admin-positive purge test (needs Jason's admin login)
- Jason read-throughs: panel manual (page overflow is the likeliest defect), Vol+ press test, A1 geometry veto window
- DOC1's backlog credit: framebuffer-assertion parity fixture (3 rendering defects were invisible to 150 green tests)
- AW3 review SHOULDs: send-path body assertion + give-up fake-timer test
- REVIEW-AW R2-S2: id-helper tcp coverage (blocked on deferred F4 = SCP effectiveTier byte)
- unifipolld stats-pacing vs 60 s freshness window (review note; only matters at >9 devices)
- PANEL_LUX_FULL=1600 field calibration (still open from the first panel week)
- Chemistry criticality left at tier 3 from testing — consider 4 (gateway)

## 6. Gotchas that WILL bite again (also in memory)

- `/api/*` is served LIVE from this repo (php-fpm SCRIPT_FILENAME → repo path). Every edit is production the moment it saves. Feature-detect columns until migrations apply. THREE incidents prove it.
- Production binaries exec from `build-io/` INSIDE the repo — lane builds clobbered it once and crash-looped the whole stack invisibly. Rebuild via clean worktree from committed HEAD; stop→cp→start (Text file busy otherwise); §14 should move them to /usr/local/bin
- `test_server_db_live` REFUSES `SOLARI_DB_NAME=solarinet` at suite level — it claims the server lease. Stage clone: `solarinet_stage` (018/019 applied). Full env: `SOLARI_TEST_DB=1 SOLARI_DB_NAME=solarinet_stage SOLARI_DB_PORT=3306 SOLARI_DB_HOST=127.0.0.1` + creds from run/db.env
- pico_flash: with pico_multicore linked, flash_safe_execute silently returns NOT_PERMITTED without `PICO_FLASH_ASSUME_CORE1_SAFE=1` (set in firmware CMakeLists — delete it only if core 1 ever launches, then init from core 1)
- Wire protocol version byte is STRICT EQUALITY — never bump; extend payloads additively (trailing-bytes tolerance)
- Panel screens: max two 5-row text elements per 11 rows; labels start ≤ row 6
- SVG regen for the manual: `node <scratchpad>/gen-screens.js` (scratchpad is session-scoped — the script is small; recreate from the docs commit message pattern or memory if lost)

## 7. This close-out's shutdown record

Maintenance window: `maintenanceWindow` scope='all', reason 'Session close-out 2026-08-08: cleanup/redeploy window', startsAt now, endsAt +48 h, status active.

Stopped on xenon: `solarinet-server solarinet-client solarinet-monitor solarinet-snmp.timer unifipolld alertbridge notify sor-apply-dns` (+ any `sor-*` siblings present). Stopped on lithium: panel slept (CONTROL kind 7), `solari-panel` stopped, `solari-glance` app stopped, `solari-glance-sync.path` stopped. NOT stopped: Apache/php-fpm/MariaDB on xenon (shared infra: dashboard read view, manual, RackWire, other vhosts), anything on cesium (Forgejo), radium (Keycloak), chemistry (gateway), fleet client agents on remote hosts (they fail-soft spool by design; benzene/steel/helium unreachable anyway).
