# CONTRACT-SW v1.0 — P5 final whole-branch review

VERDICT: REVISE

The shared protocol constants and codecs are consistently consumed by firmware,
daemon, and tools, and I found no provisioning/power/transport state that wedges
solely because of a power episode or a USB/WiFi sequence reset. The gate remains
blocked by two cross-phase alarm seams, four actionable documentation mismatches,
and host-provable acceptance gaps.

## Findings

### MUST-1 — A same-tick CONTROL acknowledge is consumed before the alarm exists

`onFrame()` applies CONTROL immediately while `pumpSerial()`/the WiFi parser is
running (`status-panel/firmware/main.c:284`, `status-panel/firmware/main.c:291`).
`ACKALARM` calls `ackAlarm()` (`status-panel/firmware/main.c:448`), which returns
without effect while `gAlarmArmed` is false (`status-panel/firmware/main.c:358`).
The tick does not poll the VBUS edge until `status-panel/firmware/main.c:739`, and
does not derive/arm the composite alarm until `status-panel/firmware/main.c:750`.
Consequently, an ACK CONTROL received in the tick that commits a VBUS-loss edge
is accepted by `panelCtlConsume` (and later reported through `lastCmdId`) but does
not acknowledge the power episode; the alarm arms after the command has already
been consumed. The analogous SNAPSHOT-then-ACK burst has the same failure because
the snapshot updates `gEnv` before `runAlarm()` updates `gAlarmArmed`.

This is the ordering hazard left by d3ab452: the move fixed the physical-button
path but not the transport CONTROL path. It violates CONTRACT-SW §13 D15/D16
(`CONTROL` and button acknowledgements share ack-all semantics).

### MUST-2 — A new power episode does not re-arm while a server episode holds the alarm

`runAlarm()` reduces both sources to one boolean and only initializes cadence,
wake, and auto-silence state when `gAlarmArmed` changes false→true
(`status-panel/firmware/main.c:566`, `status-panel/firmware/main.c:572`,
`status-panel/firmware/main.c:594`). It never observes `gPower.episodeId` as an
identity transition. If a server alarm remains unacknowledged across a restore
and a later VBUS-loss edge, the global alarm remains armed; the new power episode
does not sound immediately, reset the five-minute auto-silence, or wake the panel.
If the server episode had already auto-silenced, the new power episode is silent.

The module suite tests only the OR predicate (`status-panel/firmware/test/panelPowerTest.c:124`),
so it cannot catch this integration failure. This contradicts CONTRACT-SW §13
D15 (every loss edge is a fresh episode), D16 (independent episode sources), and
the shipped operator statement that the second outage alarms again
(`docs/panel/SolariNet_Panel_Manual.html:1101`).

### MUST-3 — Ack-all does not journal the episodes it covered

The only acknowledgement diagnostic is `PANEL_EV_ACK` with one zero byte
(`status-panel/firmware/main.c:368`, `status-panel/firmware/main.c:373`;
`status-panel/protocol.h:199`). The daemon consequently journals only EVENT
kind/arg (`status-panel/daemon/solariPanel.c:150`). Neither the server episode ID
nor `gPower.episodeId` is emitted. CONTRACT-SW §13 D16 explicitly requires the
journal to record which episodes an ack-all covered. The behavior is therefore
not operationally auditable as contracted.

### MUST-4 — The runbook's file-mode rule is not what the listener enforces

The runbook says the key must be `0600` and certificates may be `0644 or
stricter` (`docs/panel/SolariNet_Panel_Standalone_Runbook.html:224`). The common
`safeFile()` check accepts either exactly `0600` or exactly `0644` for the private
key, server certificate, and client CA alike (`status-panel/daemon/listener.c:60`,
`status-panel/daemon/listener.c:75`). Thus a world-readable `0644` private key is
accepted, while genuinely stricter modes such as `0400` or `0640` are refused.
Align code and runbook with CONTRACT-SW §8 secret discipline and §13 D11 before
an operator relies on this instruction.

