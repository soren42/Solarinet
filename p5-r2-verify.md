# P5-R2 final cross-lab verification — `8bcdbca`

VERDICT: APPROVE

Reviewed `d594b25..8bcdbca`. The three assigned findings are fixed, and no new
defect was found in the patch.

## 1. MUST-8 — FIXED

`status-panel/daemon/listener.c:24-27` now protects `WRITE_MS` with `#ifndef`
and retains `5000u` as the default. The ordinary `solariPanel` build has no
override, while only `tests/listener_test` is compiled with
`-DWRITE_MS=300u` (`status-panel/daemon/Makefile:15-16,25-26`). Preprocessor
checks produced `WRITE_MS 5000u` for the production invocation and
`WRITE_MS 300u` for the listener harness invocation.

Case 16 is a genuine stalled-write construction
(`status-panel/daemon/tests/listener_test.c:92-100`): it completes an
authorized TLS handshake, leaves that client open but performs no client
reads, and repeatedly calls `panelListenerWrite()` with 8192-byte payloads.
There are no intervening `panelListenerService()` calls in this case, so the
read-idle path cannot evict the client; the client is not closed until after
the eviction assertion; and the case does not inject a handshake or peer-close
failure. Repeated writes therefore fill the nonblocking TLS/socket buffers,
drive `SSL_write()` to `SSL_ERROR_WANT_WRITE`, and leave `sslWrite()` polling
until its real monotonic deadline (`status-panel/daemon/listener.c:85`). The
failed write then clears the active peer through the production
`panelListenerWrite()` teardown path (`listener.c:95`). The test requires both
the failed write and loss of the active connection.

The listener case could not be executed in this sandbox because loopback bind
is prohibited; see UNVERIFIED. Its source construction and build-time deadline
selection satisfy the missing host-coverage finding.

## 2. SHOULD-1 — FIXED

`logHasExact()` uses whole-line `strcmp()` matching
(`status-panel/daemon/tests/listener_test.c:30`). Exact checks now pin:

- `denylist <path> missing; using empty denylist` (`listener_test.c:47`);
- `TLS files must be regular and root-or-daemon-owned; key mode 0600 or stricter, certificate/CA 0644 or stricter` (`listener_test.c:87`); and
- `TLS listener ready on 127.0.0.1:<resolved-port>` (`listener_test.c:47`).

The readiness line is emitted inside `panelListenerCreate()` only after
successful bind/listen and `getsockname()`, using `l->port`, so a port-zero
harness bind reports the actual assigned port
(`status-panel/daemon/listener.c:88-89`). The duplicate configured-port log was
removed from `runDaemon()` (`status-panel/daemon/solariPanel.c:204`). There is
no daemon startup regression: `runDaemon()` still supplies `listenerLog`, which
forwards the listener message unchanged to `logMessage()`
(`solariPanel.c:152`), and listener creation still fails before entering the
service loop if setup fails. The move changes the source and corrects the port
value, but preserves successful-start ordering and journal behavior.

The runbook still quotes `TLS listener ready on <addr>:7443` at
`docs/panel/SolariNet_Panel_Standalone_Runbook.html:228`; that matches the
production emission for its configured address and port 7443.

## 3. MEDIUM fakePanel teardown race — FIXED

`fakePanel` now retains the held slave descriptor and, after processing the
final WIPE, polls `FIONREAD` on it for at most 200 iterations with 10 ms sleeps
(`status-panel/tools/fakePanel.c:164-170,199-212`). The logic is sound for this
PTY arrangement: bytes written by the master are queued as slave input, and
all opens of the slave observe the same terminal input queue. The final
PROVACK write completes before this loop begins. A nonzero count therefore
means the client has not consumed all queued ACK bytes; zero means it has, so
closing the master can no longer turn that pending read into `EIO`. The bound
limits teardown delay to approximately two seconds.

`sh status-panel/tools/smoke.sh` was run independently 10 times: **10 PASS / 0
FAIL**. Every run ended with the WIPE PROVACK success, an empty fake store, and
`smoke: dropped-ack + desync recovery OK`.

## New defects

None found. `git diff --check d594b25 8bcdbca` passed. The patch is confined to
the claimed listener build/test/logging and fake-panel teardown changes, apart
from the prior review/assignment documents added by the commit.

## Test evidence

- `sh status-panel/tools/smoke.sh`, 10 independent runs: **10/10 PASS**.
- `make -C status-panel/daemon test`: **PARTIAL**. `codec_test` passed; the
  listener harness stopped at `listener bind failed: Operation not permitted`.
- Production/test preprocessor checks: **PASS**, respectively `WRITE_MS 5000u`
  and `WRITE_MS 300u`.
- `git diff --check d594b25 8bcdbca`: **PASS**.

## UNVERIFIED

- Runtime execution of listener case 16 and the exact-log listener cases was
  not possible because this sandbox denies the harness's `127.0.0.1` bind.
  Those cases were verified from source and build commands only.
- No live daemon/systemd journal startup was exercised; daemon startup logging
  behavior and runbook agreement were verified from the production call path
  and exact source text.
