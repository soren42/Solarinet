# solari-panel-fw

Firmware for the SolariNet status panel: a Pimoroni **Galactic Unicorn**
(53x11 RGB matrix, Raspberry Pi **Pico 2 W / RP2350**) that renders the fleet
state pushed to it over USB-CDC by the `solari-paneld` daemon.

Governed by `../CONTRACT.md` v1.1 (§5 firmware behaviour, §9 amendments) and
`../DESIGN-BRIEF.md` (binding visual spec). `../protocol.h` / `../protocol.c`
are the shared wire codec and are consumed verbatim — never edited here.

## Board

The target is **verified, not assumed**. The attached unit's MicroPython REPL
reports:

```
Raspberry Pi Pico2 W (Galactic Unicorn) with RP2350
```

so `CMakeLists.txt` pins `PICO_BOARD=pico2_w` / `PICO_PLATFORM=rp2350`. On an
RP2040 Galactic Unicorn those two lines become `pico_w` / `rp2040`; nothing
else in the tree is chip-specific.

## Building

Build host is **lithium** (aarch64 Debian 13): pico-sdk 2.1.1,
arm-none-eabi-gcc 14.2.1, cmake 3.31.6. The repo is the source of truth; the
build directory lives on lithium.

```sh
cmake -S firmware -B build \
  -DPICO_SDK_PATH=$HOME/pico/pico-sdk \
  -DPIMORONI_PICO_PATH=$HOME/pico/pimoroni-pico \
  -DCMAKE_BUILD_TYPE=Release
make -C build -j4
# -> build/solari-panel-fw.uf2
```

### Two toolchain notes

Debian ships the arm-none-eabi C++ *headers* but not the libstdc++ runtime
archive, and this firmware must not need one. Two lines in `CMakeLists.txt`
keep it that way:

- `add_link_options(-nostdlib++)` — CMake drives the link with `g++` (the
  target has one C++ TU), and the `g++` driver appends `-lstdc++` below CMake's
  link line. This drops only libstdc++; libm/libc/libgcc are untouched.
- a headers-only stand-in `hershey_fonts` INTERFACE target declared *before*
  `galactic_unicorn.cmake`. Upstream's `hershey_fonts` is an INTERFACE library,
  so its TU compiles straight into this target and its file-scope
  `std::map<std::string, ...>` runs a global constructor at startup, dragging
  in the whole STL. This firmware paints through `set_pixel()` and never
  touches PicoGraphics or a Hershey font. `pico_graphics` itself is a static
  library, so unreferenced it is never pulled from the archive.

If a future change genuinely starts using the STL, the link fails loudly —
that is the intended outcome, and the fix is then a deliberate
`libstdc++-arm-none-eabi-newlib` dependency, not a workaround.

### Reproducibility

`SOLARI_SOURCE_DATE_EPOCH` is pinned in `CMakeLists.txt` and the version string
is derived from it; `__DATE__`/`__TIME__` appear nowhere. `-ffile-prefix-map`
strips the SDK, Pimoroni and source paths out of the image. Two clean builds in
different directories produce byte-identical UF2s.

The image carries its identity for `picotool info`:

```
program name:     solari-panel-fw
version:          1.0+<pinned epoch>
```

## Layout

| File | Purpose |
|---|---|
| `main.c` | boot, frame RX, screen rotation, buttons, alarm state machine, 25 Hz tick |
| `panelHw.h/.cpp` | the **only** C++ TU — thin `extern "C"` wrapper over the Pimoroni driver (matrix, buttons, light sensor, I2S synth) |
| `panelFb.h/.c` | float RGB framebuffer and the `set/add/dim/dimAll` primitives |
| `panelFont.h/.c` | 3x5 small and 4x7 BIG bitmap glyph tables + text/scroll drawing |
| `panelHist.h/.c` | `PanelEnv`: applies a snapshot, derives counts/pools, accumulates the history rings |
| `panelScreensA/B/C/D.c` | the 12 screens, one file per theme |
| `panelInlay.c` | universal alert inlay, LINK LOST, NO DATA |

Every renderer's header comment cites the DESIGN-BRIEF section and the exact
coordinates it implements, and any place the hardware data model forced a
departure from the mockup is marked `DEVIATION` in that comment. Those are
collected in `../RETURN-C3.md`.

## Runtime behaviour

- **Tick** 40 ms (25 Hz). Screens are pure functions of `PanelEnv` + `t`/`dt`.
- **History is local.** Per CONTRACT §9 the wire carries no ribbons,
  histograms or sparklines; the rings advance one sample per *snapshot
  arrival*, not per tick.
