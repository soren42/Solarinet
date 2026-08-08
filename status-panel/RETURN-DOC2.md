# RETURN-DOC2 — On-panel help overlay

**Task:** SOLNET-DOC2 (stretch goal) · **Date:** 2026-08-08

## STATUS

**IMPLEMENTED; lithium build gate blocked by this execution environment before
SSH connection.** The firmware-local feature is complete and the exact required
host command and untouched daemon suite both pass. No board was flashed, no
daemon or wire-codec source was changed, and no git command was run.

VOL+ now opens a two-line help overlay for the active `theme * 3 + screen`
entry. A second VOL+ closes it; another button closes it and performs its normal
action. The alarm acknowledge and sleep-wake early returns remain in their
original order ahead of all help logic. The overlay times out after 30 seconds,
dismisses immediately if an alarm arms, and pauses screen rotation while open.

## ARTIFACTS

All changed or added firmware files are under
`/home/jason/Code/Solarinet/status-panel/firmware/`.

| File | Lines | Change |
|---|---:|---|
| `main.c` | 654 | Initializes/ticks help, preserves alarm/sleep priority, routes VOL+, pauses dwell, and paints the overlay first. |
| `CMakeLists.txt` | 169 | Adds the state machine and renderer to the UF2 target. |
| `panelHelp.h` | 46 | Hardware-free public state-machine contract over caller-supplied seconds. |
| `panelHelp.c` | 30 | 30-second timeout, toggle, alarm dismissal, and non-VOL+ dismissal logic. |
| `panelHelpOverlay.h` | 10 | Renderer interface. |
| `panelHelpOverlay.c` | 44 | `HELP_TEXTS[12]` and two-line `panelText`/`panelScroll` renderer. |
| `test/panelHelpTest.c` | 66 | Plain-gcc synthetic-clock cases for every required transition. |
| `test/Makefile` | 45 | Builds/runs `panelHelpTest`; disables LeakSanitizer only for the pre-existing ASAN renderer test under ptrace-based harnesses. |

`status-panel/daemon/`, `status-panel/protocol.c`, and
`status-panel/protocol.h` are unchanged.

### Load-bearing integration

```c
if (gAlarmArmed) { ackAlarm(); return; }
if (gSleeping) { gSleeping = false; return; }

if (gHelp.active && b != PANEL_BTN_VOLUP) panelHelpDismiss(&gHelp);
...
case PANEL_BTN_VOLUP:
  (void)panelHelpToggle(&gHelp, gT, false);
  break;
```

The existing armed-alarm acknowledge return remains first and untouched. The
state machine also has an explicit `PANEL_HELP_IGNORED_ARMED` result, exercised
by the host test, so the priority is directly testable without Pico SDK code.

```c
runAlarm();
panelHelpTick(&gHelp, gT, gAlarmArmed);
...
if (!gHelp.active) {
  if (!gLink.lost) gScreenT += dt;
  ... rotate ...
}
...
if (gHelp.active) {
  panelHelpOverlayDraw((unsigned)(gTheme * 3 + gScreen), gT);
} else if (gLink.lost) {
```

This orders alarm-triggered dismissal directly after `runAlarm()`, freezes the
dwell accumulator, and gives the overlay precedence over normal/link-lost/no-
data painting. Alarm beacon/inlay code is unchanged.

## CALLS MADE

1. **VOL+ no longer changes volume.** It is exclusively the help button, per
   the task ruling. VOL- remains the sole volume adjustment button.
2. **Help text is centered when it fits and scrolls at 11 px/s when it does
   not.** The existing `panelTextW`, `panelText`, and `panelScroll` paths are
   used; primary line uses `cInk`, secondary uses `cAzure`. All 24 strings use
   only the required `[A-Z0-9 .:-]` character subset.
