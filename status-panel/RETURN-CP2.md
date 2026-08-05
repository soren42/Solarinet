# CP2 Return Packet

## Status

Complete and ready for Lead review.

## Changed

- `protocol.c`: added allocation-free, fixed-offset CONTROL and STATE
  encode/decode codecs; parser now dispatches both defined frame types.
- `daemon/solariPanel.c`: forwards each served command in order only after a
  successful poll snapshot write and after the current serial link has
  received STATE; logs withholding once per link-up. Decoded STATE is logged
  on transition and POSTed to `/api/panel/state` on change or a 30-second
  cadence using the existing hardened curl handle.
- `daemon/tests/codec_test.c`: CONTROL/STATE round-trip, wire-offset, short
  buffer, and truncated-payload coverage.

## Contract decisions

- Implemented CONTRACT-CP v1.1 §10: commands are re-served every poll; no
  command-ack request is made; `lastCmdId` is sent only as part of STATE.
- Preserved snapshot sequence staging and curl `CURLOPT_FORBID_REUSE`.
- `protocol.h` was not modified. It provides CONTROL/STATE constants and
  payload sizes but no function declarations or payload structs, so the
  consumers declare the fixed-field codec signatures locally.

## Verification

```text
make clean && make && make test
codec tests passed
cc -std=c99 -Wall -Wextra -Werror -I.. -c ../protocol.c
git diff --check
```

## UNVERIFIED

- No live authenticated API or serial-panel integration exercise was run;
  verification is compile and hardware-free codec coverage only.

## FIX ROUND CP12

### Implemented

- STATE codecs now carry `dwellSec` at byte 7 and tolerate trailing payload
  bytes; the daemon decodes, logs, change-detects, and POSTs it.
- Fixed the codec bounds tests and added byte-at-a-time parser dispatch tests
  for real CONTROL and STATE frames.
- Invalid queue elements are logged as `skipping invalid command id=%u kind=%u`
  and do not close the serial link; only failed frame writes return `-1`.
- Removed stale local codec declarations. STATE post failures now retry on the
  next frame, and the explicit dwell byte eliminates the former padding hazard.

### Verified

```text
make -C status-panel/daemon clean && make && make test
codec tests passed
```

### UNVERIFIED

- Live serial/API STATE posting and capability-gate behaviour were not run.

### Review follow-ups skipped

- S8 is a contract/UI wording question (`consumed` versus `applied`), outside
  CP2's allowed files. N4/N5 are non-blocking daemon nits and N1-N3/N6 belong
  to the migration/API/page owners.
