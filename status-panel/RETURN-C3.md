# RETURN-C3 — Galactic Unicorn firmware

**Task:** PANEL-C3 · **Role:** Worker · **Governed by:** CONTRACT.md v1.1 (§5, §9), DESIGN-BRIEF.md

## STATUS

**COMPLETE.** Firmware implements all 12 screens, the universal alert inlay, the
alarm state machine and the USB-CDC frame link. Builds clean on lithium and
produces a reproducible `solari-panel-fw.uf2`. **Not flashed** — deploy is the
Lead's phase, as assigned.

## ARTIFACTS

All under `/home/jason/Code/Solarinet/status-panel/firmware/` (repo is the source
of truth; the build tree stays on lithium at `~/panel-build/`):

| File | Lines | Purpose |
|---|---:|---|
| `CMakeLists.txt` | 125 | pico2_w/rp2350, pinned epoch, protocol.c by path, toolchain workarounds |
| `main.c` | 345 | boot, frame RX, rotation, buttons, alarm state machine, 25 Hz tick |
| `panelHw.h` / `panelHw.cpp` | 77 / 102 | the **only** C++ TU — `extern "C"` wrapper over the Pimoroni driver |
| `panelFb.h` / `panelFb.c` | 60 / 68 | float RGB framebuffer, `set/add/dim/dimAll/flush` |
| `panelFont.h` / `panelFont.c` | 41 / 118 | 3x5 small + 4x7 BIG glyph tables, text/scroll |
| `panelHist.h` / `panelHist.c` | 106 / 273 | `PanelEnv`, snapshot apply, pool derivation, history rings |
| `panelScreens.h` | 67 | screen table types, RNG |
| `panelScreensA.c` | 203 | Theme A · Constellation (A0 lattice, A1 flow gates, A2 lanes) |
| `panelScreensB.c` | 125 | Theme B · Readout (B0 counts, B1 throughput, B2 load & latency) |
| `panelScreensC.c` | 114 | Theme C · Charts (C0 distribution, C1 traces, C2 pool load) |
| `panelScreensD.c` | 197 | Theme D · Instruments (D0 waterfall, D1 pool bars, D2 ambient) |
| `panelInlay.c` | 71 | universal alert inlay, LINK LOST, NO DATA |
| `README.md` | — | build instructions, toolchain notes, runtime behaviour |
| `pico_sdk_import.cmake`, `pimoroni_pico_import.cmake` | — | copied from lithium's SDK/pimoroni trees |

Build output on lithium: `~/panel-build/b1/solari-panel-fw.uf2` (147,968 bytes).

`../protocol.h` and `../protocol.c` were **read-only** and are compiled in
verbatim from `${CMAKE_CURRENT_LIST_DIR}/..`. Peer C2's `protocol.c` landed
before link time, so the compile-only stub path was never used.

**Lead's mid-build amendment applied.** The snapshot-apply path calls the shared
`panelSeqNewer(candidate, lastApplied)` for the duplicate/ordering guard rather
than a hand-rolled signed-difference comparison (`main.c`, `onFrame`), and the
final build linked against the latest `protocol.c` including the parser
resync-drain fix. Nothing else in the amendment conflicted with what was built:
`score` is stored as `uint16_t` and never rendered, clamped or compared against
100 anywhere in the firmware; alarm logic keys only on `alarmActive` +
`episodeId`; the maint rendering paths in B0/C2/D1 are present and simply draw
nothing while the server reports zeros.

## VERIFIED

### 1. Chip identity — verified, not assumed

Driven over the board's MicroPython REPL on `/dev/ttyACM0` (Ctrl-C to break,
query, Ctrl-D to resume — nothing written, nothing flashed):

```
_machine='Raspberry Pi Pico2 W (Galactic Unicorn) with RP2350'
```

So `PICO_BOARD=pico2_w`, `PICO_PLATFORM=rp2350`.

### 2. Build succeeds on lithium

```sh
cmake ../firmware -DPICO_SDK_PATH=$HOME/pico/pico-sdk \
                  -DPIMORONI_PICO_PATH=$HOME/pico/pimoroni-pico \
                  -DCMAKE_BUILD_TYPE=Release
make -j4
```

