# Status Panel — Control Panel Page: Build Contract

`v1.1 · 2026-08-04 · Lead: Fable 5 · extends CONTRACT.md (which remains binding
for the deployed panel); DESIGN-BRIEF.md remains the visual spec of record`

> v1.1 incorporates the GPT-5.6 consult (14 findings). §10 dispositions are
> binding and WIN over §4/§5/§8 where they conflict. Headline change: the
> command queue has NO ack endpoint — confirmation of application comes from
> the panel's own STATE report (lastCmdId), and commands re-serve every poll
> until confirmed or expired.

Operator ask (verbatim intent): a software control panel, integrated into the
dashboard as a new page, featuring (a) live virtual versions of ALL theme
screens simultaneously, and (b) software controls: theme, brightness, alarm
acknowledge, and additional configuration.

## 1. Architecture

```
dashboard SPA                    xenon API                     lithium daemon        RP2350 fw
┌───────────────┐  poll /api/panel  ┌────────────────────┐  poll: data+commands ┌──────────┐ CONTROL ┌────────┐
│ PanelScreen   │◄──────────────────│ /api/panel (+cmds) │◄─────────────────────│solariPanel│────────►│firmware│
│ 12 virtual    │  POST command     │ /api/panel/command │  POST /api/panel/    │ forwards, │  STATE  │applies,│
│ LED screens + │──────────────────►│ (queue, table)     │  state + commandAck  │ acks queue│◄────────│reports │
│ controls +    │                   │ /api/panel/state   │                      └──────────┘         └────────┘
│ live state    │                   └────────────────────┘
└───────────────┘
```

- No new services, no new listeners: everything rides the existing 5 s daemon
  poll and the dashboard's existing API/auth. Command latency ceiling ≈ one
  poll (≤5 s + snapshot 2 s) — accepted; a faster path is out of scope.
- The virtual screens are rendered CLIENT-SIDE from /api/panel data by porting
  the design bundle's own renderers (`sA0..sD2` in status-panel/project/
  "Galactic Unicorn Themes.dc.html" + support.js) — the same source the
  firmware was ported from, so mockup, firmware, and page share one lineage.
  The page accumulates history rings from successive polls exactly as the
  firmware does (CONTRACT.md §9: history is never on the wire).

## 2. Components & ownership (cross-lab per review doctrine)

| # | Component | Author | Reviewer |
|---|-----------|--------|----------|
| CP1 | API: command queue + state + /api/panel extensions (PHP, migration) | GPT-5.6 codex | Claude Opus |
| CP2 | Daemon: command fetch/forward/ack + STATE frame RX + state POST (C) | GPT-5.6 codex | Claude Opus |
| CP3 | Firmware: CONTROL frame handling + STATE emission (C) | Claude Opus | GPT-5.6 codex |
| CP4 | Dashboard page: virtual screens + controls (JSX, no build step) | Claude Opus | GPT-5.6 codex |
| CP5 | protocol.h amendment (CONTROL/STATE frames) | Lead | both consume |

Branch `feat/panel-control` off main. No merges without the operator.

## 3. Protocol amendment (CP5 — normative once landed in protocol.h)

New frames, same framing/CRC/parser rules as v1 (version stays 0x01; both
sides ignore unknown types today, so this is backward/forward safe):

- `0x04 CONTROL` (host→panel): u32 cmdId, u8 kind, u8 arg, u8[2] reserved.
  Kinds: 1 setTheme(arg 0-3), 2 setScreen(arg 0-2, within current theme),
  3 setBrightness(arg 0-100, latches manual like LUX±), 4 autoBrightness
  (arg ignored; re-enables sensor), 5 ackAlarm, 6 setDwell(arg seconds,
  3/6/30 valid), 7 sleep(arg 0 wake / 1 sleep). Idempotent by design;
  at-least-once delivery is safe. Firmware ignores unknown kinds.
