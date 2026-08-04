# SolariNet Status Panel — Design Brief

Source: Claude Design prototypes in `status-panel/project/`. Canonical/final spec is
`Galactic Unicorn Themes.dc.html` ("Turn 3 · Four themes, twelve screens, one alarm path").
`Galactic Unicorn Panel.dc.html` ("Turn 1"/"Turn 2") is exploratory pre-work; Theme D in
Turn 3 explicitly reuses those faces ("The faces from the earlier turns, kept intact") so it
is cited only where Turn 3's own code differs in a material way. Where the two disagree,
**Turn 3 wins** — it is the file the user had open at handoff and is captioned as containing
"final implementation decisions."

All framebuffer/behavior details below are read directly out of the prototype's embedded JS
(`<script type="text/x-dc" data-dc-script>` in each file), not inferred from prose.

---

## Display model

- Grid: **53 columns × 11 rows** = 583 pixels/LEDs, matches Galactic Unicorn hardware exactly. Confirmed explicitly: "Note · the Galactic Unicorn grid is 53 × 11, not 58 × 11."
- Coordinate origin: (0,0) top-left, x increases right (0..52), y increases down (0..10).
- Framebuffer is float RGB per pixel (`Float32Array(W*H*3)`), values scaled 0..255 at paint time by a global brightness multiplier (see Interaction).
- No named "zones" are fixed across all screens — each screen lays out its own regions (see per-screen coordinates below). The one universal region is the **alert inlay** (see Themes).

## Themes (Turn 3 — final)

Four themes, each bound to one physical button (A/B/C/D). Each theme holds **3 screens** that auto-cycle on a dwell timer; pressing the *active* theme's button advances that theme's screens by hand (does not change theme). Pressing a *different* theme's button switches to that theme at its screen 0.

| Btn | Theme | Screen 1 | Screen 2 | Screen 3 |
|---|---|---|---|---|
| A | ABSTRACT (no glyphs) | State lattice | Flow gates | MQ and SNMP |
| B | TEXT | Condition counts | Throughput | Load and latency |
| C | CHARTS | Load distribution | Live traces | Pool load |
| D | INSTRUMENTS | Reachability | Pool bars | Ambient field |

