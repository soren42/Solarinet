# RETURN — SOLNET-AW3 · dashboard lane

`worker: claude (opus) · task: SOLNET-AW3 · governed by CONTRACT-AW.md §3, §7, §8, §9 A-1/A-2, §10 D2/D3/A-3`
`status: COMPLETE (lane) / BLOCKED (end-to-end, on AW1) — no git operations, no deploys, no service restarts, no DB writes`

## Files authored

| Path | Change |
|---|---|
| `dashboard/public/screens-panel.jsx` | A1 rewritten as the §9 A-1 flow-gate renderer; §3.1 gear decoding; §3.2 kinds 8/9 with per-screen pend keying; §3.3 config decoding; per-tile enable + weight controls. |
| `dashboard/public/styles.css` | New classes only — `.vp-tile--skipped`, `.vp-tile__badge--off/--wt`, `.vp-cfg*`, `.chip--mini`, plus their mobile overrides. No existing rule altered. |
| `status-panel/fixtures/panel-snapshot.json` | v2. `gear` arrays for every scenario from the real 9-device inventory; `panelScreenConfig` in a different accepted wire shape per scenario. 7583 → 9691 lines. |
| `tests/dashboard/test_panel_aw.js` | New. 150 assertions in 8 blocks. |

Out-scope files confirmed untouched: `dashboard/api/routes/panel.php`, `dashboard/public/api.jsx`, `status-panel/protocol.{h,c}`, `status-panel/firmware/*`. No mapper field forced an `api.jsx` change, so no grant was needed.

## What A1 now draws

Gear rows arrive as `[{role,state,rxLevel,txLevel}]` in §3.1 order and are grouped into four bands laid out left to right in traffic order — APs → hubs → switch → router → internet. Gate height encodes tier per §9 A-1: router 11 rows (full height), switch 8 centred, hubs 6-top / 5-bottom alternating, APs 3 distributed down the height. Legs connect consecutive non-empty bands, so a partial inventory still renders a connected path rather than floating columns. Particles ride the legs at a rate set by `txLevel`; **level 0 draws nothing at all**, per the contract's "0 = idle". A `down` gate keeps its full extent but is drawn with gaps — severed, not merely red — and per §10 A-3 the internet marker takes the router's state while wanBackup keeps its own. `gearCount == 0` falls through to the existing NO DATA treatment.

`a1Layout(gear)` is a pure function — gear rows in, `{bands, gates, legs, wan, router}` out — specifically so the geometry can be asserted without a canvas. The harness uses both it and the resulting framebuffer.

## Controls and the STATE-authority doctrine

Each of the 12 tiles carries a Rotation On/Off pair and six Dwell chips (¼× ½× 1× 2× 5× 10×). Toggling posts `kind 8, arg (screenIdx<<1)|enabled`; weight posts `kind 9, arg (screenIdx<<3)|weightCode`. The two shifts differ and the harness pins both against hand-computed values *and* against what the rendered buttons actually send.

The part that matters most, and the part that is easiest to get quietly wrong: **a control shows a setting as in force only once the panel has reported it.** With no `panelScreenConfig` in the payload, nothing is selected, no tile is marked skipped, no weight is badged, and the tile says so in words. The §3.4 factory default is never rendered as though it were a report. Absent, null, wrong-length, malformed and non-numeric configs all decode to `null` (unknown); a single garbled *entry* is unknown on its own rather than discarding eleven entries the panel did report.

`send()` and its give-up ladder are unchanged from CP. The one change was necessary: `CMD_GROUP` keys one pending slot per *kind*, which would have made toggling A1 paint B2 as pending. `cmdGroup(kind, arg)` now derives `screenEn:<idx>` / `screenWt:<idx>` for kinds 8/9 and returns the original key for every CP-era kind. Contained entirely within `screens-panel.jsx`.

## Parity: §9 A-1 is the oracle, and it settled a wrong turn I took