3. **The pre-existing ASAN renderer test runs with LeakSanitizer disabled.**
   In this ptrace-based harness LSAN itself aborts before the test executes.
   ASAN and UBSAN remain enabled, and the test has no heap-allocation purpose.
   This makes the required unqualified `make -C firmware/test` command portable
   to this harness.
4. **Lead authorship decision.** This was authored directly because the small
   pure-state-machine seam, its one renderer call site, and the required test
   case are tightly coupled and faster to verify as one bounded change.

## VERIFIED

### Host firmware suite — PASS (exit 0)

Command:

```sh
make -C /home/jason/Code/Solarinet/status-panel/firmware/test
```

Output tail (the command's full output was captured during execution):

```text
./panelHelpTest
ok:   case 1: init is inactive
ok:   case 1: VOL+ opens help at the supplied clock time
ok:   case 1: help remains open before 30 seconds
ok:   case 1: help closes at the 30 second boundary
ok:   case 2: armed-alarm VOL+ never enters help
ok:   case 3: second VOL+ dismisses help only
ok:   case 4: another button dismisses before its normal action
ok:   case 4: alarm arming immediately dismisses help

all panelHelp cases pass
ASAN_OPTIONS=detect_leaks=0 ./panelScreensATest
ok:   A1 renderer: APs+switch+router, no hubs
ok:   A1 renderer: APs+hubs+router, no switch
ok:   A1 renderer: full 9-device fleet
ok:   A1 renderer: APs only
ok:   A1 renderer: router only
ok:   A1 renderer: router down + wanBackup (A-3 inherit path)
ok:   A1 renderer: gearCount 0 (no-data path)
ok:   A1 renderer: mixed states fleet
make: Leaving directory '/home/jason/Code/Solarinet/status-panel/firmware/test'
```

The same successful run also executed and passed `panelLinkTest`, `panelCtlTest`
and `panelScreenCfgTest` before the excerpt above. A standalone compile check
also passed:

```sh
cc -std=c11 -Wall -Wextra -Werror -Istatus-panel/firmware \
  -c status-panel/firmware/panelHelpOverlay.c -o /tmp/panelHelpOverlay.o
```

### Daemon suite — PASS (exit 0, source untouched)

```sh
make -C /home/jason/Code/Solarinet/status-panel/daemon test
```

Output tail:

```text
make: Entering directory '/home/jason/Code/Solarinet/status-panel/daemon'
cc -I. -Ivendor -I.. -O2 -std=c99 -Wall -Wextra -Werror -Wno-unused-function -DSOLARI_PANEL_TEST -o tests/codec_test tests/codec_test.c solariPanel.c ../protocol.c vendor/cJSON.c -lcurl
./tests/codec_test
2026-08-08T06:33:21Z solariPanel: withholding panel commands until STATE confirms CONTROL capability
2026-08-08T06:33:21Z solariPanel: forwarded panel command id=21 kind=1 arg=0
codec tests passed
make: Leaving directory '/home/jason/Code/Solarinet/status-panel/daemon'
```

## UNVERIFIED

1. **Lithium two-build hash gate did not run.** The required initial rsync
   failed before reaching lithium:

   ```text
   Bad owner or permissions on /etc/ssh/ssh_config.d/20-systemd-ssh-proxy.conf
   rsync: connection unexpectedly closed (0 bytes received so far) [sender]
   rsync error: unexplained error (code 255) at io.c(232) [sender=3.4.1]
   ```

   A system-config-free connection could not resolve `lithium` in this
   environment. Consequently, **there are no b1/b2 UF2 hashes and no lithium
   warning-count result to quote**. No remote files were changed and no board
   was flashed.
2. **No physical button test occurred.** Physical VOL+ behavior, debounce
   timing, and on-panel overlay legibility/scroll readability remain unverified.
3. **No RP2350 compile occurred locally.** The renderer’s strict host compile
   passes, but the Pico SDK / arm-none-eabi full build remains gated on repaired
   lithium SSH access.
