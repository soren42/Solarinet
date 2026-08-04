# RETURN — PANEL-CP1

## STATUS

IMPLEMENTED; live DB and HTTP acceptance checks are blocked by the managed
execution sandbox, not represented as passing.

## ARTIFACTS

- `db/migrations/016_panel_control.sql`
  - Adds `panelCommand` with `pending|applied|expired` lifecycle, timestamps,
    creator, and queue/recent indexes.
  - Adds singleton `panelState` for the latest decoded STATE frame.
- `dashboard/api/routes/panel.php`
  - Preserves every existing GET `/api/panel` field and adds `panelState`.
  - Exposes `commands` only to the exact local `panel` viewer principal.
  - Exposes 16 `recentCommands` only to operator/admin sessions.
  - Adds `POST /api/panel/command` (role and protocol validation, 16-pending
    cap, 409 on full) and `POST /api/panel/state` (exact service-principal
    gate, singleton state upsert, `lastCmdId` application confirmation).
  - Re-serves pending commands until applied/expired; expires stale pending
    rows on GET and before a command-cap check. Rejected unauthorized writes
    log principal and remote IP.

## VERIFIED

```
$ php -l dashboard/api/routes/panel.php
No syntax errors detected in dashboard/api/routes/panel.php

$ php -r '<validation harness>'
panel input validation happy-path: PASS

$ php -r '<invalid-input matrix>'
panel invalid-input matrix: PASS
```

Migration numbering was checked: existing migrations end at `014`; `016` is
therefore unused and matches the supplied CP1 contract number.

## UNVERIFIED

- Applying `016` against the live MariaDB schema and SELECT verification.
- Curl role matrix, queue cap/status lifecycle, and panel-principal STATE
  round-trip.

The sandbox denied the required local sockets:

```
ERROR 2002 (HY000): Can't connect to local server through socket
'/run/mysqld/mysqld.sock' (1)
curl: (7) failed to open socket: Operation not permitted
```

No secrets were printed.

## DEVIATIONS

- None in implementation scope. Runtime acceptance remains unexercised solely
  because this execution environment blocks MariaDB and `https://xenon:9443`.

## NEXT

On xenon outside the socket-restricted runner: source `run/db.env`, apply
`db/migrations/016_panel_control.sql`, run the requested role curl matrix,
then post a valid STATE as local `panel` and confirm the GET round-trip.

## FIX ROUND CP12

### Implemented

- Added `db/migrations/017_panel_dwell.sql` (not applied): `panelState.dwellSec`
  is `TINYINT UNSIGNED NOT NULL DEFAULT 0`.
- STATE input now validates `dwellSec` and clamps `lastCmdId` to u32.
- GET and STATE upsert/select paths store and expose `dwellSec`.
- Expiry runs only for a service-principal GET. A qualifying STATE confirmation
  converts matching `pending` or `expired` commands to `applied`, preserves
  `expiredAt`, and sets `appliedAt`.

### Verified

```text
php -l dashboard/api/routes/panel.php
No syntax errors detected
```

`017_panel_dwell.sql` was statically checked as a single valid MariaDB ALTER;
it was not applied.

### UNVERIFIED

- Live MariaDB migration/upsert and authenticated role/lifecycle checks remain
  unrun because the managed sandbox blocks the local database socket.

### Review follow-ups skipped

- S8 remains a contract/UI semantic choice. Migration nits N1/N2 and queue
  isolation N3 are intentionally deferred: altering 016 is outside this
  fix-round migration scope and warrants Lead approval.