AW2's firmware landed mid-task, so I read `panelScreensA.c` and diffed it against my renderer rather than leave acceptance A1 open. We had independently converged on nearly everything: band columns 2–16 / 20–34 / 38–44, router x=47 h=11, internet x=51, wanBackup x=52, switch y0=1 h=8, hubs alternating h=6 flush-top / h=5 flush-bottom, the colour map, the broken-when-down every-other-pixel rule, and RNG seed 77. The two x-spread formulas were written independently — the firmware in biased integer arithmetic, mine in floats — and agree on every band at every count 1 to 12.

Three points differed, and **I conformed to the firmware on all three. That was the wrong call, and §9 A-1 and §10 A-3 have since reversed two of them.** Treating the other implementation as the authority is precisely the mistake the contract exists to prevent; when two lanes disagree the answer is the spec or an escalation, not whoever committed first. Current state:

| Point | I originally had | I wrongly conformed to | Ratified | Now |
|---|---|---|---|---|
| AP rows | distributed 0/4/8 | firmware's fixed y0=4 | **§9 A-1: distributed**, `y0 = round(i*(11-3)/(n-1))`, 4 when n==1 | reverted to distributed |
| Internet column | router's state treatment | firmware's always-healthy marker | **§10 A-3: router's state treatment** | reverted; A-3 implemented |
| wanBackup extent | rows 2–8 | firmware's y0=3 h=5 | unspecified in A-1 | kept y0=3 h=5 — both sides agree, no contract conflict |

§10 A-3 also settles the design question I raised: the internet marker takes the router's state (warn when degraded, crit and broken when down, never healthy over a dead gateway), and wanBackup keeps its own device state so a router outage leaves it visibly the only live path. Implemented and pinned in both states, plus the degraded case and the wanBackup-survives case.

I kept the extracted `a1Gate(x, y0, h, state, brightness)` helper — same name and signature as the firmware's — so the two remain diffable by eye. It also drops an endcap brightness boost the contract does not describe.

**AW2's committed firmware currently violates the ratified contract in two places** (`panelScreensA.c:143` fixes APs at y0=4; `:153` holds the internet healthy regardless of the router). Both are AW2's to land per A-1 and A-3's "firmware implements first". Flagged to Lead. My harness asserts §9 A-1 and cross-checks the firmware only where it already agrees, so it will not go green on a firmware that is still diverged.

## §10 dispositions absorbed

- **D2 (screenIdx = theme\*3 + slot, NORMATIVE).** The page already used this mapping for `currentIdx`; it is now asserted explicitly — every one of the 12 theme/slot pairs round-trips through both packings, and `SCREENS` is pinned to that exact order, so a reordering of the array breaks loudly instead of addressing the wrong screen.
- **D3 (reserved bits).** `screenCfg` bits 4–7 are masked and ignored rather than treated as malformed — a sender that sets one must not blank the tile. Reserved `weightCode` values 6–7 are a different case and still read as unknown for that entry: the field is in range but carries no weight we can honestly render, and clamping it to a real weight would claim something the panel never said.
- **D3 (`cfgPersisted`).** New `readCfgPersisted()` and a "Saved to panel flash" chip, shown **only** on a positive report of flags bit0. A missing or clear bit renders as silence, not as an "unsaved" warning: D4 explicitly permits persistence to ship deferred and session-volatile, and a standing warning about a feature that may never arrive is noise.
- **D1, D4, D5** are firmware/protocol-side and change nothing in this lane. D5's dwell rescale is panel-side timing; the page reports weights, it does not simulate the rotation clock.

## Alignment with AW1

Read after the fact, since AW1's packet landed mid-task. `solariPanel.c postConfig()` POSTs `{screenCfg:[{enabled:0|1,weightCode:0..7}×12], flags:N}` to `/api/panel/config`, into migration 019's `panelScreenConfig.screenCfg` JSON column, using §3.3's bit layout exactly. **No mismatch to flag.** My decoder already accepted that shape, including `enabled` arriving as a cJSON number rather than a boolean, and it decodes byte-identically to the packed form. Harness block [6] pins it so a future change to either side breaks loudly. Migration 019's widened `networkGear.kind` enum also maps cleanly — `gateway` and `router` both reach the router band; `other` is dropped rather than filed into an arbitrary one.

