# RETURN — PANEL-CP4 · Dashboard Panel page

`worker: claude-opus-5 · task: PANEL-CP4 · governed by CONTRACT-CP.md v1.1 §7 + §10`
`status: COMPLETE — no git operations performed, nothing deployed`

---

## 1. Files touched

| Path | Change |
|---|---|
| `/home/jason/Code/Solarinet/dashboard/public/screens-panel.jsx` | **new** (~1780 lines) — `window.PanelScreen` |
| `/home/jason/Code/Solarinet/dashboard/public/styles.css` | **appended** one block, `.vp-*` (no existing rule edited) |
| `/home/jason/Code/Solarinet/dashboard/public/index.html` | **+1 line** — `<script type="text/babel" src="screens-panel.jsx">` between `screens8.jsx` and `app.jsx` |
| `/home/jason/Code/Solarinet/dashboard/public/app.jsx` | **+3 lines** — window destructure, route line, command-palette entry |
| `/home/jason/Code/Solarinet/dashboard/public/components.jsx` | **+1 line** — `NAV` entry `{ id: "panel", label: "Panel", icon: "grid" }` in the Monitor group |
| `/home/jason/Code/Solarinet/status-panel/fixtures/panel-snapshot.json` | **new** — the committed §10 parity fixture |

No API, daemon, firmware or protocol file was read-modified. No other dashboard
screen was touched beyond its registration line.

---

## 2. Acceptance gates — all run, all green

```
$ node -e "babel.transform(<file>, {presets:['react']})"   # vendor/babel.min.js
screens-panel.jsx  babel ok      (81120 bytes emitted, then new Function() parse-checked)
app.jsx            babel ok
components.jsx     babel ok
styles.css brace balance: 0   min depth: 0
index.html script tags: 18  closers: 18
```

### 2.1 The renderers were actually exercised, not just read

I cannot screenshot, so instead of asserting from a code read I ran the ported
renderers headlessly. The harness Babel-transforms the shipped file, swaps only
the final `Object.assign(window, …)` for a `module.exports`, and runs it in node
with a React proxy — so **the code under test is byte-identical to the code the
browser loads**, minus the canvas paint (which needs a real 2D context).

Harness: `/tmp/claude-1000/…/scratchpad/render-harness.js` (scratch, not committed).

Every screen was painted in every fixture scenario with the same deterministic
warm-up the page uses (75 steps of dt = 0.04 → t = 3.0):

```
=== normal ===  total=9 up=9 down=0 | histFill=12 gaps=0 peak=504897kbps gLabel=403M armed=false
                pools=4 worstFirst=[STOR,APPS,WKS,CORE] probeRows=11
  A0 State lattice     lit>0.5=  15   B0 Condition counts  lit>0.5= 177
  A1 Flow gates        lit>0.5= 191   B1 Throughput        lit>0.5= 272
  A2 MQ and SNMP       lit>0.5= 263   B2 Load and latency  lit>0.5= 196
  C0 Load distribution lit>0.5= 172   D0 Reachability      lit>0.5= 583
  C1 Live traces       lit>0.5= 245   D1 Pool bars         lit>0.5= 218
  C2 Pool load         lit>0.5= 267   D2 Ambient field     lit>0.5= 563

=== alarm ===   armed=true, worstFirst=[CORE,APPS,WKS,STOR]  (tier-0 CORE sorts first)
                all 12 screens mean≈53, max=217 — the inlay's dimAll(0.1) + pulsing
                crit rails dominate every screen, which is the spec working
=== stale ===   A2 lane-1 backlog rises (73 → 99 max) off dataStale; D0 texture shifts
=== emptyHistory === histFill=1, every ring reads the oldest-real clamp, no blanks
=== noData ===  total=0 → all 12 tiles render the NO DATA treatment (identical stats)

LINK LOST  {"lit":74,"mean":18.39,"max":146,"over":0,"neg":0}
NO DATA    {"lit":77,"mean":15.13,"max":139,"over":0,"neg":0}

all screens painted, no empty or negative framebuffers
```

Font/format spot checks against the C: `textW("LOAD") = 15`, `bigW("100%") = 19`,
`formatRate` → `424M / 1.2G / 24G / 0.9M`, `driverGain(0) = 0.25` (the
`panelHw.cpp:70` floor).