### MUST-5 — The manual says the unprovisioned radio is off, contrary to D18 and code

The manual states, “With no stored credentials the radio stays off”
(`docs/panel/SolariNet_Panel_Manual.html:1070`). Production initialization calls
`cyw43_arch_init()` and enables station mode unconditionally
(`status-panel/firmware/panelNet.c:425`, `status-panel/firmware/panelNet.c:440`).
Only the network FSM remains OFF. CONTRACT-SW §13 D18 expressly struck the old
self-disable behavior because VBUS sensing requires unconditional CYW43 init.
The manual should say that WiFi association/transport remains inactive, not that
the radio is off.

### MUST-6 — The troubleshooting manual promises refusal reasons that are not logged

The manual directs the operator to the xenon journal because certificate
refusals are logged “with the reason” (`docs/panel/SolariNet_Panel_Manual.html:1077`).
The listener emits only `TLS client refused` plus peer and presented identity
(`status-panel/daemon/listener.c:69`); it does not distinguish chain, SAN, EKU,
validity, or denylist failures. A firmware-side refusal of the daemon certificate
also cannot report a reason over the TLS link it just rejected
(`status-panel/firmware/panelNet.c:310`). This is actionable troubleshooting
guidance that the implementation does not honor (CONTRACT-SW §13 D6–D10).

### MUST-7 — The runbook and contract claim per-item PROVACK output that the tool does not print

The runbook tells the operator that every item prints its PROVACK and that `ok`
throughout is the success signal (`docs/panel/SolariNet_Panel_Standalone_Runbook.html:254`).
The tool prints only `<name> <bytes> staged` after a whole item completes
(`status-panel/tools/panelProv.c:236`), then prints `COMMIT ok`
(`status-panel/tools/panelProv.c:539`). It does not print per-TLV PROVACKs or even
the word/status `ok` for each item. This also misses the still-binding tool
requirement in CONTRACT-SW §8 (`status-panel/CONTRACT-SW.md:172`). Make the output
and the verification instruction agree.

### MUST-8 — D21 has host-provable cases with no committed end-to-end coverage

CONTRACT-SW §13 D21 requires lost-PROVACK retry/out-of-order resume and expired +
not-yet-valid certificates. The handler suite proves idempotent re-ack and status
5 in isolation, but the pty smoke injects no dropped ACK or offset mismatch; it
only exercises duplicate config rejection, a happy provision, and wipe
(`status-panel/tools/smoke.sh:44`, `status-panel/tools/smoke.sh:51`). Therefore
`panelProv`'s retry/resume implementation is host-provable but untested.

The listener suite generates/tests an expired client certificate but no
not-yet-valid client certificate (`status-panel/daemon/tests/listener_test.sh:25`,
`status-panel/daemon/tests/listener_test.c:43`). It also does not cover the
host-provable unreadable-denylist fatal path, exact file-mode policy, 90 s read
idle deadline, or 5 s write deadline (`status-panel/daemon/listener.c:22`,
`status-panel/daemon/listener.c:60`, `status-panel/daemon/listener.c:62`,
`status-panel/daemon/listener.c:72`). This leaves §10.1, §13 D10/D11, and D21
short of the contract's observable host gate.

### SHOULD-1 — Assert exact operational log texts in the listener suite

The runbook depends on the exact startup/refusal messages at
`docs/panel/SolariNet_Panel_Standalone_Runbook.html:223`,
`docs/panel/SolariNet_Panel_Standalone_Runbook.html:228`, and
`docs/panel/SolariNet_Panel_Standalone_Runbook.html:315`. The suite counts lines
containing `refused` but does not assert the missing-denylist, ready, connected,
or refusal strings (`status-panel/daemon/tests/listener_test.c:25`). Exact-text
assertions would prevent docs and journal output from drifting again. Clause:
CONTRACT-SW §13 D10/D11 and §10.9.

## Acceptance coverage

