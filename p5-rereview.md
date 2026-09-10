# P5-R1 re-verification — `d594b25`

VERDICT: REVISE

## Finding-by-finding verification

### MUST-1 — FIXED

`PANEL_CTLACT_ACKALARM` now records `gAckPending` instead of calling
`ackAlarm()` while the transport parser is running
(`status-panel/firmware/main.c:453-465`). The main tick polls VBUS, calls
`runAlarm()`, and only then consumes the deferred acknowledgement
(`status-panel/firmware/main.c:768-788`). Consequently, both a VBUS episode
committed in that tick and a SNAPSHOT parsed earlier in the same serial burst
exist before the CONTROL acknowledgement reaches the common `ackAlarm()` path.
This resolves the same-tick ordering defect as stated. There is no dedicated
host integration test for this exact main-loop ordering; the evidence here is
the production call order.

### MUST-2 — FIXED

The armed alarm now carries source identity in `PanelAlarmIdent`, and
`panelAlarmIdentUpdate()` reports a fresh server or power episode even when the
other source already holds the composite alarm
(`status-panel/firmware/panelPower.c:63-80`). `runAlarm()` uses that result to
reset silence/cadence time, sound immediately, and wake the panel
(`status-panel/firmware/main.c:594-616`). The host suite directly covers a power
loss while server episode 100 remains live, server episode rollover, and a
second loss after restore (`status-panel/firmware/test/panelPowerTest.c:154-187`);
`make -C status-panel/firmware/test` passed with 61 power checks.

### MUST-3 — FIXED

ACK EVENT now has a shared 10-byte codec carrying coverage bits plus server and
power episode IDs (`status-panel/protocol.h:199-218`,
`status-panel/protocol.c:185-203`). `ackAlarm()` captures both live unacknowledged
identities before clearing the power side (`status-panel/firmware/main.c:360-389`),
and the daemon decodes and journals all three values
(`status-panel/daemon/solariPanel.c:150`). The daemon codec suite exercised the
round trip, uncovered-ID zeroing, trailing extension, truncation, and wrong-kind
cases and passed before the listener bind failure
(`status-panel/daemon/tests/codec_test.c:159-172`).

### MUST-4 — FIXED

`safeFile()` now applies role-specific forbidden-bit masks: the key rejects all
group/other and execute bits, while certificates/CA reject write and execute
bits but permit read bits (`status-panel/daemon/listener.c:60-65,85`). The
runbook states the same policy, including accepted 0400 keys and stricter
certificate modes (`docs/panel/SolariNet_Panel_Standalone_Runbook.html:224`).
The listener source test supplies a rejected 0644-key fixture and an accepted
0400-key/0600-certificate fixture (`status-panel/daemon/tests/listener_test.sh:32-40`,
`status-panel/daemon/tests/listener_test.c:83-89`). Runtime execution of those
listener cases was blocked by loopback binding in this sandbox.

### MUST-5 — FIXED

The manual no longer says the radio is off. It says the unprovisioned WiFi
transport performs no association or network traffic while CYW43 still
initializes for VBUS sensing
(`docs/panel/SolariNet_Panel_Manual.html:1070`), which matches D18 and the
accepted unconditional initialization design.

### MUST-6 — FIXED

Every listener refusal now includes a sanitized `reason=` field
(`status-panel/daemon/listener.c:74-79`). Handshake verification failures use
OpenSSL's verification result; completed-handshake authorization failures
distinguish missing certificate, EKU, SAN, and denylist. The manual accurately
limits this promise to daemon-side refusals and explains the reverse direction
(`docs/panel/SolariNet_Panel_Manual.html:1077`). The listener test source checks
wrong CA, SAN, EKU, expired, not-yet-valid, and denylisted reasons
(`status-panel/daemon/tests/listener_test.c:47-66,78-79`), though those runtime
checks could not run here because loopback bind was denied.

### MUST-7 — FIXED

After each item completes, `panelProv` now prints the item name and `PROVACK ok`;
COMMIT and WIPE also name their successful PROVACKs
(`status-panel/tools/panelProv.c:236-240,493-503,538-547`). This agrees with the
runbook's per-item success instruction
(`docs/panel/SolariNet_Panel_Standalone_Runbook.html:255`). The smoke test also
counts all staged-item lines and requires every one to contain `PROVACK ok`
(`status-panel/tools/smoke.sh:51-59`).