- `0x84 STATE` (panel→host): u8 theme, u8 screen, u8 brightnessPct,
  u8 autoBright, u8 sleeping, u8 alarmArmed, u8 alarmAcked, u8 reserved,
  u32 ackedEpisodeId, u32 lastCmdId (highest applied, 0 if none).
  Emitted on ANY state change and every 30 s as heartbeat.

## 4. API (CP1)

- Migration 016: table `panelCommand` (cmdId AUTO_INCREMENT, kind TINYINT,
  arg SMALLINT, createdAt, createdBy, collectedAt NULL, appliedCmdId-tracking
  not needed — see state). Table `panelState` (single row: the latest STATE
  fields + reportedAt).
- `POST /api/panel/command` {kind, arg} — roles admin|operator (NOT viewer;
  the panel principal stays read-only... EXCEPT commandAck/state below which
  the panel principal must write — gate those two by username=='panel' OR
  role operator+). Validates kind/arg ranges. Returns cmdId.
- `GET /api/panel` gains `"commands":[{id,kind,arg}...]` — uncollected
  commands, oldest first, marked collectedAt on serve TO THE PANEL PRINCIPAL
  ONLY (a dashboard user polling /api/panel must NOT consume the queue:
  gate collection on the session principal being `panel`).
- `POST /api/panel/commandAck` {ids:[...]} — daemon confirms serial write;
  acked commands are done. Uncollected-but-unacked commands older than 60 s
  are re-served (at-least-once).
- `POST /api/panel/state` — daemon uploads decoded STATE frames; stored in
  `panelState`, exposed in /api/panel as `"panelState":{...}` for the page.
- All within the existing session auth; same no-store/content-type rules.

## 5. Daemon (CP2)

- Parse `commands` from the poll body; for each: encode CONTROL, write-all,
  then POST commandAck (batch). Order preserved. On serial failure, do NOT
  ack — the queue re-serves.
- Parse 0x84 STATE frames; POST /api/panel/state (rate-limit: on change or
  ≥30 s). Log state transitions to journal.
- Config unchanged. Same hardening rules as CONTRACT.md §9.

## 6. Firmware (CP3)

- Handle CONTROL kinds per §3, applying through the SAME code paths as the
  physical buttons (setTheme == pressing that theme's button, ackAlarm ==
  any-button ack, brightness latches manual exactly like LUX±). lastCmdId
  dedupe: apply only cmdId > lastCmdId (u32, monotonic from AUTO_INCREMENT).
- Emit STATE per §3 (on change + 30 s heartbeat). Wire it into the existing
  event emission path.
- Host test suite extends: CONTROL apply/dedupe/unknown-kind, STATE emission
  cadence — same hardware-free pattern as panelLinkTest.

## 7. Dashboard page (CP4)