- `screenLabel` format: `"Screen {n} of 3 · {name}"`.
- Screen auto-advance: `screenT` timer resets to 0 on dwell elapse or on manual button press; increments `screen = (screen+1) % 3` (theme, not global).
- Dwell timer options offered in the mockup: **3 s / 6 s / 30 s**, default state `dwell: 6` (i.e. 6 s is the shipped default — no default explicitly marked "recommended" in Turn 3 prose, but state init is `dwell: 6`).
- Update/render loop: 40 ms tick (`setInterval(this.tick, 40)`) ⇒ **~25 Hz logical update rate**. (Hardware supports up to ~300 fps LED refresh per Pimoroni summary; the design's own animation clock runs at 25 Hz, well under that ceiling — treat 25 Hz as the target logic tick, not a hardware limit.)

### Colors (exact hex / RGB, Turn 3 canonical palette `C`)

| Role | RGB (0-255) | Hex |
|---|---|---|
| ok | `61, 214, 140` | `#3DD68C` |
| warn | `245, 166, 35` | `#F5A623` |
| crit | `255, 77, 94` | `#FF4D5E` |
| maint | `155, 135, 255` | `#9B87FF` |
| azure (accent/link) | `34, 184, 240` | `#22B8F0` |
| quiet (idle/background-state) | `78, 107, 126` | `#4E6B7E` |
| ink (text/generic) | `223, 232, 242` | `#DFE8F2` |

Turn 1/2 file (`Panel.dc.html`) additionally defines an `unknown` role: `124, 138, 160` / `#7C8AA0` — **not present in Turn 3's palette**. Flag as ambiguity (see below): the final theme file never emits an "unknown" state, only `ok/deg/down/maint`. Recommend keeping `unknown` = `#7C8AA0` in firmware as a defined-but-currently-unused state color for forward compatibility (e.g. host-unreachable-to-monitor vs down).

No separate "background" color is defined as a framebuffer value — background is implicit black/near-black; the mockup UI chrome (not the LED panel itself) uses `#070A0F` / LED-off DOM color `#0E1014`, which is cosmetic to the prototype's on-screen simulation only and not part of the panel's own color contract.

### Brightness / day-night

- Single global brightness scalar `brightness` in state, range **25%–100%**, step 5%, default **0.85 (85%)**. Applied as a flat multiplier `g` to every channel at paint time: `R = min(255, round(fb[i]*g))` etc.
- No day/night auto-brightness rule is specified anywhere in either file — this is a pure user-set value (or implied to map to a physical `+`/`−` brightness button pair; see Interaction). **Ambiguity**: no automatic dimming by time of day or by the phototransistor light sensor, despite the sensor being physically present (see Hardware constraints). Recommend: leave auto-brightness as a v2 feature; ship with the manual scalar only, since the design commits to nothing else.
- Per-tier "heat" scaling exists *within* Theme A's state-lattice screen (brightness multiplier by tier, not a global day/night rule) — see Screens.

## Screens — exact layouts (Turn 3 canonical, `sA0..sD2` + `inlay`)

Constants used throughout: `W=53, H=11`. Bitmap font `F` is 3×5 px per glyph, 4 px pitch (1 px gutter) — `textW(s) = len(s)*4 - 1`. Big numeral font `BIG` is 4×7 px per glyph, 5 px pitch — `bigW(s) = len(s)*5 - 1`.

### Theme A · Abstract (no glyphs)

**A0 — State lattice.** Fills the *entire* 53×11 grid, one cell per system-bucket (systems sorted by tier then by state-rank, bucketed evenly across `W*H = 583` slots so it degrades gracefully above 583 systems). Per cell:
- `ok`: `quiet` color, brightness `(0.1 + load*0.14) * (0.6 + heat*0.5) + small sine flicker`, where `heat = [1, 0.62, 0.4, 0.26][tier]` (tier 0 brightest).
- `deg`: `warn` color, brightness `~0.3–0.8` pulsing at 2 Hz-ish (`sin(t*2 + phase)`).
- `down`: `crit` color, brightness `0.3 + 0.68*heat*(...)`; tier-0 downs additionally pulse via `abs(sin(t*3))`.
- `maint`: `maint` color, brightness `0.2 + 0.16*heat` plus a 1.4 Hz on/off dash (`floor(t*1.4+phase)%2`).

**A1 — Flow gates.** 4 horizontal "lanes" at y = 1, 3, 5, 7, representing pools CORE/APPS/DMZ/WKS. Lane trunk drawn `quiet` from x=0..33 (b=0.045). A vertical "internet gate" bar at **x=34**, all rows, `azure`, brighter at y=5 (0.4) else 0.09–0.14. Animated particles per lane travel left→right along the trunk then converge to y=5 past the gate (speed scaled by pool load/state: down=0.1×, deg=0.5×, ok=1×), colored `crit`/`warn`/`azure` by pool state. From **x=44 (wanX) to x=52**, two WAN utilization ramps: row y=3 = upload gauge (`up = min(1, gbps/25)`), row y=7 = download gauge (`dn = min(1, gbps/18)`), both `azure` when under threshold else `quiet`. Row y=9 across x=44..52 is a loss/RTT indicator: `crit` if `loss>1`, `warn` if `rtt>30`, else `ok`.

**A2 — MQ and SNMP.** 4 horizontal lanes at y = 1, 3, 6, 8 representing message/poll queues (rates 0.7/0.45/0.9/0.3, colors azure/azure/ok/ok baseline). Each lane: trunk `quiet` from x=4..52; a 3-cell backlog indicator at x=0..2 (`crit` if backlog>0.5 else `warn`, else `quiet`); 5 traveling packets per lane animated along x=5..52, colored `warn` if lane backlogged else lane's base color. A faint dotted center line at y=5 (every 4th column). Two `azure` marker pixels at (0,10) and (1,10).

### Theme B · Text

**B0 — Condition counts.** Big 4×7 numerals at (1,2): total **UP** count (`quiet`, dim watermark), with "UP" label at (1,0) in `ok` (uses `textOver` — a bordered/knockout overlay technique: dims a 1px halo around each watermark pixel by 0.12 then draws numeral on top). At `x = max(22, 2+bigW(up)+3)`: **DOWN** count (`crit` if >0 else `quiet`, b=0.9/0.16) + "DOWN" label; then a tier flag word — `"TIER 0"` in `crit` if any down system is tier-0, else `"TIER 3"`, else `"OK"` if none down. Row y=6: **DEG** count (`warn`) + "DEG" label, then **MNT** count (`maint`) + "MNT" label, packed left-to-right.

**B1 — Throughput.** Big numeral aggregate throughput label (e.g. `"3.2G"`) at (1,2), `quiet`, with "AGGREGATE" label at (1,0) in `azure`. Starting at `x0 = max(26, bigW(gLabel)+4)`: a 24-cell horizontal link-utilization bar at rows y=1 (main, b=0.5 lit/0.05 dark) and y=2 (echo, b=0.18/0.03); cell color `azure` under 70% util, `warn` 70–85%, `crit` above 85%. Percent label + "LINK" text at y=4 (`warn` if >85% else `ink`). Scrolling ticker at y=6: `"IN {gLabel}/S · OUT {0.6×gbps}G/S · PEAK {1.4×gbps}G · DMZ UPLINK {pct}%"`, color `ink`, brightness 0.45, speed 11 px/s.

**B2 — Load and latency.** Big numeral mean-load percent at (1,2) (`warn` if meanLoad>75% else `quiet`), "MEAN LOAD" label at (1,0) `ink`. At `x0 = max(26, bigW(pct)+4)`, row y=0: `"P95 {rtt}MS"` (`warn` if rtt>35 else `ink`) followed by loss percent (`crit` if loss>1% else `quiet`). Row y=6: count of systems with load>85% + `"OVER 85"` (`warn` if any, else `quiet`).

### Theme C · Charts

**C0 — Load distribution.** Left ~36 columns (x=0..34): a 35-bucket histogram of fleet CPU load (0–100%), bar height `max(1, round(share^0.62 * 7))` rows tall from the bottom (row 7 up), color `quiet` under 75th percentile bucket, `warn` 75–90%, `crit` above 90%; tip pixel brighter (×1.5). Row 8, every 7th column, a faint tick mark. Vertical divider at x=36 (dim `quiet` dashed). Right block x=38..52: 15-sample scrolling throughput ribbon, ingress bars growing up from y=4 (`warn` if v>0.85 else `azure`), egress bars growing down from y=6 (`azure`), scroll offset `floor(t*2.2) % W`.

**C1 — Live traces.** 3 stacked horizontal trace strips, each 3 rows tall, all 53 columns wide, scrolling: throughput (`y0=0..2`, `azure`, hot>0.85→`warn`), CPU (`y0=4..6`, `ok`, hot>0.8→`warn`), RTT (`y0=8..10`, `azure`, hot>0.7→`warn`). Bar height per column = `round(v*3)` rows, tip brighter. Faint separator dots every 3 columns at y=3 and y=7.

**C2 — Pool load.** Up to 7 pools, one row each starting y=2 (rows beyond y=8 dropped). Column 0 = pool status key (`crit`/`warn`/`maint`/`quiet` by worst state in pool). Columns 2..47 (46-wide) = horizontal load bar, `azure`/`warn`/`crit` by 70%/85% thresholds, filled length ∝ pool mean load. Tier-0 pools get an extra `crit` marker pixel at x=49. Row 0 = faint tick every 4th column. Label `"BY POOL"` drawn at (2,9) in `quiet`, b=0.28.

### Theme D · Instruments

**D0 — Reachability.** Full-grid waterfall, 11 rows (fixed at H=11, so one row per probe target — Turn 1/2's file names these 11 targets explicitly: `GATEWAY, DNS 1, DNS 2, TCP/443, TCP/22, NAS, VPN, MQTT, PIHOLE 1, PIHOLE 2, WAN`; Turn 3's own code drops the names but keeps the 11-row structure and the same sampling behavior), scrolling right-to-left, one new sample column every **0.42 s** (shift+push). Cell: `crit` (loss, b=0.85) / `warn` (slow, b=0.5) / `azure` (ok, brightness `0.06 + rtt*0.26`).

**D1 — Pool bars + ticker.** Up to 6 pools, rows y=0..5. Column 0-1 = status key (2px wide). Columns 3.. = proportional stacked bar (down/deg/maint/idle segments, `crit`/`warn`/`maint`/`quiet`), bar length ∝ pool size relative to largest pool (min 6px). Row 6: faint dotted line every 2 columns, then a scrolling ticker underneath/over it: `env.ticker` = `"{alert.subject} · {alert.detail}   ·   "`, color `ink`, brightness 0.48, speed 13 px/s.

**D2 — Ambient field.** Full-grid procedural noise field (3 layered sine terms), granularity `gran = 0.6 + log10(max(10,total))*1.5` (bigger fleet = finer texture), color interpolates `azure → warn` by `warmth = clamp((meanLoad-0.35)/0.5, 0,1)`. Up to 9 discrete `crit` "spark" pixels overlay for down systems (pulsing, capped at 9 — count takes over past that). Centered text overlay cycles every 5 s through 3 messages: `"{total} SYS"` → `"{meanLoad%} LOAD"` (color `warn` if warmth>0.6 else `ink`) → `"{gLabel}/S"`; drawn at y=3 with a 1px-dimmed knockout box behind it (x = tx-2..tx+tw+1, y = 2..8, dim ×0.1).

### Universal alert inlay (all 4 themes, function `inlay()`)

Fires whenever `alarming()` is true: `env.score >= 100 && !acked`. Overlays on top of whatever screen is currently active:
1. `dimAll(0.1)` — the entire running screen drops to **10% brightness**.
2. Both edge columns (**x=0 and x=52, all 11 rows**) become `crit`-colored "rails," pulsing 0.55–1.0 brightness at `abs(sin(t*2.2))` (~0.35 Hz-ish pulse).
3. Subject line (truncated to 12 chars) centered at **row y=1**, `crit`, b=0.95 — e.g. `"DNS 1 DOWN"`.
4. Detail line scrolls at **row y=6**: `env.alert.detail + "   ·   "`, `ink`, b=0.55, speed 11 px/s — carries vantages, duration, blast radius, first action (per subject; see Data requirements → alert payloads below).
5. Speaker: two-tone square-wave alarm (990 Hz / 660 Hz alternating triad, see Animation section) on the **rising edge** of the alarm condition, repeating **every 12 s** until acknowledged.
6. **Acknowledge** suppresses the inlay overlay and the tone, but the fault stays painted in the underlying running screen (i.e. ack does not clear state, only silences the interrupt). A **new** subject reaching score 100 re-arms the inlay (ack is scoped to the current alarming subject/episode, not permanent).

## Severity / tier weighting model (drives inlay + hue but NOT screen colors directly)

This is the alarm-arming logic, not a display region — but it is load-bearing for firmware and must be replicated exactly.

- 4 tiers, each pool assigned one:
  - **Tier 0** — core infra (DNS, DHCP, NTP, gateway): interrupts on **any** down/degraded.
  - **Tier 1** — production apps, storage, DMZ: interrupts on **any** down/degraded.
  - **Tier 2** — HA/DR standby: interrupts only once **20%** of the pool is down.
  - **Tier 3** — workstations, lab: interrupts only once **20%** of the pool is down.
- Score is **per-pool**, and the panel's overall score is the **worst pool's score**, not a sum/average. Exact formula (`poolScore`, from Turn 3 code):
  ```
  fd = pool.down / pool.total   // fraction down
  fg = pool.deg  / pool.total   // fraction degraded
  if tier <= 1:
      score = pool.down > 0 ? 100 + round(40*fd) : min(80, round(60*fg/0.1))
  else (tier 2/3):
      score = (fd >= 0.2 AND pool.down >= 5) ? 100 + round(40*fd)
              : min(85, round(85*fd/0.2 + 30*fg/0.4))
  ```
- **Alarm fires at score ≥ 100.** This means: tier 0/1 pools alarm the instant *any* member goes down (100+); tier 2/3 pools alarm only when ≥20% of the pool is down **and** at least 5 absolute systems are down (guards against nuisance alarms on tiny pools).
- Hue (ok/warn/crit color) reflects *condition only*; tier affects brightness/glow/pulsing intensity and the right to trigger the inlay, not the color choice itself.

## Data requirements

Per system: `state` (`ok|deg|down|maint`), `pool` (name/index), `tier` (0-3), `load` (0.0-1.0 fraction), and an animation `phase` (design-only jitter seed, not real data).

Per pool: `name`, `tier`, `total` count, `down` count, `deg` count, `maint` count.

Aggregate/derived (computed by host from raw per-system data, not sent as-is):
- `up`, `down`, `deg`, `maint`, `total` counts.
- `score` (0-100+) and `scoreWhy` (name of worst-scoring pool) — see formula above.
- `meanLoad` (mean of per-system load, 0-1).
- 35-bucket load histogram (`loadHist`), normalized 0-1 per bucket, for Theme C0.
- Aggregate throughput `gbps` and formatted `gLabel` (e.g. `"3.2G"` / `"14G"` — no decimal above 10 Gbps).
- 53-sample ingress/egress ribbons (`thru`, `egress`), each 0-1, for C0/C1/A1.
- 53-sample synthetic RTT and CPU history ribbons (`rttHist`, `cpuHist`) for C1.
- `rtt` (ms, p95) and `loss` (%) scalars for B2/waterfall summary.
- Per-pool mean load (`poolLoad`), sorted worst-first (down×9 + deg, then load) for C2/D1.
- 583-slot (or W×H-bucketed) worst-state lattice for A0, bucketed if system count exceeds slot count.
- Worst-first quantile "strip" (53 cells) for reachability-adjacent visualizations.
- Alert payload per scenario kind: `{subject, detail}` — subject ≤ ~12 chars for the inlay headline; detail is a `·`-delimited string carrying: affected scope (e.g. "CORE INFRA"), vantage count ("ALL 2 VANTAGES"), duration ("3M"), blast radius ("DHCP LEASES AT RISK · 213 SYSTEMS RESOLVING VIA DNS 2"), first action ("RESTART BIND ON MOLYBDENUM"). **This means the alert detail line requires host-side knowledge of remediation hints/runbook text, not just monitoring state** — flag as a real data requirement, not cosmetic.
- Update cadence: no explicit "data refresh interval" is specified by the design (it's a static per-scenario snapshot in the mockup, animated only visually). Recommend the host daemon push updates on state-change plus a heartbeat no slower than the fastest animation dependent on real data (throughput ribbons feel like ~1 Hz is plenty; state changes should push immediately for alarm latency).

## Animation / motion specifics

| Element | Speed / period |
|---|---|
| Global logic tick | 40 ms (25 Hz) |
| Text scroll (`scroll()` helper, all themes) | 13 px/s default; explicit 11 px/s used for B1/B2/D1 ticker lines |
| Screen auto-advance (dwell) | 3 s / 6 s / 30 s selectable; state default 6 s |
| Waterfall column shift (A? / D0) | new column every 0.42 s |
| Alarm inlay pulse (edge rails) | `0.55 + 0.45*|sin(t*2.2)|` — continuous pulse, no fixed period stated, ~2.2 rad/s angular rate |
| Alarm tone repeat | every 12 s until acknowledged |
| Alarm tone waveform | 3-note square wave triad at offsets 0 / 0.22 / 0.44 s, frequencies alternating 990 Hz / 660 Hz / 990 Hz, each note ramps 0→0.08 gain over 20 ms then decays to ~0 by 170 ms, total note length 200 ms |
| Maint dashed pulse (A0/lattice) | ~1.4 Hz on/off (`floor(t*1.4+phase)%2`) |
| Degraded pulse (A0) | ~2 rad/s sine modulation |
| D2 ambient text cycle | every 5 s, 3-message rotation |
| C0/A1 throughput ribbon scroll offset | `floor(t*2.2) % W` |

Audio requires a user gesture to unlock (`AudioContext` resume on `pointerdown`) in the web mockup — **not applicable to firmware** (the Galactic Unicorn's onboard I2S amp has no such browser restriction); flag as a mockup-only artifact, not a spec requirement.

## Typography

- Small font `F`: **3×5 px glyphs**, 4 px horizontal pitch (1 px gutter), digits 0-9, A-Z, and symbols `/ : . - % + > !` (space). Used for all labels, ticker text, inlay subject/detail.
- Large font `BIG`: **4×7 px glyphs**, 5 px horizontal pitch, digits 0-9 plus `. % G M S` and space only — used exclusively for headline numerals (UP count, throughput label, load %, mean-load %).
- `13 characters per line` is stated as "the practical text ceiling at this height" (Turn 1/2 prose) for the small font across the 53 px width — consistent with `53 / 4 ≈ 13.25`.
- No alternate/bold/italic weight — one bitmap weight per font size.
- "Watermark" technique (`textOver`): draws a dim (b≈0.12) 1px halo/knockout around a big numeral so smaller text can legibly overlay it — used in B0/B1/B2.

## Interaction (physical controls)

From the mockup's control rail (right side icons, non-authoritative UI sketch, but the 4 theme buttons + these accessory icons are explicitly modeled as physical controls):

- **Buttons A/B/C/D**: select theme (if different from current) or advance to next screen within the current theme (if pressed while already active).
- **`+` / `−`**: brightness up/down (mapped to the single global brightness scalar, 25-100% in the mockup's slider — buttons imply discrete step increments, exact step size not specified by the design; recommend 5% steps to match the mockup's slider granularity).
- **`VOL`**: speaker volume (present as an icon only; no volume levels/curve specified anywhere in the code — **ambiguity**, recommend host-side on/off + OS-level analog volume if hardware supports it, else treat as no-op / same as sound on-off toggle in code, since code only models `sound: on/off`, not a level).
- **`ZZZ`**: sleep/standby (icon only, no behavior modeled in code — **ambiguity**; recommend blanking the display / minimum brightness on press, standard "sleep" semantics, with any button waking it).
- **Acknowledge**: dedicated action (a UI button in the mockup) that silences+hides the alert inlay for the current alarm episode. On real hardware this needs to map to *some* physical input — the design does not assign it to one of the 9 physical buttons explicitly. **Ambiguity**: recommend mapping to a long-press of the currently-lit theme button, or a dedicated 5th/6th button if the Unicorn's other buttons (it has 9 total; only A-D + brightness ± + volume + sleep + reset accounted for = 8) are enumerated — see Hardware constraints, there is likely one spare button.
- Light sensor (phototransistor): present on hardware, **never referenced** in either design file. No auto-brightness-by-ambient-light behavior is specified. Treat as unused/reserved.

## FINAL DECISIONS (verbatim-faithful, binding)

From `Galactic Unicorn Themes.dc.html` (Turn 3), which is explicitly captioned as containing "final implementation decisions":

- "**Buttons choose a language, not a screen**" — A is abstract, B is text, C is charts, D is the composite instruments. Each button holds three screens that cycle on a dwell timer, so a theme is a register the panel speaks in rather than a single view. Pressing the active theme's button advances its screens by hand.
- "Severity is weighted by tier: a workstation going dark is a dim red pixel, DNS going dark takes the board and the speaker."
- Tier table (verbatim): Tier 0 · core infra — DNS, DHCP, NTP, gateway → **any** triggers. Tier 1 · production apps, storage, DMZ → **any** triggers. Tier 2 · HA and DR standby → **20%**. Tier 3 · workstations, lab → **20%**.
- "Score is the worst pool, not the sum: a tier 0 or 1 pool scores 100 the moment anything in it goes down, while tier 2 and 3 reach 100 only when a fifth of the pool is dark. The inlay fires at 100, so one DNS server interrupts at any fleet size and sixty workstations out of nine hundred do not."
- Alert inlay (verbatim): "Universal across all four themes. Both edge columns become red rails, the running screen drops to a tenth brightness behind it, the subject holds the top line, and the troubleshooting detail scrolls beneath — vantages, duration, blast radius, first action. The speaker sounds a two-tone alarm on the rising edge, repeating every twelve seconds until acknowledged." / "Acknowledge suppresses the inlay and the tone but leaves the fault painted in the running screen. A new subject reaching 100 re-arms it."
- "Grid is 53 × 11 · 583 LEDs · 3 × 5 glyphs with a 4 × 7 numeral set for headline figures." (from Turn 3's closing line, restating the earlier Turn 1/2 note that the grid is 53×11 "not 58×11.")
- From Turn 2's own recommendation (superseded in *face choice* by Turn 3's finalized 4-theme/12-screen structure, but the underlying scale-free design principle it states is carried forward verbatim into Turn 3's math): "ship 2b as the resting face" — i.e. the worst-first quantile strip + pool key + throughput + ticker instrument was the leading candidate default screen; in Turn 3 this exact concept became **Theme D, Screen 2 ("Pool bars")**, and D1 is not designated as the specific startup default in Turn 3 — see Ambiguities.
- "Audio needs one click on the page before the browser will let the speaker sound." — mockup-only constraint, not applicable to firmware (noted above).
- Scale-free design principle (binding on all future screens, not just the shipped ones): "Bar height is a fraction, never a count, so the picture holds at any scale" / severity and load are always expressed as proportions of a pool or fleet, never raw counts, so the display is correct from 30 systems to 3000 without redesign.

## Ambiguities (with recommended defaults)

1. **Default startup theme/screen.** Turn 3's own mockup initializes state to `theme: 'A', screen: 0` (State lattice) — take this as the de facto default absent an explicit statement, though Turn 2's prose recommends "2b" (⇒ Theme D screen 2, Pool bars) as "the resting face." **Recommend: default to Theme D, screen 1 (Reachability/pool-bars family)** since Turn 2's prose is the only place a preference is argued for explicitly, and Theme A's abstract lattice, while the code default, is captioned as glyph-free and hardest to read for a first-glance operator. This is a judgment call, not a spec fact — flag to the user before locking it in.
2. **`unknown` display state.** Defined color exists in Turn 1/2 (`#7C8AA0`) but no screen in Turn 3 ever emits it (`ok/deg/down/maint` are the only 4 states threaded through both files' logic). Recommend keeping the 4-state enum for now and reserving `unknown`'s color value for a future "no data from this host" case, since real telemetry will have gaps the mockup's synthetic data never had to model.
3. **Brightness ± step size and VOL/ZZZ button behavior.** Icons only, no logic modeled. Recommended defaults given above (5% brightness steps; VOL = on/off since no level model exists; ZZZ = standard sleep/blank-on-press, wake-on-any-button).
4. **Acknowledge's physical input mapping.** Not assigned to a specific one of the Unicorn's 9 physical buttons in either file. Recommend long-press on the active theme button, pending user confirmation, since it avoids consuming one of the few free buttons.
5. **Auto-brightness by day/night or ambient light sensor.** Hardware has a phototransistor; neither file uses it. Recommend leaving unimplemented for v1 (matches the design exactly) and treating it as a clearly-flagged future enhancement, not a silent gap.
6. **Data refresh/push cadence from host to panel.** Not specified — the design only specifies on-panel animation speeds, not telemetry freshness. Recommend push-on-change for state/alerts, ~1 Hz poll ceiling for load/throughput ribbons (see Data requirements).
7. **Alert detail text content (remediation hints).** The exact "first action" strings (e.g. "RESTART BIND ON MOLYBDENUM") are scenario-specific sample data in the mockup, not a schema. The firmware/daemon needs a real data source for this field (runbook lookup, static per-alert-type template, or omit entirely) — this is a host-side data-modeling decision outside the panel's own scope, flagging it up since it's implied as required content for the inlay to work as designed.

## Hardware constraints (from Pimoroni Galactic Unicorn Summary)

- MCU: Raspberry Pi **Pico 2 W**, dual Arm Cortex-M33 @ up to 150 MHz.
- RAM: 520 KB SRAM on-chip (RP2350; "double the on-chip SRAM" vs RP2350's RP2040 predecessor).
- Flash: 4 MB QSPI (XiP-capable).
- LEDs: 583 total, 53×11 grid, 6 mm pitch, driven by **FM6047 constant-current LED drivers**, 3.5 mm square-aperture LEDs.
- Refresh: measured **~300 fps at 14-bit precision** by Pimoroni — well above the design's own ~25 Hz logic tick, so no hardware refresh bottleneck is expected.
- Audio: onboard **MAX98357 3.2 W I2S mono amplifier** driving a 30 mm 1 W speaker — sufficient for the two-tone square-wave alarm the design specifies.
- Buttons: **9 tactile user buttons** + separate reset button. Design uses 4 for theme select (A-D); remaining 5 are unassigned by the design docs (brightness ±, volume, sleep icons appear in the mockup UI but aren't confirmed as mapping 1:1 to specific physical buttons — see Ambiguities #3/#4).
- Sensors: phototransistor (ambient light) — unused by design (Ambiguity #5). 2× Qwiic/STEMMA QT I2C connectors — not referenced by either design file, presumably for future sensor expansion, out of scope here.
- Power/USB: USB micro-B for power+programming; **fleet data arrives over USB serial from a UNO Q** per Turn 1's caption ("fleet data arrives over USB serial from the UNO Q") — this specifies the transport: the Linux host talks to the Galactic Unicorn over **USB serial**, not WiFi, despite the Pico 2 W having 2.4 GHz wireless. Treat WiFi as unused transport; serial is the confirmed data path.
- Board size: 330 × 78 × 10.2 mm.

---

**UNVERIFIED:**
- Whether the "UNO Q" mentioned as the serial data source in Turn 1 is a typo/placeholder for the Linux host machine itself, or a literal separate Arduino UNO Q board sitting between the Linux host and the Galactic Unicorn — the phrase appears exactly once, uncontextualized, in Turn 1's caption line and nowhere else in either file or the Pimoroni summary. This affects the C host daemon's transport target and should be confirmed with the user before implementation.
- Exact brightness ± step size and VOL level behavior (icons only, no code — see Ambiguities #3).
- Which physical button (of the Unicorn's 9) maps to Acknowledge (see Ambiguities #4).
- Whether 6 s dwell (the mockup's state default) or something else is intended as the *shipped* default — only inferred from initial state, not asserted as a decision in prose.
- Did not open the screenshot PNGs (01-04-check.png, o2a/o2b/o2c/o2c2.png, panelA.png) — the HTML+JS left no layout ambiguity requiring visual cross-check, per the task's own instruction to consult screenshots only when the HTML is ambiguous.
- `_ds/` design-system CSS/tokens directory was not read in detail — confirmed it styles only the mockup's own UI chrome (buttons, sliders, cards) around the simulated LED panel, not the panel's own pixel output, so it carries no contract-relevant content.