- **Score and `alarmActive` are the server's.** The firmware never recomputes
  either.
- **Rotation** dwells 6 s per screen. Buttons A–D pick the theme, and pressing
  the current theme's button advances within it.
- **Brightness** LUX+/LUX− step 0.05 and reach 1.00; light-sensor auto-brightness
  runs when enabled, and either LUX button latches manual control. Auto maps the
  raw reading against `PANEL_LUX_FULL` (main.c), *not* the ADC's 4095 ceiling —
  the panel lives in direct daylight, so full brightness has to be reachable well
  below a pegged sensor. `PANEL_LUX_FULL` is the knob if it looks held back.
  Response is asymmetric: brightens in ~0.5 s, dims over ~4 s. ZZZ blanks the panel;
  any button wakes it, as does the rising edge of an alarm.
- **Text legibility** — the unlit LED packages are pale cream and reflect ambient
  room light at roughly the luminance of a mid-duty lit pixel, so there is a
  reflection floor under which dim ink of any colour is invisible. Two rules
  follow, and both are load-bearing:
  1. All small-font text is mapped into a narrow high band, `0.85 + 0.15*b`,
     applied once in `panelFont.c` (`panelTextInk`, called from `drawSmall`, the
     seam every small-font path funnels through). Text pixels are few and bright,
     never many and dim.
  2. Text hierarchy is carried by **hue**, not brightness: white (`cInk`) for
     primary readouts, azure (`cAzure`) for secondary labels and captions, and
     size/position for anything below that. `cQuiet` is never used for
     small-font text — it is the ink that dies against the reflection.

  `panelBig()` watermarks and all non-text primitives are excluded from the seam
  and keep the design's own brightnesses; they are area elements, legible by mass.
- **Seq resync** — snapshots older than or equal to the last applied seq are
  discarded per protocol.h. If that state persists for 30 s while snapshots keep
  arriving, the sender is assumed to have restarted (its seq counter resets to 1)
  and the next snapshot is adopted unconditionally, with a `LOG` frame recording
  it. Without this a sender restart wedges the panel for ~18 hours while it looks
  perfectly healthy — stale data, valid frames, no link fault.
- **Link loss** — no valid frame for 15 s raises the LINK LOST screen and emits
  `EV_LINKLOST` (0x04); recovery emits `EV_LINKBACK` (0x05). Deliberately amber,
  not red: the alarm's red-rail vocabulary stays reserved for fleet faults.

  Both the liveness timer and the seq/resync rules live in **`panelLink.c`**, a
  hardware-free translation unit, and are exercised by **`test/panelLinkTest.c`**
  on the build host (`make -C firmware/test`). They were pulled out of the render
  loop after build `dbd33885` flapped the link once per snapshot on hardware: the
  tick sampled the clock *before* pumping serial, so a frame parsed during that
  tick carried a timestamp a millisecond *ahead* of it, and the unsigned
  subtraction turned "1 ms in the future" into 4294967295 ms of apparent age —
  instantly past the 15 s timeout. A ~110-byte snapshot always crosses a
  millisecond boundary during its byte-at-a-time read; an 8-byte ping usually
  does not, which is why the flap tracked the snapshot cadence exactly. The
  comparison is now signed (also correct across the 49.7-day `uint32` ms wrap)
  and the poll is fed a post-pump timestamp. **Do not reimplement this arithmetic
  in the tick.**
- **Alarm** — a two-tone triad (990/660/990 Hz, 200 ms notes at 0/0.22/0.44 s)
  repeating every **60 s** while `alarmActive`. Acknowledge is **firmware-local
  and per-episode**: any button press during an armed alarm acks it and is
  consumed; a new `episodeId` re-arms.
  - **Auto-silence** — an episode unacknowledged for 5 minutes stops re-sounding
    for good. This is not an ack: the inlay and the beacon both stay up.
  - **Beacon** — x=51,52 × y=0,1 flash red at 1 Hz from the rising edge until
    ack, on top of every screen and the inlay, and on the blank panel while
    asleep. Independent of tone state, so it survives auto-silence.
  - The 60 s interval, the auto-silence and the beacon are an operator
    amendment (2026-08-04) superseding the DESIGN-BRIEF's 12 s alarm spec.

## Flashing

Not done here. Hold BOOTSEL, mount the RPI-RP2 volume, copy
`solari-panel-fw.uf2`. Deploy is the Lead's phase.