Note on A0's low lit-count under `normal`: `per = ceil(15/583) = 1`, so 15 of 583
lattice slots are owned and the rest stay black. That is `panelScreensA.c:34`
verbatim — "slots with no system behind them stay black, which is how the
prototype degrades at small fleet sizes too" — not a bug.

### 2.2 The §10 gap-aware resampler was tested separately

No fixture scenario contains a gap, so I tested it directly
(`scratchpad/gap-test.js`):

| case | result |
|---|---|
| 60 samples at 5 s | `histFill=53 gaps=0`, newest slot normalises to 1.000 |
| 3 samples at 5 s | `histFill=3 gaps=0`, `histAt(0) === histAt(50)` (the `panelHist.c:50` clamp) |
| 5 samples, 4-minute hole, 5 more | `histFill=53 gaps=41` — interior gaps hold the previous value and are counted |
| 1 sample | `histFill=1`, every slot equals the newest |

The gap count surfaces on the page as a chip (`41 history gaps`), so an operator
can see when the ring is padded rather than measured.

---

## 3. Provenance — every renderer cites both sources (§10)

Each function in `screens-panel.jsx` carries a header naming its mockup function
with a `Themes.dc.html` line, and its firmware `file:line`. Where the two
disagree the **firmware** is reproduced, because this page exists to show what
the panel is actually painting.

| Screen | Mockup | Firmware | Firmware deviation reproduced here |
|---|---|---|---|
| A0 | `sA0()` :408 | `panelScreensA.c:36-75` | the `UNKNOWN` branch (`A.c:59-67`) — `cUnknown` #7C8AA0 at the maint dash rate; the mockup never emits UNKNOWN |
| A1 | `sA1()` :436 | `panelScreensA.c:88-156` | WAN ramps read the newest normalised `egress`/`thru` slots, not the fixed 25/18 Gbps divisors (`A.c:135-155`) |
| A2 | `sA2()` :474 | `panelScreensA.c:173-207` | lane backlogs driven by loss / `dataStale` / unknown-share / degraded-share, since protocol.h has no MQ/SNMP telemetry (`A.c:158-172`) |
| B0 | `sB0()` :498 | `panelScreensB.c:16-56` | hue-tier pass (`B.c:29-48`): `cQuiet` never used for small text; tier flag from pool aggregates, not the 64-capped systems array |
| B1 | `sB1()` :519 | `panelScreensB.c:63-98` | real IN/OUT split + adaptive-peak `LINK %` in the ticker (`B.c:88-97`) |
| B2 | `sB2()` :535 | `panelScreensB.c:108-180` | **DEVIATION 14** — label contracts to `LOAD`, P95/LOSS alternate on a 5 s slot right-aligned at `rMinX = textW("LOAD")+3`; **DEVIATION 13** — `"%d/%d>85"` with the §9b denominator, floored at `minX = bigW(pct)+3` |
| C0 | `sC0()` :547 | `panelScreensC.c:25-54` | chronological indexing; the mockup's synthetic scroll offset dropped (`C.c:6-12`) |
| C1 | `sC1()` :567 | `panelScreensC.c:61-82` | same |
| C2 | `sC2()` :588 | `panelScreensC.c:90-116` | **DEVIATION 8** severity order down > degraded > unknown > maint |
| D0 | `sD0()` :609 + `sample()` :626 | `panelScreensD.c:38-87` | rows are the 11 worst-first systems, not 11 named probe targets (`D.c:18-24`) |
| D1 | `sD1()` :634 | `panelScreensD.c:95-143` | worst-first row ordering (`D.c:101-103`) and the `nu` UNKNOWN band (`D.c:121/126/134`) |
| D2 | `sD2()` :657 | `panelScreensD.c:152-207` | `downTier` from the first down system in **wire** order |
| inlay | `inlay()` :687 | `panelInlay.c:20-43` | — |
| beacon | *(none)* | `panelInlay.c:63-67` | operator amendment 2026-08-04; neither the mockup nor the DESIGN-BRIEF has a beacon |
| LINK LOST / NO DATA | *(none)* | `panelInlay.c:76-95` | — |