- New screen in the existing SPA idiom (Babel-in-browser, window.PanelScreen
  global, nav entry, dashboard design tokens — azure accent, status colors
  per the Interface Guide Rev 2; the LED simulations are exempt from the
  guide's palette since they reproduce the physical panel).
- Virtual panel grid: all 4 themes × 3 screens rendered simultaneously as
  53×11 LED simulations (canvas; round-dot pixels, dark bezel) at readable
  scale, each labeled (A0..D2). Live from /api/panel poll (5 s) + client-side
  history rings mirroring firmware accumulation. Alert inlay + beacon
  simulated when alarmActive.
- The PHYSICAL panel's current screen is highlighted (from panelState), with
  its live state chip: theme/screen, brightness %, auto/manual, awake/asleep,
  alarm armed/acked.
- Controls: theme buttons A-D, screen select, brightness slider + AUTO
  toggle, ACK ALARM button (disabled when no alarm), dwell select (3/6/30 s),
  sleep/wake toggle. Each posts /api/panel/command, optimistic-disables for
  one poll cycle, then reflects panelState truth. Viewer role sees the page
  read-only (controls hidden/disabled per session role from /api/auth).
- Port fidelity: renderers ported from the mockup's own sA0..sD2 functions;
  cite the mockup function per ported renderer in comments.

## 8. Acceptance (observable)

1. Page renders all 12 virtual screens with live data; a fleet state change
   appears in the virtual screens within one poll cycle.
2. `panelCommand` round trip: click theme B on the page → physical panel
   switches within ≤7 s → page highlight follows via panelState ≤ one poll
   later. Same for brightness, dwell, sleep/wake.
3. ACK: with a live (synthetic) alarm, page ACK silences the physical panel
   tone/beacon ≤7 s; panelState shows acked.
4. Dedupe: replaying the same cmdId (manual SQL) does not re-apply.
5. Roles: viewer session gets read-only page; POST /api/panel/command as
   viewer → 403. Dashboard-user polls do not consume the command queue.
6. Firmware host suite green; daemon `make test` green; two-build UF2 pair +
   `make -C firmware/test` flash gate as established.
7. Existing physical controls unchanged (spot-check buttons after flash).

## 9. Out of scope

WiFi transport (task #8), faster-than-poll command latency, audio volume
control (Vol± stay unassigned pending a volume feature), multi-panel support,
historical playback.

## 10. v1.1 dispositions (binding, supersede §4/§5/§8 where they conflict)

**Queue redesign (kills consult findings 1, 2, 6).** Single consumer → no
lease, no commandAck endpoint. Lifecycle: `pending` → `applied` | `expired`.
- GET /api/panel (panel principal only) returns ALL pending commands, oldest
  first, EVERY poll — re-serving until terminal. Dashboard-user polls never
  include the `commands` field at all.
- The server marks a command `applied` when a panel-principal POST
  /api/panel/state carries lastCmdId >= cmdId. That is the ONLY completion
  path: serial write success is a transport attempt, never completion.
- Expiry: pending > 120 s → `expired` (visible in the page as failed, with
  the reason; never silently dropped). Bound: max 16 pending; POST /command
  beyond that → 409. Malformed kind/arg rejected at POST (400), so the queue
  cannot be poisoned.
- Late-confirmation ruling (review S1, bounded per re-check R1): STATE is
  the ONLY completion authority, so a confirmation arriving shortly after
  expiry WINS — `expired` → `applied` is legal ONLY within 15 s of
  expiredAt (appliedAt set; expiredAt retained). Older expired commands are
  terminal forever: a firmware reboot resetting lastCmdId must never
  resurrect history. The expiry sweep runs only on service-principal GETs
  (S3); for pure observers, a pending command older than 120 s is
  PRESENTED as expired read-side without writing (R4).
- Record semantics (re-check R2, binding on the page): `status` is the only
  authority; appliedAt and expiredAt are NOT mutually exclusive (a late
  confirm carries both) and failure must never be inferred from expiredAt.
- Daemon forwards without a kind-range check (re-check R3): the firmware
  consumes unknown kinds (a consuming reject), so forwarding under version
  skew yields an honest outcome; a daemon-side range check would strand the
  command pending and let R1's sweep falsify it as applied. The daemon
  keeps only cmdId!=0/arg/encode guards.
- Known residual (review S8): the server cannot distinguish firmware-applied
  from firmware-rejected consumption; both report `applied`. Accepted —
  server-side validation mirrors firmware validation, making rejection
  practically unreachable; revisit if kind semantics ever diverge.
- STATE dwellSec: panelState carries dwellSec (byte 7); page dwell control
  confirms against it.
- Daemon: forwards every served command each poll (order preserved);
  firmware dedupe (cmdId <= lastCmdId ignored) makes re-serve free.
- Reboot replay window (finding 3): after firmware reboot lastCmdId=0 and
  UNCONFIRMED pending commands re-apply — harmless (idempotent kinds) and
  bounded by the 120 s expiry. Confirmed commands are terminal server-side
  and never re-served. RECORDED as accepted residual; reboot/replay is an
  explicit test case.
- Capability gate (finding 5): the daemon forwards commands ONLY after it has
  received ≥1 STATE frame since the current serial link-up (proof the
  firmware speaks CONTROL/STATE). Otherwise commands stay pending and expire
  naturally against an old firmware. Journal-log the withholding.

**Auth hardening (findings 6, 7, 8).**
- /api/panel/state accepts ONLY the local service principal: authenticated
  via the LOCAL user store AND username=='panel' AND role=='viewer' — the
  triple is the service-principal predicate; directory/OIDC identities never
  qualify. Rejected writes are logged with principal + IP.
- The service principal can never POST /api/panel/command (explicit deny
  before the role check). Operators/admins can never POST state.
- Page controls are presentation only; authority is server-side. Unknown/
  stale/failed role fetch → render read-only (fail closed). Tests include
  direct POSTs: command as viewer → 403, state as operator → 403.

**STATE semantics (finding 9, amended at build).** lastCmdId = highest cmdId
CONSUMED by the firmware (applied, or rejected as invalid/unknown — both
consume). Never advanced on mere receipt of a frame; commands consume in
strictly ascending cmdId order. STATE emitted on change of ANY wire field
(including lastCmdId/ackedEpisodeId — required: no-visible-effect commands
must still confirm, else they falsely expire) + 30 s heartbeat, with a 1 s
change-emission floor (withheld changes coalesce, never drop; heartbeat
exempt). The daemon sends HELLOREQ at each serial link-up; the firmware
answers HELLO + an immediate forced STATE, opening the capability gate
without waiting on the heartbeat. CONTROL commands are deliberately NOT
consumed as alarm acks and do NOT wake a sleeping panel — the page has
explicit ACK and sleep controls.

**Virtual-screen fidelity (findings 10, 11, 12).**
- Client history rings resample on server `ts` (time-based, gap-aware), not
  poll count; the page labels the simulation "~5 s cadence" — an
  approximation of the 2 s firmware rings, by design, stated in the UI once
  (footer note), not pretended away.
- Parity harness: a shared JSON fixture (status-panel/fixtures/panel-
  snapshot.json, committed) renders deterministically via ?fixture=1; every
  ported renderer cites its mockup source function AND its firmware
  counterpart file:line in comments. Reviewer checks the fixture render
  against both citations for all 12 screens incl. alarm + stale + empty-
  history states.
- Control buttons show pending (until applied/expired) and failed states;
  nothing silently re-enables.

**Acceptance (§8) is re-cut to observable form (findings 13, 14):**
1. Fixture render: ?fixture=1 shows all 12 screens deterministically; the
   reviewer signs off against citations.
2. Live delta: change a node state in DB → virtual screens reflect it ≤7 s
   (timestamps from browser console + DB).
3. Command round trip with evidence: page click → panelCommand row
   (createdAt) → daemon journal forward line → STATE lastCmdId → row
   `applied` (appliedAt) → page highlight. Each timestamp recorded; total
   ≤ 12 s. Repeat for theme/brightness/dwell/sleep.
4. ACK round trip under synthetic alarm (same fixture recipe as CONTRACT.md
   §9a): tone/beacon stop ≤7 s of click; panelState.ackedEpisodeId equals
   the episode; alertEvent NOT cleared (ack ≠ clear).
5. Races: firmware reboot mid-pending (re-apply then confirm), daemon
   restart mid-pending (re-serve), duplicate serve (no double-apply, via
   journal), expiry (unplug panel USB, post command, observe `expired` at
   120 s + page failed state), old-firmware withholding (simulated by
   suppressing STATE), simultaneous dashboard-user polling (queue untouched),
   and HELLOREQ gate-opening: `systemctl restart solari-panel` against a
   booted panel → STATE frame in the journal within one poll (not 30 s).
6. AuthZ: viewer POST command 403; operator POST state 403; service
   principal POST command 403; page read-only as viewer.
7. Gates: firmware host suite (extended: CONTROL apply/dedupe/consume-order/
   unknown-kind, STATE cadence) green; daemon make test green; two-build UF2
   pair + firmware/test green before flash; php -l clean; existing physical
   buttons re-checked after flash.