Configure tail:

```
PICO_SDK_PATH is /home/jason/pico/pico-sdk
Target board (PICO_BOARD) is 'pico2_w'.
Using board configuration from /home/jason/pico/pico-sdk/src/boards/include/boards/pico2_w.h
Pico Platform (PICO_PLATFORM) is 'rp2350-arm-s'.
PIMORONI_PICO_PATH is /home/jason/pico/pimoroni-pico
Build type is Release
```

Build tail:

```
[ 98%] Building C object CMakeFiles/solari-panel-fw.dir/home/jason/pico/pico-sdk/src/rp2_common/hardware_dma/dma.c.o
[ 99%] Building CXX object CMakeFiles/solari-panel-fw.dir/home/jason/pico/pimoroni-pico/libraries/galactic_unicorn/galactic_unicorn.cpp.o
[ 99%] Building CXX object CMakeFiles/solari-panel-fw.dir/home/jason/pico/pimoroni-pico/libraries/pico_synth/pico_synth.cpp.o
[100%] Linking CXX executable solari-panel-fw.elf
[100%] Built target solari-panel-fw
```

**Zero warnings** across the entire build, SDK and Pimoroni sources included:

```
$ grep -cE "warning:" ~/panel-build/b1/build.log
0
```

### 3. Reproducible — two clean builds, byte-identical

Two fresh directories, configured and built independently:

```
ef2aebc90e092a9e77d8e9b62187c95e0489e003bc1063ae83c70826dbf7e027  /home/jason/panel-build/b1/solari-panel-fw.uf2
ef2aebc90e092a9e77d8e9b62187c95e0489e003bc1063ae83c70826dbf7e027  /home/jason/panel-build/b2/solari-panel-fw.uf2
```

(This is the SHIPPING hash, after the FIX ROUND below. The pre-review build was
`78a5c64f…`; the intermediate build that added `panelSeqNewer` was `3be3b202…`.
Each of the three rounds reproduced byte-identically across two clean builds.)

`SOURCE_DATE_EPOCH` is pinned to `1785801600` in `CMakeLists.txt`, the version
string is derived from it, `__DATE__`/`__TIME__` appear nowhere, and
`-ffile-prefix-map` strips the SDK/Pimoroni/source paths.

### 4. Image identity — `picotool info`

```
File /home/jason/panel-build/b1/solari-panel-fw.uf2 family ID 'rp2350-arm-s':

Program Information
 name:          solari-panel-fw
 version:       1.0+1785801600
 description:   SolariNet fleet status panel (Galactic Unicorn)
 features:      USB stdin / stdout
 binary start:  0x10000000
 binary end:    0x10011fc0
 target chip:   RP2350
 image type:    ARM Secure
```

### 5. Design fidelity is reviewable by reading

Every screen renderer carries a header comment naming its DESIGN-BRIEF section
and the exact coordinates, thresholds, colours and timings it implements. The
screens were ported function-by-function from the Turn 3 canonical prototype
(`project/Galactic Unicorn Themes.dc.html`) rather than reimplemented from prose,
and the glyph tables were machine-generated from it, so geometry is exact.

### 6. Repo is the source of truth

`diff -r` between lithium's `~/panel-build/firmware` and the repo directory is
clean apart from `README.md`, which exists only in the repo.

## UNVERIFIED

Everything below is **untested on hardware** — nothing was flashed.

- **No pixel has ever been lit.** All 12 screens, the inlay, LINK LOST and NO
  DATA are compile-verified and read-verified only. Colour balance, legibility
  at the panel's real gamma, and scroll speed all need eyes on the board.
- **The USB-CDC link has never carried a frame.** RX path (`panelParserFeed`
  fed from `getchar_timeout_us(0)`), TX path (HELLO/EVENT/LOG), seq/duplicate
  handling and the 15 s LINK LOST timeout are unexercised end-to-end. No
  host-side unit test was written (not required by the task).
- **Alarm audio is untested.** The I2S synth is configured but has never made a
  sound; the 990/660/990 Hz triad timing and the 12 s re-alarm are unverified.
