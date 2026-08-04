# SolariNet Status Panel — Build Contract

`v1.1 · 2026-08-04 · Lead: Fable 5 · binding alongside DESIGN-BRIEF.md and protocol.h`

> v1.1 incorporates the GPT-5.6 architecture review (22 findings, dispositioned
> in §9). Where §9 conflicts with §3–§7, **§9 and protocol.h win**.

The Pimoroni Galactic Unicorn (53×11 RGB LED matrix, Pico 2 W / RP2350) attached
via USB to **lithium** (Arduino UNO Q, Debian 13 aarch64, the panel's Linux host)
becomes a live SolariNet fleet-status surface. Visual/behavioral spec:
`DESIGN-BRIEF.md` (binding). This contract fixes architecture, interfaces,
division of work, and acceptance criteria.

## 1. Architecture

```
xenon                          lithium                      Galactic Unicorn
┌─────────────────────┐        ┌──────────────────┐         ┌───────────────┐
│ dashboard PHP API   │ HTTPS  │ solariPanel      │ USB CDC │ C firmware    │
│  + NEW /api/panel   │◄───────│ daemon (std C)   │────────►│ (pico-sdk +   │
│ (SoR- & monitoring- │  poll  │ login→cookie,    │ binary  │  pimoroni drv)│
│  fed, auth: viewer) │  5 s   │ poll, frame,     │ frames  │ renders 12    │
└─────────────────────┘        │ write ttyACM0    │  2 s    │ screens,      │
                               └──────────────────┘         │ alarms, btns  │
                                                            └───────────────┘
```

**Why this shape** (from the data brief, verified live):
- cesium SoR `monitoring_state` is EMPTY — SoR is static inventory. Live health
  exists only in xenon's `solarinet` DB, which is 127.0.0.1-bound. The dashboard
  API is the only feed reachable from lithium, and it already merges SoR-adopted
  systems — so SoR grounding arrives via the API, not a raw DB socket.
- MQ/MQTT push are down (benzene offline). Polling now; MQTT is a future
  optimization, not scope.
- Auth has no token path. A dedicated `viewer`-role principal is added to
  solari-auth.json; the daemon logs in via POST /api/auth/login, holds the
  session cookie in memory, re-logins on 401.

## 2. Components & ownership

| # | Component | Language | Author | Reviewer (cross-lab) |
|---|-----------|----------|--------|----------------------|
| C1 | `dashboard/api/routes/panel.php` — GET /api/panel | functional PHP 8+ | GPT-5.6 (codex) | Claude Opus |
| C2 | `status-panel/daemon/` — solariPanel daemon + Makefile + systemd unit | standard C (camelCase, block-comment contracts) | GPT-5.6 (codex) | Claude Opus |
| C3 | `status-panel/firmware/` — Galactic Unicorn firmware | pico-sdk C / C++ only where the Pimoroni driver requires | Claude Opus | GPT-5.6 (codex) |
| C4 | Integration, flash, deploy, live verify | — | Lead | operator (Jason) sees final report |

Repo branch: `feat/status-panel`. No push/merge beyond a feature branch + PR.

## 3. Interface A — GET /api/panel (C1)

Auth: session (viewer role suffices). One cheap response, shaped for the panel;
poll target 5 s. All counts use the DASHBOARD-ACTIVE alert definition
(`ackedAt IS NULL AND (clearedAt IS NULL OR clearedAt > NOW()-60min)`) so the
panel never disagrees with the SPA. JSON:

```json
{ "ok": true, "data": {
  "ts": 1754280000,
  "score": 87,                      // 0-100, worst-pool rule per DESIGN-BRIEF
  "stateRoll": {"up":9,"degraded":0,"down":0,"unknown":0,"maint":0},
  "alerts": {"info":0,"warn":95,"crit":0},
  "meanLoadPct": 12, "throughputKbps": 340,
  "pools": [ {"name":"CORE","tier":0,"up":4,"degraded":0,"down":0,
              "unknown":0,"maint":0,"loadPct":18}, ... ],   // ≤8, ordered by tier
  "systems": [ {"name":"xenon","pool":0,"tier":0,"state":"up","loadPct":22}, ... ], // ≤32
  "topAlert": {"id":1234,"severity":"crit","subject":"BIND DOWN",
               "detail":"RESTART BIND ON MOLYBDENUM"} | null   // crit > warn, newest first
} }
```

- `tier`: from pool/system metadata where the DB has it; else derive
  (server/monitor roles → 0/1, clients → 2/3) and note the derivation in code.
- `topAlert.detail`: composed by the endpoint from alertRule.metric + node name
  (e.g. metric `reachable`, node molybdenum → "CHECK LINK ON MOLYBDENUM").
  Uppercase, ≤48 chars, panel-renderable charset [A-Z0-9 .:-].
- `score`: worst pool, not sum — tier-weighted severity exactly as
  DESIGN-BRIEF "FINAL DECISIONS" states (tier 0/1: any failure alarms;
  tier 2/3: ≥20% of pool AND ≥5 systems down).
- Performance bound: no per-node window queries; aggregates only.

## 4. Interface B — serial protocol (C2 ⇄ C3, shared)

Transport: USB CDC `/dev/ttyACM0`, 115200 8N1 (rate nominal — CDC ignores it).
Binary frames, little-endian:

```
u8  magic0 = 0xA5      u8  magic1 = 0x53 ('S')
u8  version = 0x01     u8  type
u16 payloadLen         u8[payloadLen] payload
u16 crc16-ccitt (poly 0x1021, init 0xFFFF, over version..payload)
```

Host→panel types:
- `0x01 SNAPSHOT` — full panel state (fixed-layout struct mirroring §3:
  globals, then poolCount×pool records, then systemCount×system records,
  then optional topAlert record; names fixed-width, NUL-padded:
  pool.name char[8], system.name char[12], alert subject char[24],
  alert detail char[48], alert id u32). Sent every 2 s and on daemon start.
- `0x02 PING` — empty payload, sent when no snapshot is due.

Panel→host types (logging/diagnostics only; daemon must tolerate silence):
- `0x81 HELLO` — firmware version string, sent at boot and on request.
- `0x82 EVENT` — u8 kind (button/ack/themeChange), u8 arg.
- `0x83 LOG` — ASCII text.

Rules: receiver resyncs by scanning for magic; bad CRC → drop frame, count it.
Firmware treats >15 s without any valid frame as LINK LOST and renders the
design's stale indicator. Daemon treats write errors/ENODEV as disconnect and
reopens the port with backoff (1 s → max 30 s). The exact C struct layout for
SNAPSHOT lives in a single shared header `status-panel/protocol.h`, authored
by C3, consumed verbatim by C2 — neither side redefines it.

## 5. Firmware behavior (C3)

Binding spec: DESIGN-BRIEF.md — all 4 themes × 3 screens, exact layouts,
colors, fonts (3×5 small, 4×7 numeral), tier-weighted severity, universal
alert inlay (red rails, 10% dim, 12 s two-tone re-alarm until ack, ack
silences not clears, new alert id re-arms). Ambiguity resolutions (Lead,
recorded): boot = Theme D screen 2 ("resting face"); dwell default 6 s;
`unknown` state color kept and rendered where data yields it; LUX± = global
brightness step, ZZZ = display sleep toggle; ANY button press during active
alarm is consumed as acknowledge; light-sensor auto-brightness is SHOULD
(implement if it doesn't threaten the deadline; LUX± overrides it).
Rendering is autonomous: the panel animates at its own tick from the last
snapshot; data staleness only triggers the LINK LOST treatment.

## 6. Daemon behavior (C2)

Standard C, camelCase, block-comment function contracts, Makefile (no cmake).
Dependencies: libcurl, vendored cJSON (single-file, ANSI C) — no other libs.
Config file `/etc/solari-panel.conf` (key=value): apiBase, user, passFile,
pollSec=5, serialDev=/dev/ttyACM0, snapshotSec=2. Password read from a
0600 file, never argv/env-visible. systemd unit `solari-panel.service`
(Restart=always, After=network-online.target, User=jason, group dialout).
Degraded modes are first-class: API down → keep serving last state, mark
stale after 30 s (firmware shows LINK LOST only if serial dies; API-stale
sets a staleness flag INSIDE the snapshot — add `u8 dataStale` to globals).
Serial gone → reconnect loop. Never exits on transient failure.

## 7. Acceptance criteria (observable, independently checkable)

1. `curl` /api/panel as viewer returns §3 shape in <150 ms on xenon; SPA and
   panel alert counts agree on the dashboard-active definition.
2. Firmware UF2 builds reproducibly on lithium (pico-sdk 2.1.1, arm-none-eabi
   14.2); `picotool info` confirms it after flash.
3. Daemon builds with `-std=c99 -Wall -Wextra -Werror` clean on lithium.
4. End-to-end: a state change visible in the dashboard (e.g. a node marked
   down, or an injected test alert) appears on the LED matrix within 10 s.
5. Alarm path: an active crit alert triggers the inlay + tone; button press
   acks (silences, doesn't clear); same alert does not re-alarm; a new alert
   id does.
6. Survives: daemon restart, firmware replug (port re-enumeration), xenon
   API 5-minute outage (panel shows stale, recovers alone).
7. `solari-panel.service` enabled, running, and boot-persistent on lithium.

## 8. Out of scope

MQTT transport (benzene down), write-back of acks to the server, audio beyond
the alarm two-tone, day/night scheduling beyond the light sensor SHOULD,
Panel.dc.html exploratory screens not carried into Themes.dc.html.

## 9. v1.1 amendments (review dispositions — binding)

**Wire protocol.** `status-panel/protocol.h` is NORMATIVE and supersedes §4's
prose. Byte-level fixed offsets, shared codec `protocol.c` compiled into BOTH
daemon and firmware; no struct casting to wire bytes; MAX_PAYLOAD 2048; frame
timeout 500 ms; overlapping-magic resync; seq-based duplicate/ordering guard;
HELLO carries protoVer + fw version; HELLOREQ exists. C2 implements
`protocol.c` against the header; C3 compiles it verbatim. Codec questions are
peer-channel business (§10); layout changes come to the Lead.

**Canonical semantics.** State enum: ok=0, degraded=1, down=2, unknown=3,
maint=4 (PanelState) — API strings map onto it, `up`→ok. Score is u16, range
per the DESIGN-BRIEF formula (uncapped, alarm thresholds per its FINAL
DECISIONS); the API and firmware use the same formula, implemented server-side,
sent on the wire — firmware never recomputes score. `alarmActive` (crit alert
OR tier-threshold breach) drives the inlay; `topAlert` is its content — if
alarmActive with no qualifying alertEvent, the endpoint synthesizes
subject/detail from the breaching pool. `episodeId` (u32, server-composed,
monotonic per alarm episode) is the re-arm key: change of episodeId re-arms
the tone; ack is firmware-local per episodeId.

**/api/panel additions.** Per-pool `total`; throughput split rx/tx (u32 kbps);
aggregate `rttTenthMs` + `lossPermille` from probeCurrent; `Cache-Control:
no-store`; `Content-Type: application/json`; response composed from one
consistent read (single transaction / one multi-query pass, `ts` = server time
at composition); reuse the dashboard's existing summary/alert derivation code
paths where they exist rather than re-deriving. Systems cap raised to 64,
selection = order by (tier, name), overflow systems still counted in pool
aggregates (which are computed server-side over the FULL fleet). Zero-data
(no nodes) is valid: empty arrays + dataStale=0 — firmware renders a NO DATA
treatment. Names: uppercase ASCII [A-Z0-9 .:-], truncated to wire width with
trailing '.'; history (ribbons/histograms/sparklines) is NOT in the API —
firmware accumulates it from successive snapshots.

**Daemon hardening.** libcurl in-memory cookie engine; TLS verify ON with
CURLOPT_CAINFO=/etc/solari-panel/ca.pem (the dashboard cert is self-signed —
the deploy step ships /etc/ssl/solarinet/dashboard.pem there); bounded
timeouts (connect 5 s, total 10 s); no redirect following; strict
content-type + top-level-JSON validation — an HTML login page must never
parse as data; exactly one re-login retry on 401, then exponential backoff
(auth failures backoff separately from transient fetch failures, 1 s→max
60 s, last good state retained and never overwritten by partial data).
Serial: open via config `serialDev` supporting a glob (default
`/dev/serial/by-id/*` match, tried in order; literal path allowed); raw
termios; write-all loop handling EINTR/EAGAIN with 2 s write deadline; on
reopen, reset parser state and send an immediate fresh snapshot. systemd:
`Wants=network-online.target` + `After=`, `RestartSec=3`, no start-limit
lockout (`StartLimitIntervalSec=0`). `dataStale` = API payload `ts` older
than 30 s at composition time (server-clock based, tolerating skew ±5 s).

**Acceptance criteria (§7) — replaced with testable forms:**
1. `curl -w` p95 < 150 ms over 100 authed sequential requests to /api/panel,
   run on xenon (server-local, warm), viewer session.
2. Equivalence: /api/panel `stateRoll` + active-alert count equal
   /api/summary's for the same instant (script compares both, 3 runs).
3. Firmware: `picotool info` reports program name `solari-panel-fw` and the
   version embedded via `bi_decl`; UF2 rebuild on lithium is byte-identical
   twice in a row (SOURCE_DATE_EPOCH pinned).
4. Daemon builds `-std=c99 -Wall -Wextra -Werror` clean on lithium.
5. E2E latency: insert a synthetic crit alertEvent row on xenon (test rule,
   clearly named, cleared afterwards); the alarm inlay activates on the LEDs
   within 10 s of the INSERT (observed via the daemon journal's 0x82
   ACK/EVENT log + operator eyes); button press acks (0x82 EV_ACK in
   journal); same episodeId does not re-alarm; bumping episodeId (second
   synthetic alert) does.
6. Resilience: `systemctl restart solari-panel` recovers < 10 s; USB replug
   recovers < 15 s (by-id reopen); 5-min API block (iptables on lithium)
   → dataStale renders, then auto-recovery, no daemon restart.
7. `systemctl is-enabled && is-active` solari-panel on lithium; survives a
   lithium reboot (tested once at deploy).

## 9a. v1.1.1 — review-round spec dispositions (Lead)

- **S1 accepted**: `panelSeqNewer()` added to protocol.h; protocol.c implements
  it; both sides call it, never hand-rolled.
- **S2 accepted**: score is u16, formula-defined (currently 0..~140); the
  "0-100" in §3 and "0..1000" in early protocol.h are superseded.
- **P8 REJECTED**: the DESIGN-BRIEF's formula block explicitly includes
  `pool.down >= 5` for tiers 2/3 with stated rationale (nuisance-alarm guard
  on small pools). The endpoint implements the brief verbatim. Stands.
- **P9 accepted (amended)**: the endpoint sets dataStale=1 when
  MAX(hostCurrent.sampledAt) is older than 30 s (monitoring-data age); the
  daemon ADDITIONALLY forces dataStale=1 when the API payload ts it last
  fetched is older than 30 s. Either source suffices.
- **P10 noted**: maint is reserved-zero on the wire until the server grows a
  maintenance state; firmware keeps its maint rendering path.
- **U2 accepted**: config path is /etc/solari-panel/solari-panel.conf
  (directory form); §6's flat path is superseded.

## 9b. Known limitations (recorded at ship, re-check round)

- **Episode persistence (P1 residual)**: episodeId = MIN(eventId) over active
  crits narrows but does not eliminate mid-episode re-arm — if the OLDEST
  active crit clears while others remain, episodeId moves and an acked alarm
  re-arms once. A correct fix needs server-side episode state; deferred.
- **Tier 2 is presently unreachable (P7)**: no `standby` value exists in
  node.role or asset.class today, so the tier-2 threshold path is dormant
  until such data appears. The derivation comment in panel.php documents it.
- **Two populations by design (P4)**: stateRoll counts agent NODES (matches
  /api/summary, 9 today); pools/systems additionally include adopted
  probe-only assets (15 today). Screens must not present the two as the same
  total; the firmware treats stateRoll as "the dashboard headline" and
  pools/systems as "the panel roster."
- D16/D17/U3-docs and remaining N-series SHOULD/NITs: deferred with owner
  (next maintenance pass on this component set).

## 10. Peer channel grant

PEER      C2 (daemon author) ⇄ C3 (firmware author)
CHANNEL   via Lead-relayed notes in status-panel/PEER-NOTES.md (append-only)
SCOPE     protocol.c codec semantics, HELLO/EVENT payload details, timing
BUDGET    coordination only; interface/layout changes escalate to Lead
