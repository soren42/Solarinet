# P3 WP-A Return — Firmware WiFi Transport FSM

## Summary

Implemented a platform-free, tick-driven WiFi transport policy core and the
D13 per-transport snapshot sequence state. No lwIP, mbedTLS, CYW43, protocol
wire-format, daemon, tool, dashboard, contract, or git changes were made.

### `PanelNetOps` seam

`PanelNetOps` contains asynchronous start callbacks for WiFi join, addressing,
SNTP, and TLS; an immediate close callback; a caller-supplied random-byte
callback; and one opaque user pointer. Completion and fault results return via
typed event injectors. The FSM owns all policy, timing, state, backoff, time
plausibility, and arbitration decisions; WP-B only implements effects.

The FSM has OFF, JOINING, ADDRESSING, TIMESYNC, CONNECTING, LINKED, BACKOFF,
and SUSPENDED states. It allocates no memory and uses integer/wrap-safe timing.
Backoff is exponential from a 1 s base, with seam-provided 0–25% additive
jitter below the ceiling and subtractive jitter at the ceiling so capped peers
do not synchronize; total delay never exceeds 60 s. A successful TLS
connection resets the base. Each failure retries its earliest still-valid stage: AP loss → JOINING,
DHCP failure → ADDRESSING, SNTP failure → TIMESYNC, and TLS failure/server
close → CONNECTING.

D7 is enforced at both initialization and SNTP completion: wall time must be
strictly greater than the injected build epoch before `startTls` can run.

D14 is exposed as `panelNetFsmUsbFrameSeen(fsm, typeKnown, nowMs)`, called only
after CRC validation. The header documents the exact qualifying known types.
The first qualifying frame calls `close` synchronously and enters SUSPENDED;
unknown types do nothing. Resume occurs after 15 s from the latest qualifying
frame, or immediately after `panelNetFsmCdcDisconnected`. A PROVISION commit
while USB remains active stays suspended rather than starting WiFi underneath
the provisioning session.

### Sequence-state diff

`PanelLink` now owns two independent `PanelLinkSeq` entries (serial and WiFi)
and an active-transport discriminator. `panelLinkSetActiveTransport` marks the
new transport's first snapshot for explicit adoption, regardless of lower,
equal, or wrap-boundary sequence value. Frames tagged with the inactive
transport are rejected. `panelLinkResetTransportSeq` supplies the analogous
explicit adoption signal for a new WiFi TCP connection.

The timed RFC1982 recovery escape hatch remains serial-only. LINKLOST/LINKBACK
now use accepted-SNAPSHOT staleness, per D14, rather than any valid frame;
switching transports does not alter the liveness timestamp or emit an edge.
`main.c` passes the existing parser's frames as serial, leaving WP-B to add the
WiFi parser/caller wiring.

## Files touched

- `status-panel/firmware/panelNetFsm.h` — new pure FSM API and state.
- `status-panel/firmware/panelNetFsm.c` — new policy implementation.
- `status-panel/firmware/panelLink.h` — transport and per-transport seq API.
- `status-panel/firmware/panelLink.c` — explicit adoption, inactive-frame
  rejection, serial-only resync, snapshot-only staleness.
- `status-panel/firmware/main.c` — identifies existing snapshots as serial and
  updates the stale-sequence rationale.
- `status-panel/firmware/CMakeLists.txt` — adds `panelNetFsm.c` to the firmware
  target source list.
- `status-panel/firmware/test/panelNetFsmTest.c` — new host suite.
- `status-panel/firmware/test/panelLinkTest.c` — updated API/staleness cases.
- `status-panel/firmware/test/Makefile` — registers the new ASAN/UBSAN suite.
- `p3-wpa-return.md` — this return packet.

## Verification

- `make -C status-panel/firmware/test`: PASS, all 9 test executables.
- New `panelNetFsmTest`: PASS, 36 checks, ASAN/UBSAN enabled.
- Updated `panelLinkTest`: PASS, 22 checks.
- Existing framebuffer parity suite: PASS, 107 checks, 0 failed.
- Standalone strict C11 compile of `panelNetFsm.c` and `panelLink.c` with
  `-Wall -Wextra -Werror -pedantic`: PASS.

## Contract readings / resolved ambiguity

- D7 “plausible (> firmware build timestamp)” is read strictly: equality is
  invalid and no TLS callback occurs.
- D13 “adopts the first CRC-valid snapshot” is read as unconditional ordering
  adoption on every active-transport switch. A new WiFi TCP connection is an
  additional explicit reset/adopt signal. The 30 s timed escape remains only
  on serial.
- D14 is read as snapshot-only staleness for LINKLOST/LINKBACK. PING qualifies
  as USB arbitration activity but does not keep snapshot data fresh.
- D20 specifies jittered bounded backoff but not a distribution. This module
  derives 0–25% jitter from one seam byte, adds it below the cap, subtracts it
  at the cap (avoiding synchronized fixed intervals), and keeps the final
  delay at or below 60 s.
- “Retry from the correct earlier state” is read as retaining successful lower
  layers when possible: DHCP/ADDRESSING, SNTP/TIMESYNC, TLS/CONNECTING;
  explicit AP loss returns to JOINING.

## UNVERIFIED

- Pico cross-build was not run in this host worktree; SDK/Pimoroni dependency
  availability and final UF2 link/image-size gate are unverified.
- Real CYW43/lwIP/SNTP/mbedTLS behavior, callback timing, CDC disconnect
  detection, TLS close semantics, and hardware reconnect behavior remain WP-B
  integration work and are unverified here.
- Bench reconnect soak, heap/stack high-water, forced AP bounce, and the D20
  hardware fault matrix remain unverified.