| Contract item | Host evidence in the branch / this review | Remaining gate |
|---|---|---|
| §10.1 listener | Committed loopback suite covers wrong CA/SAN/EKU, expired client, denylist, unauthenticated supersession attempt, authenticated supersession, slow handshake, parser reset, HELLOREQ, and capability gating. Daemon codec passed here. | MUST-8 gaps; listener executable could not run in this sandbox. |
| §10.2 provisioning | Shared codec/status constants, generation changes, retransmit idempotency, status-5 handler path, commit validation, A/B election, corrupt/torn writes, wipe, and WiFi rejection passed in the firmware suite. The real `panelProv` happy provision+wipe pty smoke passed. | Real panel power-cycle; tool-level dropped-ACK/status-5 injection (MUST-8). |
| §10.3–§10.6 WiFi/mTLS/arbitration | FSM ordering, bounded jittered backoff, D13 transport adoption/discard, D14 USB suspension/resume, and TLS-selection helpers passed on host. | Live RP2350/AP/xenon/chlorine chain, both-direction firmware TLS negatives, command E2E, AP bounce, and measured reconnect bound. |
| §10.7 + D19 power | Debounce, namespace, boot-on-battery, two edge IDs, ack/restore state, and the OR predicate passed (49 checks). | MUST-1/MUST-2/MUST-3; real VBUS, cap/glyph/tone, pack measurement, ratification, repeat runtime, and ≥3 cycles. |
| §10.8 regression | Firmware host suite passed, including parity 107/107; daemon codec passed; tool build/smoke passed; D17 PHP range test and JS panel parity 59/59 passed. | Listener harness blocked locally; device build/two-build UF2 pair, serial bench round trip, and the full repository JS/PHP suite were not exercised here. |
| §10.9 docs | Config keys, status names, source behavior, and journal strings were source-compared. | MUST-4 through MUST-7; physical print check and live operational procedure. |
| D21 additions | Host suites cover handler retransmission/status 5, torn/corrupt active records, wrong CA/SAN/EKU + expired client, unauthenticated supersession, slow handshake, D13 sequence cases, two power edges, and source OR-composition. | MUST-8; real intermediate chain, device-side SAN/EKU/time negatives and key-match crypto seam remain bench/device gated. |

## Test evidence

- `make -C status-panel/firmware/test`: PASS (all binaries; power 49 checks;
  framebuffer parity 107/107).
- `make -C status-panel/daemon test`: PARTIAL — codec PASS; listener harness
  stopped at loopback bind with `Operation not permitted` in this sandbox.
- `make -C status-panel/tools && sh status-panel/tools/smoke.sh`: PASS.
- Additional focused committed gates: D17 PHP episode test PASS (1505-seed
  sweep); JS panel parity PASS (59/59).
- `git diff --check main...HEAD`: PASS.

## Secrets discipline

PASS for the branch diff. A metadata-only scan of all added lines found no
private-key blocks, embedded SSH/PGP key material, tokens, passwords, PSKs, or
secret-value assignments. Documentation contains paths/placeholders only; the
tool smoke creates throwaway bytes at runtime in `mktemp` and commits none.

## UNVERIFIED

- The daemon listener harness could not bind a loopback socket under the current
  filesystem/network sandbox, so none of its ten runtime cases were re-exercised
  in this review.
- No RP2350/Pico SDK device build, two-build byte-identical UF2 pair, flash, or
  real hardware behavior was exercised.
- No live WiFi association, SNTP, DNS, mTLS against the real chlorine chain,
  xenon listener deployment, USB arbitration timing, AP/server bounce, or
  command round trip was exercised.
- No physical VBUS transition, battery brightness cap/glyph/tone observation,
  pack polarity connection, current/runtime measurement, operator ratification,
  repeat runtime, or one-hour reconnect/heap/stack soak was exercised.
- The runbook's certificate issuance, DNS, systemd, renewal, revocation, expiry
  probe, and rollback procedures were source-reviewed but not executed.
- The manual/runbook were not physically print-checked in this review.
- The full repository JS/PHP regression set, serial-mode bench round trip, and
  D20 live fault matrix were not exercised.