## Commands exercised

```
$ node tests/dashboard/test_jsx_parse.js
15/15 parsed

$ node tests/dashboard/test_lifecycle_ui.js
80 passed, 0 failed          # unchanged by this lane

$ node tests/dashboard/test_panel_aw.js
[1] A1 flow-gate geometry — CONTRACT-AW §1 heights, §4 stagger and internet
[2] A1 state treatments — down gates, no-gear fallback, partial inventories
[3] every fixture scenario renders every screen
[4] §3.2 command packing, asserted from the rendered controls
[5] §3.3 panelScreenConfig — absent reads as UNKNOWN, never as a default
[6] alignment with the shapes AW1 actually emits
[7] conformance to §9 A-1, the parity oracle (acceptance A1)
[8] the fixture file
150 passed, 0 failed

$ node tests/dashboard/test_layout.js
all assertions passed          # unchanged by this lane
```

Block [3] paints all 5 scenarios × 12 screens for 75 frames each and asserts no empty and no negative framebuffers — the §7 acceptance gate. Block [1] asserts the geometry on the framebuffer the renderer actually produced, using a level-0 inventory so no particles are drawn and every lit pixel above threshold is structural.

## UNVERIFIED

1. **No live browser.** Nothing here has been rendered by a real canvas at real pixel sizes. The harness proves the framebuffer contents; it does not prove the tiles *look* right, and it cannot catch a CSS regression. Layout, contrast and the new `.chip--mini` hit targets need one pass of human eyes.
2. **No real panel confirm loop.** Kinds 8/9 have never made a round trip. The harness proves what the UI *sends* and what it does with a config it is *given*; that a real panel accepts kind 9 and reports the change back is untested by anyone in any lane.
3. **Parity is asserted structurally, not by pixel diff.** Block [7] compares my geometry against constants and a formula I transcribed by hand from `panelScreensA.c`, and it will not notice if AW2 later edits that file. Nobody has yet rendered the same gear bytes through both renderers and diffed the two framebuffers, which is what acceptance A1 actually asks for. That needs a fixture the C test harness can also read, and it crosses two lanes — I did not build it. Particle motion in particular is only "same seed, same count", not proven identical: the two integrators differ in shape and I made no attempt to reconcile them.
4. **End-to-end is still blocked on AW1's read path — the write path landed, the read path did not.** AW1 re-ran `panel.php` after RETURN-AW1 was written (+82 lines). Now present: kinds 8/9 validation on `POST /api/panel/command`, the `POST /api/panel/config` endpoint, a `panelGearRole()` mapper whose role assignments match mine exactly (including dropping `'other'`), and a Kbps→0..7 log-scale level helper. **Still absent: the `GET /api/panel` payload emits neither `gear` nor `panelScreenConfig`.** `panelGearRole()` and the level helper are currently dead code — defined, never called. Consequence deployed as-is: A1 renders NO DATA and every tile reads "the panel has not reported", which is correct behaviour on an empty payload but will look broken to Jason. **This is a small, well-defined piece of work: call the two existing helpers and add two keys to the payload array.** My side is tested against exactly those two keys and should light up the moment they appear.
5. **The fixture's gear data is synthetic.** Device roles and the 9-device inventory are real (CONTRACT-AW §1); the rx/tx levels are deterministic jitter I generated, not observed UniFi counters. Real counter-derived levels may cluster differently and make the particle density read wrong.
6. **`buildEnv` gear decoding is unexercised against a real payload.** It is tested against the fixture and against hand-built rows only.
7. **Not deployed.** Nothing copied to `/var/www`; Lead deploys.

## Lead risks / next action

1. Decide the internet-column question above (lit vs. dark when the router is down). It is the one open A1 design call, and it belongs in the firmware first.
2. **Send AW1 back for the read path.** `panel.php` needs `gear` and `panelScreenConfig` added to the `GET /api/panel` payload; the helpers to build both are already sitting in the file unused. This is the single thing standing between a complete lane and a working feature. Until then the panel page is correct but dark.
3. Give the panel page one human look in a browser at `/?fixture=1` before it reaches Jason.
