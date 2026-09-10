VERDICT: APPROVE

Cross-lab verification of HEAD `d3ab452` against `HEAD~1` found all three P4 review findings resolved.

- MUST-1 resolved — `status-panel/firmware/main.c:739-751` commits the power edge, then calls `runAlarm()` before `scanButtons()`. A loss-edge or first alarming server snapshot therefore arms before `handlePress()` tests `gAlarmArmed`, so a same-tick debounced press reaches the existing ack-all path. Pure-server behavior remains correct: a new/live unacknowledged server episode is armed before the press; an already acknowledged episode stays unarmed; and an episode cleared by this tick's snapshot no longer consumes the press. No persistent tone or alarm-state regression was found.
- MUST-2 resolved — `status-panel/firmware/main.c:788-806` suppresses `panelInlayPower()` under help, matching the server inlay, while `panelPowerGlyph()` is outside the help branch and is drawn whenever `gPower.onBattery`. The sleeping path also retains its battery glyph at `main.c:778-785`.
- SHOULD-1 resolved — `panelAlarmWant()` is declared/implemented in `panelPower.[ch]`, used directly by `main.c:570`, linked into `panelPowerTest`, and exercised for no source, server-only, power-only, simultaneous sources, ack-all, restore isolation, and server-side transitions while power remains live. `panelPowerTest` now reports 49 checks passed.

Verification run:

- `make -C status-panel/firmware/test` — PASS; all test binaries passed, including `panelPowerTest: 49 checks passed` and `panelParityTest: 107 passed, 0 failed`.
- `git diff --check HEAD~1 HEAD` — PASS.

Remaining findings: none.

UNVERIFIED

- Device-only Pico SDK compilation/linking was not attempted; Pico/CYW43 headers and device link dependencies were not compiled in this host pass.
- Physical WL_GPIO2 behavior, real 100 ms edge timing, button/power-edge coincidence on hardware, brightness-cap apply/lift, glyph/inlay appearance on the LED panel, sleep rendering, and tone/night-profile behavior were not exercised on hardware.
- D19 pack identification, polarity confirmation, current/runtime measurement, operator ratification, repeat runtime, and the required three physical loss/restore cycles remain unverified pending the operator session.