### MUST-8 — PARTIAL

Most named host gaps gained real cases: `fakePanel` can drop an ACK and forge a
forward watermark (`status-panel/tools/fakePanel.c:92-134`), and the smoke test
requires recovery through the resulting genuine offset mismatch
(`status-panel/tools/smoke.sh:69-94`); successful runs printed both
`provision + wipe round-trip OK` and `dropped-ack + desync recovery OK`. The
listener fixtures/test source add not-yet-valid validity, unreadable denylist,
file-mode, and injected-clock read-idle cases
(`status-panel/daemon/tests/listener_test.sh:21-40`,
`status-panel/daemon/tests/listener_test.c:64-66,72-89`). However, the original
finding also required host coverage of the 5-second write deadline. The deadline
remains production-only at `status-panel/daemon/listener.c:24,82`; there is no
write-stall/deadline case anywhere in `status-panel/daemon/tests`. Therefore the
finding is not fully resolved. In addition, the new smoke pass is intermittent
as detailed under New defects.

### SHOULD-1 — PARTIAL

The suite now retains log lines and checks missing-denylist, connected, and
reason-bearing refusal content (`status-panel/daemon/tests/listener_test.c:21-35,46-66`),
but it does not assert exact strings: `logHas()` is a `strstr()` substring
search. It also never exercises or asserts the runbook's `TLS listener ready on
<addr>:7443` text, which is emitted by the daemon integration path at
`status-panel/daemon/solariPanel.c:204`, outside this listener harness. Thus it
improves drift coverage but does not resolve the exact-operational-text finding
as stated.

## New defects introduced by `d594b25`

### MEDIUM — The new fault-injection smoke pass is flaky at WIPE completion

The new second `fakePanel` lifecycle in `status-panel/tools/smoke.sh:73-94`
occasionally fails with `panelProv: read: Input/output error` and `wipe failed:
no PROVACK`. The fake increments `wipes`, writes the acknowledgement, then its
loop condition becomes false and the process immediately closes the PTY master
(`status-panel/tools/fakePanel.c:129-134,187-198`), so the client can observe
master teardown before consuming the buffered ACK. In isolated comparison,
the parent smoke passed 12/12 runs; the fixed revision passed 11/12, with the
failure on the fault-injection pass's WIPE. Earlier direct executions of the
fixed revision also failed twice at the baseline WIPE before later passes. This
makes the newly expanded required gate nondeterministic even though its recovery
logic succeeds when the ACK teardown race does not fire.

No production-code regression was found in the other touched files.

## Test evidence

- `make -C status-panel/firmware/test`: PASS; all firmware host binaries passed,
  including `panelPowerTest: 61 checks passed` and framebuffer parity 107/107.
- `make -C status-panel/daemon test`: PARTIAL; codec PASS, listener harness
  stopped at `listener bind failed: Operation not permitted`.
- `sh status-panel/tools/smoke.sh`: INTERMITTENT; the required recovery messages
  were observed on successful runs, but WIPE teardown failures were reproduced.
  A 12-run isolated sample at `d594b25` was 11 PASS / 1 FAIL; the isolated
  parent sample was 12 PASS / 0 FAIL.
- `git diff --check a3ce9b5 d594b25`: PASS.
- The in-scope worktree files matched `d594b25` during review; pre-existing
  unrelated workspace changes were not modified.

## UNVERIFIED

- The listener runtime suite could not bind `127.0.0.1` in this sandbox, so its
  handshake/refusal, not-yet-valid, unreadable-denylist, file-mode, and injected
  read-idle cases were verified from source but not executed here.
- The 5-second TLS write deadline has no committed test and was not exercised.
- Same-tick CONTROL acknowledgement ordering and alarm wake/tone/auto-silence
  effects were verified from production call order and pure host transition
  tests, not on RP2350 hardware.
- No live daemon journal/systemd deployment or physical/print rendering of the
  two HTML documents was exercised.
