# RETURN-CP3 — Firmware CONTROL/STATE support

`2026-08-04 · Claude Opus / worker · task PANEL-CP3 · branch feat/panel-control`
`governed by CONTRACT-CP.md v1.1 §3/§6/§10`

## Status

**Complete and buildable.** Host suite green (two binaries), two-build UF2 pair
byte-identical, zero warnings, image reports version 1.1. Not flashed — the
Lead owns the gate.

Latest round: STATE byte 7 carries `dwellSec` (v1.1 CP amendment). The shared
codec gained the `dwellSec` parameter on `panelEncodeState`/`panelDecodeState`
while this round was in flight, so the field now reaches the wire and the
earlier BLOCKED pin is gone. **This build is the flash candidate.**

| Artifact | Value |
|---|---|
| UF2 sha256 (b1 == b2) | `565518ac31942e87548cb10a3866986b7e08d3801b0a6c9054f56805428cfebd` |
| Archive | `lithium:~/fw-archive/565518ac.uf2` |
| Supersedes | `2fe309ef…` (pre-dwellSec; do not flash) |
| `picotool info` | `solari-panel-fw` / `1.1+1785801600` / RP2350 ARM Secure |
| Build warnings | 0 in both builds |
| protocol.c stubbed? | **No.** See "Peer CP2" below. |

## Files

| File | Change |
|---|---|
| `firmware/panelCtl.h` / `.c` | **new** — CONTROL dedupe/validation + STATE cadence, the testable unit |
| `firmware/main.c` | CONTROL RX + application, STATE emission, `ackAlarm()` extracted, dwell made runtime-settable |
| `firmware/CMakeLists.txt` | `panelCtl.c`; version minor 0 -> 1; configure-time guard that the shared codec is present |
| `firmware/test/panelCtlTest.c` | **new** — 15 cases / 76 assertions |
| `firmware/test/Makefile` | builds and runs both suites |
| `firmware/README.md` | layout table + a "software control" runtime section |

`protocol.h`, `protocol.c`, daemon, PHP and JSX untouched. No git operations.

## Design

`panelCtl` is pure decision logic over a caller-supplied millisecond clock —
same shape and same reasoning as `panelLink.c`. It never touches hardware or a
firmware global. It answers two questions and nothing else:

1. **Should this command be consumed, and what action does it map to?**
2. **Should a STATE frame go out this tick?**

`main.c` performs every action through the function a physical button already
calls: `pressTheme()` for setTheme, `ackAlarm()` for ackAlarm,
`panelHwSetBrightness()` + `gAutoBright = false` for setBrightness. There is
deliberately no second copy of any of that behaviour, so a software press and a
physical press cannot drift.

## Deviations and judgement calls (all deliberate, all recorded)

> **Adjudicated 2026-08-04.** The Lead approved items 1, 2 and 3 below and
> recorded them in CONTRACT-CP.md §10 (amended); the CONTROL-is-not-an-ack /
> does-not-wake design in item 4 is endorsed. They are contract now, not
> deviations — kept here for the reasoning, which the amendment does not repeat.
> The Lead also closed the other half of item 2: the daemon now sends HELLOREQ
> at every serial link-up, so `panelCtlForceReport()` has a live trigger.

**1. STATE is emitted on any change to a *wire* field, not only to the 7 state
fields.** The assignment said "on ANY of the 7 state fields changing"; that is
insufficient and §10 is why. `lastCmdId` is the ONLY completion signal the
server accepts, and several correct outcomes move nothing else:

- `ackAlarm` with no alarm armed
- `setBrightness` to the percentage already showing
- any command the firmware *rejects* (unknown kind, out-of-range arg)

(`setDwell` was on this list until the v1.1 amendment gave dwell byte 7; it now
reports via the field *and* `lastCmdId`.) Under the narrow rule each of those
sits pending for the full 120 s and then
surfaces to the operator as an expiry, for a command the panel handled exactly
right. Emission therefore triggers on `lastCmdId` and `ackedEpisodeId` too.

**2. A rejected command still consumes `lastCmdId`.** §10 says so explicitly
("applied, or rejected as invalid/unknown — both consume") and the reason is
the same as above: rejection has to be terminal or it re-serves until expiry.
Only a *duplicate* (`cmdId <= lastCmdId`) leaves the counter alone.

**3. Change-triggered STATE has a 1 s floor** (`PANEL_CTL_MIN_STATE_MS`).
Without it auto-brightness alone floods the link: `gAutoBrightSmoothed` moves
every 40 ms tick, so `brightnessPct` genuinely ticks through whole percents
whenever room light changes, and every one of those is "a state change" — up to
25 frames/s. The floor bounds it to ~1 Hz (~26 B/s), an order of magnitude
inside the ≤7 s round-trip budget. A change seen inside the window is **not**
dropped: `panelCtlPoll` re-evaluates against live state every tick and emits the
moment the window opens. The heartbeat is exempt from the floor.

**4. CONTROL is never swallowed as an acknowledge, and never wakes the panel.**
`handlePress` consumes *any* button as an ack while the alarm is armed
(CONTRACT §5) and *any* button as a wake while sleeping, because a physical
button carries implicit intent. CONTROL carries explicit intent: the page has
its own ACK button (kind 5) and its own sleep toggle (kind 7, explicit 0/1).
Applying the button rules to CONTROL would make every other page control dead
whenever an alarm is up, and would light a sleeping panel on a theme change.