Supporting ports: framebuffer primitives ← `panelFb.c:11-48`; fonts, the packed
glyph tables and **`panelTextInk` = `0.85 + 0.15*b`** ← `panelFont.c:17-175`
(small text only — never `panelBig`, never bars/ramps/particles, never a hue);
`buildEnv` ← `panelHist.c:124-273` including both insertion-sort comparators,
the 35-bucket histogram, the 583-slot lattice, `stateRank`, `panelFormatRate`
and the adaptive peak (floor 1000 kbps, decay 0.997); paint order ← `main.c`
(no-data → screen → inlay → beacon **last**).

---

## 4. Behaviour delivered (§7)

- **Grid**: 12 canvas tiles, A0…D2, labelled with theme name and screen title,
  each a 53×11 round-dot LED sim on a dark bezel. The tile the panel is
  currently showing (`panelState.theme*3 + screen`) is highlighted and carries
  an `ON PANEL` badge.
- **Live**: `GET /api/panel` every 5 s via `SolariAPI.get`; history rings are
  accumulated client-side and resampled on the **server** `ts`, gap-aware.
- **State chip row**: theme/screen, brightness + AUTO/MANUAL, awake/asleep,
  alarm armed / acknowledged, upstream `dataStale`, panel-state staleness
  (`reportedAt` older than 90 s, with age), history-slot count and gap count.
- **Alarm**: the inlay and the beacon are simulated whenever `alarmActive` and
  the current `episodeId` is not the acknowledged one — same predicate as
  `main.c`'s `gAlarmArmed`.
- **Brightness fidelity**: canvases are gained by the driver's global
  brightness, floored at 0.25 (`panelHw.cpp:70`), so a virtual screen dims down
  but never out — as the hardware does.
- **Controls** (`POST /api/panel/command`): theme 0-3, screen 0-2, brightness
  slider + AUTO, ACK (disabled unless an unacknowledged episode is armed),
  dwell 3/6/30, sleep/wake. A control group is disabled and shows `pending`
  from submission until its queue row reports `applied` or `expired`; an
  expired row shows `failed` with the server's `failureReason`, and a row that
  never appears within 130 s (120 s expiry + one poll of slack) shows `failed`
  with that reason. **Nothing re-enables on a timer** — only a terminal status
  does. Failures also render as a persistent banner naming the command.
- **Role, fail closed**: the page seeds from `window.SOLARI.operator` and then
  confirms with `/api/auth/whoami`. Controls render only for a **confirmed**
  `operator` or `admin`. A viewer, a missing profile, a failed or still-pending
  whoami, a missing `SolariAPI`, or fixture mode → controls hidden, page
  read-only, with the reason stated. Authority remains server-side; the page
  never assumes its own gate held.
- **Footnote**, once, as §10 requires: the sim is a **~5 s cadence
  approximation** and will differ in ribbon/trace detail from the panel even on
  identical fleet data.
- **`?fixture=1`**: renders the committed fixture with **no network calls to the
  API**, no rAF, a fixed clock (t = 3.0 reached in 75 fixed dt = 0.04 steps) and
  freshly reseeded per-screen state, so two reviewers on the same build see the
  same pixels. A scenario picker exposes all five scenarios with their notes.

---

## 5. Deviations I introduced (page-only), and why

1. **Per-tile animation state.** `panelScreensA.c`'s `gA1[]` and
   `panelScreensD.c`'s `gWf/gWfRng/gWfStep` are file statics because exactly one
   screen is live on the hardware. Twelve are live here, so A1's 90 particles
   and D0's waterfall are per-tile, each seeded with the firmware's own seeds
   (77 and 4211). The panel's single instance is unaffected; the twelve tiles
   simply do not share one particle array.
2. **Peak decay is applied per resampled slot.** The firmware decays
   `gPeakKbps` once per snapshot arrival. The page replays the decay
   chronologically across the 53 resampled slots so old slots normalise against
   the then-current peak. Across a gap the held value keeps the peak pinned —
   which is also what the firmware does, since no sample arrives to decay it.
3. **Sleep is an overlay, not a blanked canvas.** `main.c` blanks the
   framebuffer when sleeping. Blanking all twelve tiles would destroy the
   preview the operator needs in order to choose what to wake to, so sleep is
   drawn as a CSS `ASLEEP` overlay on the on-panel tile only.
