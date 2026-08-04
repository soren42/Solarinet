# RETURN — PANEL-C2

## Status

BLOCKED for deployment completeness; implementation and local build/test are
otherwise complete. `protocol.h` was treated as read-only.

## Delivered

- `protocol.c`: portable C99 CRC, framing, snapshot codec, and incremental
  timeout/resynchronising parser.
- `daemon/`: C99 libcurl polling daemon, in-memory session cookies, one 401
  re-login retry, split exponential backoff, strict JSON/content-type gate,
  last-good state, API staleness marking, raw serial glob/literal open,
  serial read diagnostics, bounded write-all, reconnect/reset/immediate send,
  and PINGs before a valid snapshot exists.
- `daemon/tests/codec_test.c`: one-byte frame feed round trip, CRC rejection,
  and timeout/truncation resynchronisation test.
- Makefile, systemd unit, example configuration, and daemon README.

## Verification

Observed locally:

```text
make -C status-panel/daemon test
codec tests passed
make -C status-panel/daemon
cc ... -std=c99 -Wall -Wextra -Werror ...
```

## Blocker / rejected environment premise

The task requires vendoring upstream cJSON v1.7.19 `cJSON.c` and `cJSON.h`.
The sandbox cannot resolve `raw.githubusercontent.com`; the fetch failed with
`Could not resolve host`. It has Debian `libcjson-dev`, so `vendor/cJSON.h` is
a documented shim and the Makefile links `-lcjson`, allowing the daemon to
compile here. This is not the requested vendored single-file dependency and
must be replaced before deployment with upstream v1.7.19's two files; the
README gives the exact required Makefile change.

## UNVERIFIED

- Build against the actual vendored cJSON v1.7.19 source.
- HTTPS login/polling against xenon and the deployed CA file.
- USB CDC reconnect behavior on lithium / Galactic Unicorn hardware.

## Lead authorship decision

I authored directly because the bounded shared-codec and daemon implementation
were faster to integrate and validate in one working context. No agents were
delegated. No repository changes were committed or deployed.

## FIX ROUND 2

STATUS: COMPLETE

FIXED:

- C1/C2: parser drains every recovered buffered frame after resync and the
  encoder normalises `hasTopAlert`; shared `panelSeqNewer()` implements the
  RFC1982 comparison declared by `protocol.h`.
- D1-D8: password trimming is non-aliasing; curl is initialised correctly;
  alert episode/severity and wide metrics decode correctly; snapshots stage
  before replacing last-good state; server `alarmActive` is preserved; and
  deadlines start from the current monotonic tick.
- D9-D15: SIGPIPE/CURL no-signal handling, login header lifetime and JSON
  escaping, password mode enforcement, ordered serial glob fallback, reconnect
  backoff, and skew-safe stale logic are implemented.
- D18: codec tests now cover noise-header resync draining four frames,
  overlapping magic, mid-stream CRC corruption, RFC1982 wrap vectors, and
  malformed snapshot rejection without clobbering a last-good snapshot.
- U4: warning flags are appended rather than replaceable by an environment
  `CFLAGS` override.
- U1: the systemd service now enables the requested privilege, filesystem,
  home, temporary-directory, kernel-tunable, and address-family restrictions.

SKIPPED:

- D16/D17 and U3 are nits outside the requested daemon-code and test focus;
  the login body is nevertheless zeroed and copied by curl. Systemd hardening
  and configuration commentary were not changed because deployment unit files
  were not part of the explicit in-scope list.

VERIFIED:

- `make -C status-panel/daemon clean && make -C status-panel/daemon && make -C status-panel/daemon test clean` — clean; codec tests passed.
- `php -l dashboard/api/routes/panel.php` — clean.
- `git diff --check -- dashboard/api/routes/panel.php status-panel/protocol.c status-panel/daemon` — clean.

UNVERIFIED:

- HTTPS authentication/polling and USB reconnect remain hardware/environment
  verification on lithium and xenon.