**5. Added `panelCtlForceReport()`, called on HELLOREQ.** §10's capability gate
withholds the entire queue until the daemon has seen a STATE frame *since the
current serial link-up*. After a **daemon** restart against an already-booted
panel there is no boot STATE to see, so the first command of the session would
stall on the 30 s heartbeat. The daemon already sends HELLOREQ on link-up; the
panel now answers with HELLO **and** a forced STATE. 24 extra bytes, removes a
30 s worst case. Approved; the daemon side now sends HELLOREQ at every serial
link-up, so the trigger is live rather than incidental.

**6. Dwell became a runtime variable** (`gDwellSec`, was `#define DWELL_SEC`),
and a `setDwell` resets `gScreenT` so a shortened dwell cannot strand the panel
on the current screen for the old interval. `setScreen` resets it too, matching
a manual button advance.

**7. `setDwell` rejects rather than clamps.** 5 does not become 6, 31 does not
become 30. Clamping would report success for a dwell the operator never chose.

## Peer CP2 — resolved mid-task

I began against a `protocol.h`/`protocol.c` that had the frame types and payload
sizes but no codec, and built a local `protocolCpStub.{h,c}` to compile against.
**CP2 landed while I worked**, with *scalar* signatures rather than the
struct-based ones I had assumed (`panelEncodeState(theme, screen, …, payload,
cap)` rather than a `PanelStateReport *`). I deleted the stub, re-pointed
`main.c` and the test at the real codec, and the final build and both suites are
against `../protocol.c` verbatim. **Nothing in the delivered tree is stubbed.**

CP2 also correctly widened `protocol.c`'s `knownType()` to admit `0x04` and
`0x84`. That was the one thing that could have made this firmware look perfectly
healthy while silently discarding every command — CRC-valid frames dropped
before the callback, no desync, no counter, no symptom. Test cases 12 and 12b
drive real frames through the real parser byte-at-a-time and assert the
dispatch, so the gate stays closed.

## Test suite

`make -C firmware/test` builds and runs both binaries. 16 cases in
`panelCtlTest.c`, all green:

| Case | Covers |
|---|---|
| 1 | every valid kind maps to its action, argument preserved, boundaries (`brightness 100`, `sleep 0` is wake not toggle) |
| 2 | out-of-range args and unknown kinds are **rejected AND consumed** |
| 3 | `setDwell` takes exactly 3/6/30 — 0, 5, 31, 255 rejected, not rounded or clamped, and each still consumes |
| 4 | dedupe, strict ascent, `cmdId 0` never valid, AUTO_INCREMENT gaps, a re-served batch applying only its unseen tail |
| 5 | reboot replay — asserts the §10-accepted behaviour, so a future change that persists `lastCmdId` reads as a contract change, not a fix |
| 6 | STATE on change; the floor withholds and then releases; settles |
| 7 | `lastCmdId` alone triggers a report (dwell, rejected command); an ignored duplicate does **not** |
| 8 | 30 s heartbeat, exact boundary, re-arms rather than repeating |
| 9 | 60 s of per-tick brightness churn stays ≤ ~1 Hz and is not suppressed altogether |
| 10 | uint32 millisecond wrap at ~49.7 days |
| 11 | per-episode ack transitions (ack falls away on a new episodeId) |
| 11c | a dwell change alone triggers a report (field path), and a *commanded* dwell change reports via field + `lastCmdId` |
| 11b | HELLOREQ forces a report, one-shot, restarts the heartbeat clock |
| 12 / 12b | real CONTROL / STATE frames through the real parser, fed one byte at a time |
| 13 | STATE payload round-trip incl. u32 endianness at the ceiling, `dwellSec` asserted at raw wire offset 7 as well as through the decoder, short-buffer refusal |
| 14 | truncated CONTROL payload rejected, so no cmdId is consumed unread |

## UNVERIFIED

Everything below needs the board, or a peer, and none of it was exercised here.

1. **Nothing has run on hardware.** No CONTROL frame has been applied by a real
   panel and no STATE frame has left one. All 76 assertions are host-side.
2. **End-to-end round trip** (§10 acceptance 3) needs CP1's API and CP2's daemon
   forwarding; the firmware half is untested against either.
3. **Brightness percentage fidelity.** `brightnessPct()` rounds
   `panelHwGetBrightness() * 100`. Whether the Pimoroni driver returns exactly
   what was set (vs. quantising to its own internal steps) is unmeasured, so the
   page's slider may read back a percent or two off what it sent. Cosmetic if so.
4. **The 1 s floor's real-world frame rate** is a calculation, not a measurement.
   If the daemon journal shows more STATE traffic than expected in daylight,
   `PANEL_CTL_MIN_STATE_MS` is the single knob.
5. **`sleep(1)` while an alarm is armed** puts the panel into the blanked branch
   that still draws the beacon (existing `main.c` behaviour, unchanged by me).
   That is the right outcome as far as I can tell, but it is now reachable
   remotely rather than only by someone standing at the panel, which is a
   different exposure and worth a look on the board.
6. **Physical buttons after flash** (§10 acceptance 7) — `handlePress` was
   refactored to route its ack through the new `ackAlarm()`. Logically identical;
   unverified on hardware. Spot-check the ack path specifically.

## Build reproduction

```sh
# on lithium, sources synced to ~/panel-build/{firmware,protocol.c,protocol.h}
export PIMORONI_PICO_PATH=/home/jason/pico/pimoroni-pico
bash ~/panel-build/twobuild.sh
sha256sum b1/solari-panel-fw.uf2 b2/solari-panel-fw.uf2   # must match
```
