# CONTRACT-AW — A1 Flow Gates + Per-Screen Enable/Weights

`v1.0 · 2026-08-07 · Lead: fable-5 · tasks #11 #12 · one firmware flash cycle`

Two features, one contract, one flash: both touch the wire protocol and the
firmware rotation engine, and Jason's standing rule is to bundle flash cycles.

## 1. Problem

**A1 (#11).** Screen A1 ("flow gates") must depict each piece of core
Ubiquiti equipment as a vertical gate, traffic flowing left→right:
- Router: FULL display height (11 rows).
- Switch: 75% (8 rows).
- Hubs: 50% (5–6 rows), STAGGERED vertically.
- APs: 25% (3 rows).
- Left of the gates: the array of APs + hubs feeding traffic rightward.
- Right of the gates: switch + hub; the INTERNET connection rendered to the
  RIGHT of the router line.

Live inventory (UniFi Integration API, creds `run/unifi.env`, verified
2026-08-07): chemistry (UDM Pro Max = router), laboratory (USW Pro Max 16 =
switch), Test Tube/pipette/slide (USW Ultra ×3 = hubs), beaker/flask
(U7 Pro) + cyclotron (U7 Pro XGS) = APs, covalent (U5G Backup = backup WAN,
drawn with the internet connection). 9 devices, all ONLINE.

**Weights (#12).** Every screen individually toggleable on/off plus a dwell
weight ∈ {¼x, ½x, 1x, 2x, 5x, 10x} multiplying the theme's base dwell
(base 30 s + weights 5x/½x/1x → 150 s / 15 s / 30 s). Config PERSISTS ON THE
RP2350 (flash) across resets and network changes (task #8 arch constraint),
is readable/editable via the API from any authorized machine, and surfaces
in the dashboard control page.

## 2. Data pipeline (new)

`deploy/unifi/unifipolld.py` (python daemon, systemd unit on xenon, pattern
per alertbridge/sorsync): polls the UniFi Integration API every 15 s.
- Upserts `networkGear` (gearId `unifi-<mac-or-id>`, name, kind mapped from
  model: udm*→gateway, "USW Pro"→switch, "USW Ultra"/USW Flex/Lite→hub,
  U6/U7/U5G AP models→ap, U5G Backup→wanBackup; mgmtIp, model).
- Upserts `gearInterfaceCurrent` per device with aggregate in/out Kbps
  (integration API statistics; per-device aggregate is sufficient for A1 —
  per-port is NOT required this cycle).
- Fail-soft: API errors log and skip a cycle; no crash loops. State file for
  rate derivation if the API returns counters instead of rates.
- Secrets: reads run/unifi.env; never logs the key.

## 3. Wire protocol (Lead-owned NORMATIVE deltas — apply exactly)

### 3.1 SNAPSHOT gear section (protocol version bump 1 → 2)
Appended to the SNAPSHOT payload (decoders accept longer payloads — S6
forward-compat holds for OLD firmware during rollout; NEW firmware requires
v2 for A1 data and renders A1's no-data treatment when absent):
```
u8  gearCount        (0..12)
per gear (4 bytes):
  u8 role            0=router 1=switch 2=hub 3=ap 4=wanBackup
  u8 state           0=down 1=up 2=degraded
  u8 rxLevel         0..7 (log-scaled by server; 0=idle)
  u8 txLevel         0..7
```
Order: server emits router first, then switch(es), hubs, aps, wanBackup —
firmware lays out by role, order within role stable by gearId.

### 3.2 CONTROL kinds (extend PanelControlKind)
```
PANEL_CTL_SCREENEN = 8   arg: (screenIdx << 1) | enabled     screenIdx 0..11
PANEL_CTL_SCREENWT = 9   arg: (screenIdx << 3) | weightCode  weightCode 0..5
```
weightCode: 0=¼x 1=½x 2=1x 3=2x 4=5x 5=10x. Server validates ranges
(screenIdx ≤ 11, weightCode ≤ 5) at POST /api/panel/command exactly like
kinds 1–7.

### 3.3 CONFIG report frame 0x85 (PANEL_FT_CONFIG, firmware → daemon)
16-byte payload: `u8[12] screenCfg` (per screen: bit0 enabled, bits1..3
weightCode), `u8 flags` (bit0 = cfgDirty persisted ok), `u8 reserved[3]`.
Emitted on change and on HELLOREQ (alongside STATE). Daemon decodes and
POSTs to /api/panel/config (service principal only); panel.php stores in a
`panelScreenConfig` singleton (migration 019) and serves it read-only in the
GET payload; the dashboard controls render from it (STATE-authority doctrine:
a toggle/weight shows confirmed only when the PANEL reported it).

### 3.4 Firmware persistence
Config struct (magic, version, screenCfg[12], CRC16 via panelCrc16) stored
in the LAST 4 KB flash sector via pico-SDK flash_range_erase/program, writes
debounced ≥ 2 s after last change, interrupts disabled during program (both
cores: use flash_safe_execute). Factory default: all enabled, weight 1x.
Corrupt/absent config → defaults, not a crash.

## 4. Firmware behavior

- **A1 renderer** (panelScreenA1 rewrite, main.c): geometry per §1 on the
  53×11 grid. Gates are vertical lines placed across the width; traffic =
  particles flowing left→right between gates, spawn rate/brightness scaled
  by rx/txLevel; gate color by state (up=theme accent, degraded=warn hue,
  down=crit hue + gate rendered broken). No gear data (gearCount 0 or v1
  server) → A1's existing no-data treatment. Hub gates STAGGERED: alternate
  top/bottom alignment. Internet: rightmost column treatment right of the
  router gate; wanBackup drawn dimmer beside it.
- **Rotation engine**: dwell per screen = base dwell × weight (¼,½,1,2,5,10).
  Disabled screens skipped in rotation AND by manual advance. Guard: if ALL
  screens of the active theme are disabled, treat as all-enabled (never a
  black wall); same guard per setScreen command targeting a disabled screen
  (accept, but rotation resumes per config).
- **panelCtl**: consume kinds 8/9 (validate; reject invalid as consuming per
  §10 CP contract), update config, mark dirty, emit CONFIG frame, persist.

## 5. Server + dashboard

- panel.php: gear section in the payload (from networkGear +
  gearInterfaceCurrent, freshest ≤ 60 s rows only; log-scale rates to 0..7);
  kinds 8/9 validation; POST /api/panel/config (service principal, mirrors
  /state); panelScreenConfig in GET (all principals read). Migration 019
  (panelScreenConfig singleton). LIVE-FILE RULE: every new column/table
  reference feature-detected until 019 applied; migration lands FIRST.
- Dashboard page (screens-panel.jsx): A1 virtual renderer parity with the
  firmware (same geometry/particles, fixture-driven); per-screen controls in
  the virtual grid tiles — enable toggle + weight selector (¼×–10×) with
  pending/confirmed states from panelScreenConfig (send() give-up pattern);
  fixture panel-snapshot.json extended with gear arrays + screenCfg.
- Daemon (solariPanel.c): decode CONFIG frames, POST /api/panel/config;
  forward kinds 8/9 unchanged (structural guards only, per R3 doctrine).

## 6. Edge cases

- E1: rollout order — daemon+server understand v2 before firmware flash;
  old firmware ignores trailing gear bytes (S6); new firmware on old server
  payload renders A1 no-data. No flag day.
- E2: 0 gear rows / stale rows → gearCount 0 → A1 no-data treatment.
- E3: >12 devices → server emits the 12 highest-role-priority (router,
  switch, hubs, aps in stable order) and logs once.
- E4: all-disabled guard (§4); weight change mid-dwell restarts the dwell
  accumulator only if it would already have expired under the new weight.
- E5: flash wear — debounce + only-on-change writes; a config storm from
  the API cannot exceed one write per 2 s.
- E6: CONFIG POST racing STATE POST — independent singletons, last write
  wins, both idempotent.
- E7: UniFi API down → poller skips; gear rows go stale → server stops
  emitting them at 60 s → panel degrades to no-data A1, no alarm.

## 7. Acceptance

- A1: fixture render of the new A1 matches the geometry spec (router 11,
  switch 8, hubs 5–6 staggered, APs 3, internet right of router) — virtual
  + firmware host-test parity on the same fixture bytes.
- A2: weights timing — base 30 s with weights 5x/½x/1x measured at
  150/15/30 s (±1 s) in the firmware host test harness (simulated clock).
- A3: disable a screen → rotation skips it; disable all → all-enabled
  fallback engages.
- A4: config persists across firmware reboot (host-test flash mock +
  hardware verification post-flash).
- A5: wire round-trips — v2 SNAPSHOT with 9-gear fixture, CONFIG frame,
  kinds 8/9 — codec unit tests + parser-path dispatch tests (the CP lesson:
  parser-path coverage mandatory, mutation-check knownType).
- A6: old-firmware compat — v1 decoder fed a v2 payload accepts and ignores
  trailing bytes (unit test).
- A7: live E2E after flash: toggle a screen + set a weight from the page →
  panel confirms via CONFIG report → page renders confirmed state; A1 shows
  live gear with traffic.
- A8: unifipolld resilience — API timeout injected → no crash, next cycle
  recovers; key never appears in logs (grep).
- A9: php -l/parse/harness gates; migration 019 stage-validated then live
  BEFORE referencing code (CP1 rule); no live-served file breakage at any
  point (feature-detect until migration applied).

## 8. Build lanes

- **AW1 — server+pipeline** (codex): unifipolld.py + unit, migration 019,
  panel.php gear section + kinds 8/9 + /config route + panelScreenConfig
  (ALL feature-detected until 019 applies — panel.php is served live; the
  LC2 incident is the cautionary tale), daemon CONFIG decode+POST.
  OUT: protocol.{h,c}, firmware, dashboard/public.
- **AW2 — protocol+firmware** (codex): apply §3 deltas to protocol.h/c
  EXACTLY as written (Lead-normative), codecs + tests (A5/A6), A1 renderer,
  rotation weights, panelCtl kinds 8/9, flash persistence (§3.4), host-test
  suite extensions (A2/A3/A4). Two-build reproducible UF2 gate; DO NOT
  flash. OUT: server, PHP, python, dashboard.
- **AW3 — dashboard** (opus): virtual A1 parity renderer, per-tile
  enable/weight controls, fixture extension, harness blocks. OUT: api
  routes, firmware, deploy. Coordinates fixture shape with AW1's payload
  via this contract only.
- Reviews: cross-lab (codex lanes ← opus reviewer; opus lane ← codex),
  mutation tests on rotation weights, codec bounds, poller resilience.
- Lane discipline (post-mortem rules now binding): NO edits to live-served
  files without feature-detection; report progress heartbeats — a lane
  silent > 60 min is considered stalled; packet (RETURN-AW<n>.md) is part
  of the run, not a follow-up; no git operations; no service restarts; no
  DB writes to live solarinet (stage clone only).

## 9. Amendments (Lead, during build)

**A-1 (layout, NORMATIVE — resolves the §1 left/right contradiction).**
§1's second bullet transcribed the original request's wording ("to the switch
and hub on the right") without resolving it against "internet right of the
router line". Operative reading, ratified: traffic order

    APs → hubs → switch → router → internet (+wanBackup)

x/y constants (53×11 grid, bands spread n gates evenly, centred when n==1):
```
ap     band x 2..16      h=3   y0 = round(i*(11-3)/(n-1)); 4 when n==1
hub    band x 20..34     h = i even ? 6 : 5; y0 = i even ? 0 : 11-h  (stagger)
switch band x 38..44     h=8   y0=1
router band x 46..48     h=11  y0=0
internet column x=51 · wanBackup column x=52
```
9-device inventory → AP gates at x 2/9/16, hubs 20/27/34, switch 41,
router 47. Legs: band gate i → next band gate (i mod nNext); final leg
router→internet. Wire brightness from src rxLevel; particle spawn/brightness
from src txLevel; level 0 = no particles. Gate colour up=accent /
degraded=warn / down=crit with every-other-pixel skip for "broken".
gearCount 0 → the screen calls the existing no-data treatment.
BOTH renderers (firmware A1, virtual A1) implement THESE constants — the
fixture is the parity oracle.

**A-2 (page pend keys).** Kinds 8/9 pend per-screen in the dashboard page
("screenEn:<idx>" / "screenWt:<idx>"), page-local; the one-key-per-kind
CMD_GROUP does not apply to per-screen commands.

## 10. Design-consult dispositions (BINDING — override §3/§4/§6 where they conflict)

**D1 (§3.1 — NO version bump).** protocol.c dispatches on strict version
equality; bumping would make old firmware drop every v2 frame (rollout
outage). The gear section ships as a V1-ADDITIVE trailing extension:
PANEL_PROTO_VERSION stays 1; new decoders parse gear ONLY when
`baseEnd + 1 + 4*gearCount <= len` and `gearCount <= 12`; absent/short/
oversized → gearCount 0 (no-data), never a decode error. Old firmware
ignores trailing bytes (S6). Rollout: server/daemon first, firmware after —
no flag day, no dropped frames.

**D2 (§3.2 — screenIdx mapping).** NORMATIVE: `screenIdx = theme*3 + slot`
(themes 0..3 × slots 0..2 → 0..11). Validation: kind 8 arg ≤ 23, kind 9
screenIdx ≤ 11 ∧ weightCode ≤ 5. SETSCREEN targeting a DISABLED slot:
accepted and displayed (operator intent wins), rotation resumes per config
on the next advance.

**D3 (§3.3).** knownType() MUST admit 0x85 — with a parser-path dispatch
test and a knownType mutation check (the CP lesson, mandatory). Flag bit0
renamed `cfgPersisted` (1 only after a successful flash write). All reserved
bits (screenCfg bits 4-7, weightCode 6-7, reserved bytes) transmitted as
zero; receivers ignore, senders never set.

**D4 (§3.4 — flash discipline).** The config sector is RESERVED IN THE
LINKER/BUILD LAYOUT (image end capped a sector short; assert at build time
that the binary does not reach it). Record is page-padded (256 B multiple)
at a 4 KB-aligned erase offset. Writes go through flash_safe_execute with
the full dual-core protocol: SRAM-resident callback, core-1 safe-state
(lockout) initialized at boot, deterministic behavior for USB/DMA during
the window. Power-loss mid-write → CRC fails → defaults (A4 gains this
case). If pico-sdk's flash_safe_execute setup proves heavier than the cycle
allows, the fallback is DEFERRING persistence to a follow-up and shipping
config as session-volatile — flag in the packet rather than shipping an
unsafe write path.

**D5 (§4/E4 — dwell rescale).** Weight change mid-dwell RESCALES elapsed:
`elapsed = elapsed / oldDwell * newDwell`, advancing immediately if elapsed
≥ newDwell. Never a reset (a 10×→¼× change at 299 s must not hold another
7.5 s). Manual advance always selects the NEXT ENABLED screen.

A2 (weights timing) gains the rescale case; A5 gains the 0x85 knownType
mutation check; A6 is restated as: v1 decoder (old firmware) fed a
gear-extended payload accepts and ignores it — same version byte.

**A-3 (internet column state — design ruling from AW3's flag).** The
internet marker column (x=51) is only meaningful THROUGH the router: when
the router gate's state is not `up`, the internet column renders in the
router's state treatment (degraded=warn hue; down=crit + broken/every-other
pixel), never a healthy full-height marker over a dead router. wanBackup
(x=52) keeps its own device state — during a router outage it visibly
becomes the only live path, which is exactly the at-a-glance story the
screen exists to tell. Firmware implements first; virtual conforms; harness
pins both states.

**A-4 (IF-MIB → wire state mapping, NORMATIVE — bug found by AW3 in
AW1's payload assembly).** gearInterfaceCurrent.operStatus is IF-MIB
(1=up, 2=down, 3=testing, 4=unknown, 5=dormant, 6=notPresent,
7=lowerLayerDown, nullable); §3.1 wire state is 0=down, 1=up, 2=degraded.
Passing operStatus through unmapped makes every down device read DEGRADED
(amber-solid) and state 0 unreachable — a down router would defeat A-3
exactly as written. Normative mapping (panelGearState()):
  1 → 1 (up) · 3,5 → 2 (degraded: present but not passing normal traffic)
  2,7 → 0 (down) · 4,6,NULL,anything else → 0 (down)
Principle: FAIL DARK — an unknown state must never render healthier than
down. Verification: stage-DB rows driven through the live endpoint proving
all three wire states reachable; cross-lab review mutation-checks the
mapper. Fixture stays wire-encoded (AW3's harness tests downstream of this
mapping by design — the mapping's own test lives server-side).
