# RETURN — SOLNET-AW2 · protocol + firmware

`status: complete — closed out by Lead (fable-5); wrapper lost post-codex
(silent ~4 h), work salvaged from disk and finished by Lead`

## ARTIFACTS

status-panel/protocol.h            (kinds 8/9, PANEL_FT_CONFIG 0x85, gear ext)
status-panel/protocol.c            (v1-additive gear codec, CONFIG codec, knownType 0x85)
status-panel/firmware/panelScreensA.c   (A1 flow-gates renderer, §9 A-1 geometry)
status-panel/firmware/panelScreenCfg.c/.h (config store + flash persistence)
status-panel/firmware/main.c       (rotation weights + dwell rescale, CONFIG emit)
status-panel/firmware/panelCtl.c/.h (kinds 8/9 consume)
status-panel/firmware/test/*       (suite extended to 142 cases)
status-panel/daemon/tests/codec_test.c (shared codec cases)

## VERIFIED (Lead, on disk)

- §10 D1: PANEL_PROTO_VERSION back to 0x01; gear section is a v1-additive
  trailing extension. Host tests: "v1 additive nine-gear snapshot round
  trip", "legacy payload with no gear section decodes as no gear",
  "oversized additive gear count decodes as no gear" — all pass.
- §10 D5: dwell rescale implemented + tested ("20/30*7.5 = 5 seconds, no
  immediate advance"; "rescaled elapsed at new dwell advances immediately").
- A3: all-disabled fallback tested.
- §10 D3: CONFIG codec rejects nonzero reserved bits (2 cases); parser-path
  dispatch tests for v1-additive SNAPSHOT, CONFIG, kinds 8/9; 0x86-skip
  desync guard.
- §9 A-1: AP distribution formula (0/4/8 at n=3) confirmed in
  panelScreensA.c; bands/stagger/legs per the normative table; RNG seed 77.
- §9 A-3: internet column inherits router state — APPLIED BY LEAD (the
  wrapper died before the amendment arrived; hardcoded healthy state
  replaced with haveRouter ? routerState : fail-dark 0).
- flash_safe_execute callback signature corrected BY LEAD (SDK wants
  void(*)(void*); was int-returning — target build error).
- Suites: daemon `make test` → codec tests passed; firmware host suite →
  142 ok, 0 fail.
- Target compile on lithium (pico2_w, SDK 2.1.1): clean, zero warnings —
  solari-panel-fw.uf2 sha256 d951389a… (single build; the TWO-BUILD
  reproducibility gate runs at deploy, per flash protocol).

## UNVERIFIED

- Real flash persistence on hardware (host tests use the flash mock);
  power-loss-mid-write case is design-only until the physical flash cycle.
- §10 D4 linker reservation: the config sector sits at PICO_FLASH_SIZE_BYTES
  - 4096 with no build-time assert that the image stays clear — REVIEW MUST
  CHECK image size headroom (current UF2 ~150 KB vs 4 MB flash: ample, but
  the assert is contractual and absent).
- A1 renderer has no direct host-test (geometry pinned via AW3's ported-
  constant harness on the virtual side + physical acceptance).
- Two-build reproducibility not yet run (deploy gate).

## DEVIATIONS

- Lead authored the A-3 fix and the flash-callback signature fix post-lane.
- Packet written by Lead; all other code authored by gpt-5.6-codex.