4. **LINK LOST is reference-only.** CONTRACT §4 defines it as 15 s without a
   valid frame — a panel↔daemon fact the dashboard cannot observe (the server
   only knows when the panel last POSTed STATE, which is a different fact). I
   did not infer it. Instead both universal treatments render as static
   reference tiles below the grid, so every paint path in `panelInlay.c` is
   visible and reviewable on this page.
5. **Canvas compositing rather than per-LED `arc()`.** 583 LEDs × 12 screens ×
   25 fps is ~175 k `arc()` calls/s. The painter writes the framebuffer as a
   53×11 `ImageData`, nearest-neighbour upscales it, masks it to round dots with
   a cached `destination-in` stencil, and composites with `lighter` over a
   cached unlit-package layer — ~4 `drawImage` calls per tile per frame,
   independent of how many LEDs are lit. The composite reproduces the mockup's
   `paint()`: unlit `#0E1014`, lit cell = `rgb(14+R, 16+G, 20+B)`.

---

## 6. UNVERIFIED

Required field, and genuinely non-empty.

1. **No pixels were seen.** I have no browser and cannot screenshot. What is
   verified is that every renderer produces a non-empty, non-negative
   framebuffer with plausible statistics in all five scenarios, and that the
   ports match the C line by line. **Whether the page *looks* right is
   unverified** and needs a human or a screenshot-capable reviewer.
2. **The canvas paint path never executed.** `getLayers()` / `paintCanvas()`
   need a real 2D context; node has none. The compositing pipeline
   (`destination-in` mask, `lighter` composite, the bloom pass) is unexercised.
   This is the single most likely place for a visual defect.
3. **Against the live API: never run.** I coded to the CONTRACT-CP §4/§10
   response shape, cross-checked against the keys `dashboard/api/routes/panel.php`
   actually emits, but no request was issued. The command lifecycle
   (submit → `pending` → `applied`/`expired`), the 409 queue-full path and the
   403 role rejection are **untested end-to-end**.
4. **Role gating not exercised against a real session.** The fail-closed logic
   is verified by reading; no viewer/operator/admin session was used.
5. **Fixture discovery path.** `?fixture=1` fetches
   `fixtures/panel-snapshot.json` relative to the docroot, then
   `panel-snapshot.json`, then `../status-panel/fixtures/…`. **The repo file is
   not under `dashboard/public/`**, so parity mode will show its "fixture not
   found" error until deployment copies it:
   `cp status-panel/fixtures/panel-snapshot.json /var/www/solarinet/fixtures/`.
   Adding a second file under `dashboard/public/` was outside IN-SCOPE, so this
   is flagged rather than done — **it needs a deploy-step decision from you.**
6. **`reportedAt` timezone.** The API returns the raw DB datetime string; I parse
   `YYYY-MM-DD HH:MM:SS` as **UTC**. If the MariaDB session runs local time the
   staleness chip will be wrong by the offset. Worth one check against a live
   `panelState`.
7. **Dwell can never show truth.** The STATE frame (§3) carries no dwell field,
   so the control confirms collection only. No dwell button renders as selected,
   and the UI says so. If dwell needs to be reflected, §3 needs a STATE byte —
   that is a CP5 amendment, not a page fix.
8. **Performance under twelve live canvases** is reasoned, not measured. No
   frame-time profile was taken on real hardware.
9. **Light theme** styling uses existing tokens throughout but was not viewed.

---

## 7. Not done, deliberately

- **No git operations.** Nothing staged, committed, branched or pushed.
- **Nothing deployed.** The repo copy is not live; `cp` to `/var/www/solarinet`
  is yours.
- The API, daemon, firmware and protocol files were read but never modified.

---

## FIX ROUND 1

Cross-lab review verdict FIX-THEN-SHIP. Both code MUST-FIXes are done. Nothing
else in the file was touched — no renderer, no fixture, no CSS, no registration
line changed. Still no git operations, still nothing deployed.

### MF-1 · role gating was not fail-closed (screens-panel.jsx, whoami effect)