- **Buttons and the light sensor are untested.** Debounce constant (2 ticks) is
  a guess that has never met a real switch.
- **Timing headroom is unmeasured.** The 40 ms tick budget with the heaviest
  screen (D2's per-pixel triple-sine field, 583 `powf` calls) has not been
  profiled. RP2350 has a hardware FPU and 583 pixels is small, so this should be
  comfortable, but "should be" is inference, not measurement.
- **Flash/RAM headroom not analysed** beyond the image fitting (73 KB of flash).

## DEVIATIONS

Each is also marked `DEVIATION` in a code comment at the site.

**Toolchain (two workarounds, both in `CMakeLists.txt`, both explained inline):**

1. `add_link_options(-nostdlib++)`. CMake links with `arm-none-eabi-g++` because
   the target has a C++ TU, and the g++ driver appends `-lstdc++` on its own.
   lithium has `libstdc++-arm-none-eabi-dev` (headers) but not
   `-newlib` (the archive). This firmware has no business needing an STL
   runtime, so the dependency is dropped rather than installed. libm/libc/libgcc
   are unaffected. **I did not install any system package on lithium.**
2. A headers-only `hershey_fonts` INTERFACE stand-in declared before
   `galactic_unicorn.cmake`. Upstream `hershey_fonts` is an INTERFACE library, so
   its TU compiles into our target and its file-scope `std::map<std::string,…>`
   runs a global constructor at startup, pulling in the whole STL unconditionally.
   We paint through `GalacticUnicorn::set_pixel()` and never touch PicoGraphics
   or a Hershey font. `pico_graphics` proper is a static library, so unreferenced
   it is never pulled from the archive.

The named risk in the BUDGET — "if the pimoroni library fights RP2350 support" —
**did not materialise**. The galactic_unicorn driver has no RP2040-only
conditionals and compiled clean for RP2350. Both issues above are host-toolchain
packaging, not library incompatibility.

**Data-model (the mockup had scenario constants where the wire has real fields):**

3. **Throughput normalisation** (`panelHist.c`): the prototype divides by fixed
   25 Gbps / 18 Gbps WAN ceilings. Replaced with an adaptive decaying peak
   (floor 1 Mbps, decay 0.997/sample) so the ribbons scale to whatever this
   fleet actually pushes.
4. **Rate label** (`panelHist.c`): an M (megabit) tier added below 1 Gbps, for
   the same reason.
5. **A2 lanes** (`panelScreensA.c`): the mockup's four lanes are MQ/SNMP backlog
   scenario constants. Nothing on the wire carries those, so the lanes are
   re-sourced to loss, dataStale, unknown-share and degraded-share. Geometry,
   rates and colours unchanged.
6. **D0 waterfall rows** (`panelScreensD.c`, `panelHist.c`): the mockup's 11 rows
   are named probe targets with per-row synthetic loss. protocol.h carries only
   fleet-aggregate `rttTenthMs`/`lossPermille`, so each row tracks one of the 11
   worst-first systems and samples from that system's state plus the aggregate
   RTT/loss. Cadence (0.42 s), geometry and colours unchanged.
7. **C0/C1 indexing** (`panelScreensC.c`): the prototype scrolls a *static*
   waveform with a synthetic animation offset because its data never changed.
   The rings here genuinely advance one sample per snapshot, so traces are
   indexed chronologically (oldest left, newest right) and the offset is dropped.
   Keeping it would scroll real history past itself twice and read as noise.
8. **UNKNOWN state** (DESIGN-BRIEF Ambiguity #2): the mockup never emits it. It
   renders in `cUnknown` (#7C8AA0) and ranks between degraded and maint in
   worst-first ordering.
9. **B0 tier-0-down flag** (`panelScreensB.c`): sourced from full-fleet pool
   aggregates rather than the systems array, which the wire caps at 64.
10. **B2 "over 85"** counts only over the ≤64 systems the wire carries — the
    snapshot has no fleet-wide over-threshold count.
11. **`'·'` separators** replaced by pixel-identical ASCII space runs (the 3x5
    font is ASCII-only; the glyph was a mid-dot the font does not have).
12. **LINK LOST and NO DATA screens are invented** — DESIGN-BRIEF specifies no
    treatment for either. LINK LOST is deliberately amber and rail-free, *not*
    the alarm's red-rail vocabulary: a dead USB cable is a panel fault, not a
    fleet fault, and confusing the two would train the operator to ignore red.

## FIX ROUND (cross-lab review: FIX-THEN-SHIP, 3 findings)

All three fixed. Rebuilt on lithium with the latest `protocol.c`
(md5 `5754c9312bc7c2a01609355a17b64b3c`, including the resync-drain fix and
`panelSeqNewer`).

### 1. MUST-FIX — `panelScreensA.c` A1 WAN ramps (accepted, real bug)

The ramps used each direction's **share** of the current total,
`tx/(rx+tx)` and `rx/(rx+tx)`. That is a ratio, not a magnitude: a 1 Kbps
one-way flow painted a full-scale ramp, and a busy symmetric link painted both
ramps half-lit. The reviewer is right, and the fix is the normalisation I had
already built for the rings and B1 — the ramps now read the newest sample of the
already-normalised `egress` / `thru` rings:

```c
float up = panelHistAt(env, env->egress, PANEL_HIST_LEN - 1);
float dn = panelHistAt(env, env->thru,   PANEL_HIST_LEN - 1);
```

A1, B1 and the C0 ribbons now all mean the same thing by "full". The dead
`busy` gate is gone (a normalised zero is already zero), and the comment records
what the old code got wrong so it does not come back.

### 2. SHOULD — `panelScreensD.c` D1 pool ordering (accepted)

D1 iterated `env->pools[i]` in wire order while C2 iterated worst-first. With
only 6 of up to 8 rows visible, wire order can push the one pool that is
actually down off the bottom of the panel. Now indexes through
`env->poolWorstFirst[i]`, matching C2.

### 3. SHOULD — `panelScreensD.c` D1 unknown handling (accepted)

Two defects, both fixed:

- **Key order.** `maint` was tested before `unknown`, so a pool with both
  painted maint purple. Corrected to down > degraded > unknown > maint, which is
  what my own DEVIATION #8 declared and what `stateRank()` in `panelHist.c`
  already implemented.
- **Missing segment.** The stacked bar had no unknown band at all — unreachable
  hosts rendered as idle, which is the most misleading possible reading. Added
  `cUnknown` at b=0.42 between the degraded and maint bands, with the same
  "at least one lit pixel if non-zero" rule as the others.

### Extra: same defect found in C2 (not flagged by the reviewer)

`panelScreensC.c:97` had the identical `maint`-before-`unknown` key ordering.
Fixing D1 alone would have left the two pool screens colouring the same pool
differently, so I fixed C2's key order too. This is beyond the three findings —
flagging it rather than burying it.

### Rebuild after fixes

Two clean builds, zero warnings, byte-identical:

```
ef2aebc90e092a9e77d8e9b62187c95e0489e003bc1063ae83c70826dbf7e027  b1/solari-panel-fw.uf2
ef2aebc90e092a9e77d8e9b62187c95e0489e003bc1063ae83c70826dbf7e027  b2/solari-panel-fw.uf2
```

None of these fixes is hardware-verified — they are corrections to render logic
that has still never lit a pixel. Everything under UNVERIFIED still stands.

## P4 ROUND (two populations, CONTRACT §9b)

Audit result: **no screen renders a stateRoll figure and a systems/pools figure
side by side as one contradictory total.** B0's printed integers are all
stateRoll (its TIER 0/TIER 3 flag is pool-derived but gated on `env->down > 0`,
so it qualifies a stateRoll number rather than competing with it); B1 prints no
population figure; D0 prints no numbers; D1 draws bars with no absolute counts;
D2's "{n} SYS" and spark count are both stateRoll. A0/A1/A2 and C0/C1/C2 print
no population totals either.

**One residual accepted for fixing:** B2's hot-host count is roster-derived and
could read "12" while B0 and D2 say 9 one rotation step away. The firmware
cannot separate the populations itself — `PanelSystem` carries no agent-vs-probe
flag. Per the Lead's instruction the count now prints its denominator.

### Layout defect found while implementing (pre-existing, now fixed)

The literal instruction — `"3/15 OVER 85"` — **does not fit and never did.**
At the design's `x0 = max(26, bigW+4)` on a 53 px panel, using the shipped 4 px
text pitch:

```
'3 OVER 85'      w= 35  x0=26 -> ends at  60  CLIPPED   <- what shipped until now
'3/15 OVER 85'   w= 47  x0=26 -> ends at  72  CLIPPED
'3/15>85'        w= 27  x0=26 -> ends at  52  FITS
```

So B2 row 6 has been rendering the truncated string **"3 OVER"** — the
threshold was already invisible. This is inherited from DESIGN-BRIEF §B2 and
from the Turn 3 prototype (`Themes.dc.html:543`), which has the identical
`hot + ' OVER 85'` at the identical `x0`. Adding a denominator would have made
it strictly worse.

**DEVIATION 13.** The label contracts from the word `"OVER 85"` to `">85"`,
which carries the same threshold in 3 characters (both `/` and `>` are in the
shipped 3x5 glyph set). The row is also right-aligned to the panel edge and
floored just clear of the BIG numeral, so it sits at the design's x=26 in the
common case and shifts left instead of clipping once the counts reach two
digits (worst case `"64/64>85"`, 31 px, lands at x=22).

### Row y=0 of the same screen (Lead authorised, fixed in the freeze build)

Worse than a clip: the design's row 0 is **over-subscribed**. It asks for
`"MEAN LOAD"` (35 px) at x=1 *plus* `"P95 {rtt}MS"` (31 px) *plus* a loss
percent (15 px) starting at `x0 = max(26, bigW+4)` — roughly 84 px of text on a
53 px row. The Turn 3 prototype has the same arrangement. In practice `"P95 …"`
overpainted the tail of `"MEAN LOAD"` and then ran off the right edge itself,
so **both** strings were rendering truncated.

**DEVIATION 14.** Three changes make it fit with nothing clipped and no figure
dropped:

- the big numeral's label contracts to `"LOAD"` (15 px, x=1..15) — with a
  percent numeral directly beneath it, "MEAN" carried no information;
- latency and loss **alternate on a 5 s slot** rather than sharing the row, the
  same rotation vocabulary D2 already uses. This preserves both *labels* intact
  instead of abbreviating them to symbols, which matters more here than
  simultaneity: `"P95"` distinguishes a percentile from a mean, and dropping it
  would silently change what the number means;
- the active reading is right-aligned to the panel edge and floored clear of the
  `"LOAD"` label, so it can neither overlap nor overrun.

Both figures are clamped to the widest form that fits. Verified across the
range:

```
'P95 12MS'   w= 31 x= 22 ends= 52 FITS
'P95 999MS'  w= 35 x= 18 ends= 52 FITS
'LOSS 0.3%'  w= 35 x= 18 ends= 52 FITS
'LOSS 100%'  w= 35 x= 18 ends= 52 FITS
LOAD occupies 1..15,  floor = 18
```

Colours, thresholds, brightnesses and the BIG numeral are exactly as specified;
only the text layout moved.

**Accepted as-is, no action:** D2's spark *glow tier* reads from the first DOWN
entry in `systems[]` (roster) while the spark *count* is stateRoll. Affects one
brightness multiplier — no count, no colour.

### Rebuild

```
93816f1979922cd9a21362beee1f5a4571942aa2ba576304a3028d9fcbbb0293  b1/solari-panel-fw.uf2
93816f1979922cd9a21362beee1f5a4571942aa2ba576304a3028d9fcbbb0293  b2/solari-panel-fw.uf2
```

Zero warnings. `protocol.c` md5 `5754c9312bc7c2a01609355a17b64b3c` (the
resync-drain + `panelSeqNewer` revision). This is the image the Lead flashed.

## ALARM AMENDMENT ROUND (operator-directed, 2026-08-04)

Operator-directed change relayed by the Lead, **superseding the DESIGN-BRIEF's
12 s re-alarm spec**. Recorded here as an amendment, not a deviation: the brief
is wrong on this point by decision, not by my reading of it. Alarm machine only;
ack semantics, inlay, and episode re-arm are untouched.

| # | Change | Where |
|---|---|---|
| 1 | Re-alarm tone interval 12 s → **60 s**. Rising-edge tone and the 3-note triad (990/660/990 Hz at 0 / 0.22 / 0.44 s) unchanged. | `main.c` `REALARM_SEC` |
| 2 | **Auto-silence at 5 min** unacked from the rising edge: tone stops permanently for that episode. Explicitly *not* an ack — inlay stays, beacon stays, a new `episodeId` re-arms sound normally. | `main.c` `AUTOSILENCE_SEC`, `runAlarm()` |
| 3 | **Unacked beacon**: x=51,52 × y=0,1 flash crit red at 1 Hz (50% duty, b=1.0) from the rising edge until ack, independent of tone state. | `panelInlay.c` `panelBeacon()` |

Auto-silence is one-way within an episode. It is cleared in exactly three
places, all of which end the episode anyway: the ack path in `handlePress()`,
the alarm-cleared branch of `runAlarm()`, and the rising edge of a new
`episodeId`. There is no path that re-sounds a silenced episode.

**Beacon render order and sleep visibility — the code paths:**

- `main.c` — normal branch: screen → `panelInlay()` → `panelBeacon()`. The
  beacon is painted **after** the inlay because the inlay's right-hand rail
  runs down x=52 and would otherwise mask two of the four beacon pixels.
  `panelBeacon()` is called outside the branch, so it also covers the zero-node
  NO DATA case, which draws no inlay.
- `main.c` — sleep branch: `panelFbClear()` → `if (gAlarmArmed) panelBeacon(gT)`
  → `panelFbFlush()`. This is the cited path: the ZZZ branch previously did a
  bare clear-and-flush and returned; it now paints the beacon on the otherwise
  dark panel before flushing.
- Dimming: `panelBeacon()` writes at framebuffer brightness 1.0, and the LED
  driver's global brightness **floors at 0.25** (`panelHw.cpp:70`, clamp in
  `panelHwSetBrightness`). The beacon can be dimmed down but not out, and at
  any setting it is the brightest thing on a sleeping panel.

**DEVIATION 15 (required by change 2+3).** `runAlarm()` used to re-assert
`gSleeping = false` on **every tick** while armed, which pinned the panel awake
for the entire life of an unacknowledged episode. That is incompatible with the
amendment: after auto-silence the intended overnight state is a quiet, sleeping
panel carrying nothing but the beacon. The wake is now a **one-shot at the
rising edge**. Ack semantics are unaffected (any button during an alarm is still
consumed as ack, so the operator still cannot reach ZZZ mid-alarm without
acking).

### Rebuild

```
FIXME_ALARM_SHA  b1/solari-panel-fw.uf2
FIXME_ALARM_SHA  b2/solari-panel-fw.uf2
```

Also fixed in this round, unrequested: `panelScreensB.c` had picked up one
`-Wformat-truncation` warning from the DEVIATION 14 rewrite (gcc cannot see the
999 ms clamp). Buffer widened 16 → 24 bytes; back to a zero-warning build.

## NEXT

For the Lead's deploy phase:

1. Flash `~/panel-build/b1/solari-panel-fw.uf2` (BOOTSEL → RPI-RP2 → copy).
   Note this **replaces the MicroPython runtime** currently on the board.
2. Bring up `solari-paneld` against it and confirm a SNAPSHOT round-trip: panel
   leaves NO DATA, HELLO comes back, a button press produces `EV_BUTTON`.
3. Eyes-on pass over all 12 screens for legibility and colour at real gamma —
   the single highest-value check, since none of it has been seen.
4. Pull the cable for >15 s to confirm LINK LOST + `EV_LINKLOST`, then restore
   for `EV_LINKBACK`.
5. Drive a synthetic `alarmActive` episode to check the triad, the **60 s**
   re-alarm and per-episode ack. Budget 5 minutes of wall clock if you want to
   see auto-silence fire, and leave it unacked to watch the beacon outlive the
   tone; then press ZZZ *after* acking to confirm the sleep path separately.
6. If the tick budget is tight on screen D2, the per-pixel `powf` is the first
   thing to precompute into a lookup table.
