# RETURN — PANEL-C1

STATUS: COMPLETE

ARTIFACTS:

- `dashboard/api/routes/panel.php` — authenticated `GET /api/panel`, composed
  from one read-only consistent database snapshot; includes §3 and §9 fields,
  pool totals, split RX/TX, probe RTT/loss, maintenance suppression, server-side
  score/alarm calculation, and bounded panel-safe strings.
- `dashboard/api/routes.php` — minimal registration of the new read route.

VERIFIED:

- `php -l dashboard/api/routes/panel.php` — clean.
- `php -l dashboard/api/routes.php` — clean.
- `git diff --check -- dashboard/api/routes/panel.php dashboard/api/routes.php` — clean.
- Confirmed from migrations that membership is `asset.poolId`; no stored tier
  exists. The endpoint derives monitor=0, server/network/appliance=1, and
  client/unknown=3, with a pool inheriting its lowest member tier.
- Confirmed `hostCurrent.ifaces` carries `rxKbps`/`txKbps` JSON objects and
  `probeCurrent` carries RTT/loss fields used by the response.

UNVERIFIED:

- Live read-only SQL execution was not possible: this sandbox has no
  `SOLARI_DB_*` credentials/DSN configured for `Db.php`.
- Live authenticated response shape, `/api/summary` equivalence, and polling
  latency remain to be exercised on xenon.

DEVIATIONS:

- None. The synthetic pool-breach episode uses
  `0x80000000 | (crc32(breaching pool set) & 0x7fffffff)`, per the contract;
  normal critical-alert episodes use the maximum active, unsuppressed eventId.

NEXT:

- On xenon, authenticate as a viewer and exercise `/api/panel`; verify its
  read-only SQL against the deployed migration level and compare state/alerts
  with `/api/summary` over three same-instant samples.

## FIX ROUND 1

- Changed panel system state and `stateRoll` to use `node.state`, matching the
  dashboard summary rollup instead of deriving health from probe outcomes.
  `summary.php` has no maintenance-window state handling, so panel `maint`
  remains zero.
- Changed panel alert selection, severity totals, and `topAlert` input to use
  the exact dashboard summary predicate: `clearedAt IS NULL`.  Removed the
  panel-only acknowledgement, 60-minute, and maintenance suppression rules so
  its alert total agrees with the dashboard header.

## FIX ROUND 2

STATUS: COMPLETE

FIXED:

- P1/P2: critical episodes now use the minimum active critical `eventId`; pool
  breach episodes use one canonical CRC32 of sorted breaching pool IDs in both
  branches.
- P3: aggregate state initialisation includes `retired`; retired nodes are
  excluded from panel pools, systems, and five-state rollup, matching the
  summary endpoint's separate retired treatment.
- P4/P5: `stateRoll` is summed from the panel pool population and assets linked
  to an already-seen node are de-duplicated before telemetry/count aggregation.
- P6/P7/P9: alert counts and the displayed alert are bounded separate queries;
  standby class/role maps to tier 2; `dataStale` follows `MAX(hostCurrent.sampledAt)`
  older than 30 seconds.

SKIPPED:

- P8: rejected by Lead disposition; the tier-2/3 `down >= 5` floor remains.
- P10: maint remains reserved-zero per §9a.
- P11: nit not changed; mean load retains the selected panel population.
- P12: fixed incidentally; the now-total episode assignment no longer uses a
  dead coalesce.

VERIFIED:

- `php -l dashboard/api/routes/panel.php` — clean.
- `git diff --check -- dashboard/api/routes/panel.php` — clean.

UNVERIFIED:

- Live database execution, data-staleness timing, and endpoint/summary
  equivalence remain to be exercised on xenon.