**Was**: the effect read `window.SOLARI.operator` and, if that hint already
named an operator or admin, promoted it to the effective role immediately —
so between mount and the whoami response the controls were live on the strength
of a client-side hint. The reviewer is right: §10 requires read-only under
*every* whoami outcome, and "not answered yet" is one of them.

**Now**: the seed is gone. The only assignment that can produce a controlling
role is the resolved `/api/auth/whoami` response. The effect explicitly sets
`role` back to `undefined` on entry, so a remount or a fixture-mode toggle
re-closes the gate rather than inheriting the previous resolution. Missing
`SolariAPI`, rejected whoami, and a whoami that never answers all land on
`role === null` or stay `undefined`; `canControl` requires a literal
`"operator"` or `"admin"`, so all three render read-only. There is now **no
pre-resolution promotion path in the file** — `window.SOLARI` is not read at
all any more (the only remaining occurrence of the name is the comment
explaining why it must not be).

### MF-2 · poll ordering (screens-panel.jsx, /api/panel effect)

**Was**: `if (sampleMs(prev[last]) === ms) return prev` — only an *equal* ts was
rejected. A response completing out of order carried an older ts and was
appended, pushing stale data into the history rings and regressing the visible
fleet state.

**Now**, two guards:

- **Monotonic on the server ts.** `lastMs` tracks the newest ts actually
  applied. `ms < lastMs` is dropped outright — no history, no reconcile, since
  nothing in a reordered response is current. `ms > lastMs` appends. An **equal**
  ts still adds no history but *does* reconcile: the collector may not have
  produced a new snapshot yet while a command has since moved to `applied`, and
  dropping the whole response would have stalled the command lifecycle. That
  distinction is deliberate and commented.
- **In-flight guard.** One request open at a time; if the 5 s timer fires while
  a request is still open, that slot is skipped rather than starting a racing
  request. `inFlight` is cleared on both the resolve and the reject path.

### Re-run acceptance — all green

```
screens-panel.jsx  babel ok 82631
app.jsx            babel ok 27554
components.jsx     babel ok 31651
styles.css brace balance: 0  min depth: 0
headless fixture render: 12 screens x 5 scenarios, no empty or negative framebuffers
gap resampler: gapped histFill 53 / gaps 41; single histFill 1, all slots equal
```

Renderer output is byte-for-byte what it was before this round, as expected —
neither fix touches the paint path.

### New: both fixes are now regression-tested, not just re-read

`scratchpad/poll-role-test.js` mounts the **shipped** `PanelScreen` against a
minimal hooks runtime (real useState/useEffect/useMemo/useCallback/useRef
semantics, effects flushed after render) with a stubbed `SolariAPI`, so the
fixes are exercised rather than asserted:

```
PASS  FIX1 seed does NOT promote: no controls while whoami unresolved   window.SOLARI.operator.role = admin
PASS  FIX1 read-only note says confirming
PASS  FIX1 controls appear only after a confirmed admin whoami
PASS  FIX1 failed whoami stays read-only   reason shown
PASS  FIX2 in-flight guard: no second request while one is open   requests before=1 after=1
PASS  FIX2 older ts REJECTED (state does not regress)   up=2 (99 would mean the stale row was appended)
PASS  FIX2 equal ts adds no history
PASS  FIX2 next newer ts still applies
```

The first assertion is the real regression test for MF-1: the harness leaves
`window.SOLARI.operator = { role: "admin" }` in place, which is exactly the
condition that used to unlock the controls early, and the controls stay disabled
until whoami answers.

This **supersedes UNVERIFIED item 4** ("role gating not exercised") for the
client-side logic. It does not supersede item 3 — nothing has yet run against a
real session or a real `/api/panel`, and server-side authority remains the
actual enforcement point.

### Note on presentation, unchanged and deliberate

Read-only does not unmount the controls panel; it renders every control
`disabled` with the reason stated in the panel header ("Confirming your role…",
"Your role could not be confirmed…", "Your role is viewer…"), plus a `Read-only`
chip in the page header. An operator can see what the page would offer and why
it does not, which a hidden panel cannot communicate. Every button carries
`disabled={off || …}`, so the gate is on the controls themselves, not on layout.

---

## FIX ROUND 2 — dwell confirmation via `panelState.dwellSec`

The API now serves `panelState.dwellSec` (u8 seconds, 0 = unreported; migration
applied live). The dwell control is wired to it. Two files changed and nothing
else — no renderer, no CSS, no registration line, no git operations, nothing
deployed.

### Page — `dashboard/public/screens-panel.jsx` (`PanelControls`)

```
dwellPend  = busyKey("dwell")
dwellSec   = num(state && state.dwellSec)            // null / absent / 0 all ⇒ 0
dwellOther = dwellSec > 0 && [3,6,30].indexOf(dwellSec) < 0
on         = !dwellPend && dwellSec === n
```

| `panelState.dwellSec` | selection | note |
|---|---|---|
| 3 / 6 / 30 | that chip only | "The panel reports *n* s in force." |
| 0, absent, or `panelState` null | **none** | "The panel has not reported a dwell yet, so no interval is shown as in force." |
| any other value (e.g. 12) | **none** | "The panel reports 12 s, which is not one of the presets — none is marked selected." |
| dwell command pending | **none**, `CmdState` pending badge retained | "Waiting for the panel to report the new dwell." |

The rule the code enforces is that **a selection means the panel said so, never
that we sent it**. Hence two behaviours worth your sign-off:

- **Pending suppresses selection** rather than leaving the old value lit. If the
  operator moves 6 s → 30 s, leaving 6 s selected while the command is in flight
  reads as "still 6 s, confirmed", and leaving it lit next to a pending badge on
  the *same* control is worse. Nothing is selected until the panel reports.
- **0 is neutral, not a default.** Unreported is not the same as 3 s, and the
  page never guesses a default the firmware might not be running.

This supersedes UNVERIFIED item 7 and the §4 "dwell confirms collection only"
bullet. The control now confirms the dwell actually in force, from the panel.

### Fixture — `status-panel/fixtures/panel-snapshot.json`

Regenerated; the diff against the previous file is **only** `dwellSec` fields
plus the four scenario notes (verified by sorted-JSON diff — no other key, value
or sample moved). Coverage across the four branches:

| scenario | `dwellSec` | branch exercised |
|---|---|---|
| `normal` | 6 | preset, selected |
| `alarm` | 3 | preset, selected, alongside an armed alarm |
| `stale` | 0 | unreported, neutral |
| `emptyHistory` | *(`panelState` is null)* | null-state path |
| `noData` | 12 | deliberately non-preset — reported, nothing selected |

### Acceptance — full battery re-run, all green

```
screens-panel.jsx  babel ok 82922
app.jsx            babel ok 27554
components.jsx     babel ok 31651
styles.css brace balance: 0  min depth: 0
fixture JSON parses ok
headless fixture render: 12 screens x 5 scenarios, no empty or negative framebuffers
gap resampler: gapped histFill 53 / gaps 41; single histFill 1, all slots equal
```

Renderer output is unchanged, as expected — `dwellSec` never reaches the paint
path.

The regression harness now mounts `PanelControls` directly and asserts the
selection and the note for every branch. 19/19 assertions pass:

```
PASS  DWELL presets are 3/6/30   [3,6,30]
PASS  DWELL dwellSec 6 selects exactly the 6 s chip   note=The panel reports 6 s in force.
PASS  DWELL dwellSec 3 selects exactly the 3 s chip   [3]
PASS  DWELL dwellSec 0 selects nothing (neutral, not a default)
PASS  DWELL absent field behaves as 0
PASS  DWELL null panelState selects nothing
PASS  DWELL non-preset 12 s selects nothing but is reported
PASS  DWELL pending suppresses selection and keeps the pending treatment
```

(plus the eleven FIX ROUND 1 assertions, still passing.)

### UNVERIFIED, this round

- `dwellSec` has **not** been seen coming off the live API or the real panel.
  The page's contract with it is `num()`-coerced and total — string, null,
  absent and out-of-range all land on a defined branch — but the round trip
  *set dwell → panel applies → STATE reports dwellSec → chip lights* has not
  been exercised end to end. That needs the panel, and it is the one thing worth
  watching on first live use.
- No screenshot. The selected-chip styling is the existing `.chip.on` used by
  theme/screen/power, so it inherits their appearance, but it has not been seen.
