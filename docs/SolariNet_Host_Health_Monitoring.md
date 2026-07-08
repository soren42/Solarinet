# SolariNet — Host-Health Monitoring + HTTP-Status-Aware Probe

*Built against `docs/design/HOST_HEALTH_CONTRACT.md` v1, motivated by the 2026-07-07 cesium
incident: a USB-attached git disk fell off the bus, its btrfs went emergency-read-only, and
Forgejo 500'd for ~26h while the monitor stayed green — because the `:3000` check was bare
`tcp` (socket stayed open) and no host-local health signal (fs-readonly / missing device /
SMART) was being collected. This closes both gaps: host-local fault signals, and an
HTTP probe that actually checks the response, not just that a socket answers.*

## What's collected

Five signals, all defensive (a missing tool or unreadable source yields a zeroed signal —
never a crash, never a false positive):

| signal | source | flags when |
|---|---|---|
| **fs-readonly** | `/proc/mounts` | a normally-rw mount now shows `ro` (excludes squashfs/iso9660/overlay-lowerdir/cdrom by design) — this is exactly what btrfs emergency-RO looked like on cesium |
| **block-device-missing** | `/sys/block/*` vs. a baseline snapshot | a device present at baseline (`/var/lib/solari/blockdev.baseline`) is absent now (loop/zram/sr excluded from the baseline) |
| **SMART** | `smartctl -H <dev>` per physical disk | overall-health self-assessment != `PASSED`; 0 if `smartctl` isn't installed |
| **failed systemd units** | `systemctl --failed --no-legend --plain` | count + unit names; 0 on a non-systemd host |
| **dmesg-critical** | `journalctl -k` (preferred) or `/dev/kmsg`, since a stored cursor | new lines matching `btrfs.*(error\|critical\|emergency)`, `I/O error`, `EXT4-fs error`, `ata[0-9]+.*(error\|reset)`, `Out of memory`, `md/raid.*fail` (case-insensitive); never re-counts old lines |

## Data flow

```
 client host                              cesium/xenon (server)                 xenon
┌───────────────────┐  platHostHealth()  ┌──────────────────────┐  alertApply() ┌─────────────┐
│ platLinux.c        │ ───────────────▶  │ solariHostHealth h    │ ───────────▶ │ alertRule    │
│  /proc/mounts       │                   │ (5 scalars + 4 csv    │  breach eval │  (host scope)│
│  /sys/block/*       │                   │  detail strings)      │              └──────┬───────┘
│  smartctl -H        │                                                                 │ sustain window
│  systemctl --failed │                                                                 ▼
│  journalctl -k       │                                                        alertEvent (crit/warn)
└─────────┬───────────┘                                                                 │
          │ clientCollectReport()                                                       │ PUB / DB write
          ▼                                                                             ▼
 solariClientReport.health          serverIngestClientReport()             deploy/alertbridge/
  (embedded solariHostHealth)  ───▶  serverDb: hostCurrent + hostHistory ──▶ poll alertEvent (checkpoint
          │  TLV, 0x40.. block                                                by max id, no double-send)
          ▼                                                                             │
  solariMsgParseClientReport()                                                          │ publish
   (older client, no health TLV                                                         ▼
    → zeroed health; b/c-safe)                                            RabbitMQ exchange notify.events
                                                                            routing key notify.<severity>
                                                                                          │
                                                                                          ▼
                                                                                     notifyd (deploy/notify)
                                                                            severity routing: crit → log,sms,push
                                                                                          │
                                                                                          ▼
                                                                              senders/apple.py → SSH to a
                                                                              Mac relay → osascript iMessage
                                                                              → iCloud → iPhone/iPad/Watch/Mac
```

The **HTTP status-aware probe** is a parallel, independent input into the same `alertRule` /
`alertEvent` / bridge / notifyd / Apple pipeline (it's a `probe`-scope signal, not `host`-scope
like the five above) — see "HTTP status-aware probe" below.

## Wire format (contract §1)

`solariHostHealth`, embedded as `health` in `solariClientReport` (`include/solari/solariMsg.h`):

```c
typedef struct {
    uint8_t  fsReadonlyCount;       /* mounts unexpectedly read-only */
    uint8_t  blockDevMissing;       /* expected block devices now absent */
    uint8_t  smartFailCount;        /* disks with SMART health != PASSED */
    uint16_t failedUnitCount;       /* systemd failed units */
    uint16_t dmesgCritCount;        /* new critical kernel lines since last report */
    char     fsReadonlyList[256];   /* csv of RO mountpoints, "" if none */
    char     smartFailList[256];    /* csv "sde:FAILING", failing disks only */
    char     failedUnitList[256];   /* csv of failed unit names (truncate to fit) */
    char     dmesgCritSample[256];  /* most recent critical kernel line */
} solariHostHealth;
```

TLV codes for the five scalars + four strings occupy a contiguous block starting at `0x40`
(`TLV_CR_HEALTH_FS=0x40`, …) in the client-report TLV enum in `lib/solari/solariMsg.c`.
Backward compatibility is a hard requirement: `solariMsgParseClientReport()` tolerates a
report carrying no health TLVs at all (an older, un-upgraded client) and leaves `health`
zeroed — a zeroed struct never breaches an alert rule, so mixed-version fleets degrade
safely (old clients simply don't emit these five signals yet, they don't false-alarm).

Server side: `serverIngestClientReport()` passes `report.health` to `serverDbWriteClientReport()`,
which persists the scalars + detail strings onto `hostCurrent` (columns added in migration
`db/migrations/010_host_health.sql`, all `NULL`/0-default so pre-existing rows stay valid) and
mirrors counters into `hostHistory` where cheap. `serverAlert.c` exposes the five as host-scope
metrics — `health.fsReadonly`, `health.blockDevMissing`, `health.smartFail`,
`health.failedUnits`, `health.dmesgCrit` — so the existing `alertApply()` →
`serverDbWriteAlertEvent()` path fires on them with **no new emission code**.

## HTTP status-aware probe

The core fix for the cesium incident: a bare-`tcp` check only proves a socket answers — it
does not prove the application behind it is healthy. `appCheckHttp()` (`probeNet.c`) now
accepts an optional expected status via `appArg`, syntax `path|status`:

- `/health/ready|200` — exact status code match
- `/|2xx` — status-class match (any `2xx`)
- no `|` at all — default, same as `|2xx`

A `200` or `2xx` mismatch (e.g. a `500`) now returns `PROBE_PROTO_ERR` instead of `PROBE_OK` —
**a bare-tcp server returning 500 now fails the check**, which is exactly what should have
happened to cesium's Forgejo for 26 hours.

`monitorParseTarget()` (`monitorConfig.c`) widens the HTTP grammar to carry `path|status` in
`appArg`. `probeTypeForPort()` (`serverAssets.c`) maps well-known ports to `http` with a
sensible default `checkArg`:

| port | probeType | default checkArg |
|---|---|---|
| 80, 443, 8080 | http | `/\|2xx` |
| 3000 | http | `/\|2xx` |
| 9000 | http | `/health/ready\|200` |
| other | unchanged | — |

`assetAddService()` passes the real `checkArg` through to `serverDbUpsertProbeTarget` and
into the dispatch-spec string the monitor receives: `http:ip:port:path|status : label` (the
existing parser splits on the first three colons for host:port; `path|status` is the 4th
field).

### Adopting a service with an expected status

To move an existing asset from bare-`tcp` to a status-checked `http` probe:

1. In the dashboard/server config for the service's asset, set `probeType=http` and
   `checkArg=<path>|<status-or-class>`.
   - Forgejo on cesium, port `3000`: `checkArg=/|2xx` (any 2xx from `/` is healthy — Forgejo's
     landing page; a 500 there is the cesium failure mode this whole effort exists to catch).
   - Keycloak, port `9000`: `checkArg=/health/ready|200` (Keycloak's dedicated management
     health port — `/health/ready` returns a JSON body and `200` only when the realm store is
     actually reachable, not just that the HTTP listener is up).
2. Re-run (or wait for) `serverProvisionDispatchTarget()` to regenerate the dispatch spec for
   that target; confirm the spec string in the monitor's config shows the `path|status` 4th
   field.
3. Restart/reload the monitor process so it re-reads dispatch specs.
4. Verify: hit the endpoint with a bad status manually (or use the synthetic-fault procedure
   below) and confirm the probe result flips to `PROBE_PROTO_ERR`, not `PROBE_OK`.

## Dead-man's-switch

"Monitor the monitor." Every bridge cycle (`deploy/alertbridge/`), the last-report age is
checked per known node (`hostCurrent`) and per monitor/probe target. If a node that was
reporting goes silent for longer than `3 × sampleInterval` (floor 120s), the bridge emits a
synthetic `crit` **"node X stopped reporting"** — once, until the node reports again. This is
the guard that would have caught cesium's silence *as an event* even if every other signal
had somehow stayed quiet: an unreachable/crashed/powered-off client is itself an incident,
not an absence of one.

## Alert severities & thresholds (contract §4)

Host-scope, immediate sustain (no `forSeconds` delay — these fire on first breach,
`SCP_FLAG_URGENT`-style):

| metric | op | threshold | severity |
|---|---|---|---|
| `health.fsReadonly` | > | 0 | **crit** |
| `health.blockDevMissing` | > | 0 | **crit** |
| `health.smartFail` | > | 0 | **crit** |
| `health.failedUnits` | > | 0 | warn |
| `health.dmesgCrit` | > | 0 | warn |

Rationale: the three `crit` rows are all "the storage layer is actively lying to you or
about to fail" — exactly the class of fault that hid for 26 hours. `failedUnits` and
`dmesgCrit` are `warn` because they're common noisier signals (a single unit flapping during
a deploy, a benign kernel warning) that deserve attention but not a page — they still route
through the `notifyd` `warn` row (`log,push` per `notify.conf`'s default `[routing]`, minus
`sms`), whereas `crit` reaches `sms`/`push`/Apple.

---

# RUNBOOK

## Reading host-health state

- **Dashboard / DB**: `hostCurrent` carries the live scalars (`fsReadonlyCount`,
  `blockDevMissing`, `smartFailCount`, `failedUnitCount`, `dmesgCritCount`) and the detail
  strings (`fsReadonlyList`, `smartFailList`, `failedUnitList`, `dmesgCritSample`) for every
  reporting node. A quick manual check:
  ```sql
  SELECT nodeId, fsReadonlyCount, blockDevMissing, smartFailCount,
         failedUnitCount, dmesgCritCount, fsReadonlyList, smartFailList,
         failedUnitList, dmesgCritSample
  FROM hostCurrent
  WHERE fsReadonlyCount > 0 OR blockDevMissing > 0 OR smartFailCount > 0
     OR failedUnitCount > 0 OR dmesgCritCount > 0;
  ```
- **Open alerts**: `alertEvent` rows with `scope='host'` and a `metric` starting `health.` are
  currently-breaching or historical fires; an open row (no `clearedAt`) is still active.
- **Notification history**: the `log` sender always writes a durable JSON line per
  notification (`/var/log/solarinet/notify.log` by default) regardless of whether SMS/push/Apple
  actually reached anyone — this is the ground truth for "did this fire", independent of
  delivery.

## What each alert means and first response

| alert | meaning | first response |
|---|---|---|
| `health.fsReadonly` (crit) | A normally-writable filesystem remounted read-only — usually a kernel-detected corruption/I-O panic (this is exactly the cesium btrfs failure mode) | SSH to the host, `mount \| grep ro,`; check `dmesg`/`journalctl -k` for the triggering I/O error; do **not** blindly remount rw — first confirm the underlying disk/bus is sane (see block-device-missing and SMART below), then `mount -o remount,rw <mnt>` once safe |
| `health.blockDevMissing` (crit) | A disk present at baseline is gone from `/sys/block` — a cable/bus/enclosure/power fault, or a drive that died | Check physical connection (this was a USB disk falling off the bus on cesium); `lsusb`/`lsblk`/`dmesg` for a disconnect event; if the device is intentionally being replaced, re-baseline (see below) rather than treating it as a fault |
| `health.smartFail` (crit) | `smartctl -H` reports overall health != `PASSED` on a physical disk | Treat as imminent hardware failure; check `smartFailList` for which device; schedule replacement, verify the most recent backup for any data on that spindle (see `deploy/backups/`) |
| `health.failedUnits` (warn) | One or more systemd units are in `failed` state | `systemctl --failed` on the host; `journalctl -u <unit>` for the failure; restart if transient, else investigate — a flapping unit during a deploy is expected noise, a unit stuck failed for hours is not |
| `health.dmesgCrit` (warn) | A new kernel line matched a known critical pattern (I/O error, EXT4/btrfs error, ATA reset, OOM, md/raid fail) since the last cursor | Check `dmesgCritSample` for the actual line; correlate with `fsReadonlyCount`/`blockDevMissing` — dmesg-critical is often the earliest signal of the same underlying event that later trips those two |

## Adding or adjusting an alertRule

Rules live in `alertRule` (seeded by migration `010_host_health.sql` or a follow-on seed file).
To add a new host-health rule or change a threshold:

1. Insert/update a row: `scope` (`host` for these signals), `metric` (must match a name
   `alertClientMetric()` in `serverAlert.c` recognizes), `op` (`gt`/`lt`/`eq`/`transition`),
   `threshold`, `severity` (`info`/`warn`/`crit`), `forSeconds` (0 for immediate-fire signals
   like these; set the appropriate flag if the rule should sustain a breach for N seconds
   before firing instead), `enabled`.
2. If the metric doesn't exist yet, extend `alertClientMetric()` in `serverAlert.c` to expose
   it (only needed for genuinely new signals — the five in this doc are already wired).
3. Restart/reload the server process so `alertRule` is re-read (or confirm the server
   re-polls it live, per current implementation).
4. Verify with the synthetic-fault procedure below, or by directly forcing the underlying
   condition (e.g. `systemctl start some-oneshot-that-fails.service` to test
   `health.failedUnits`).

## Baselining block devices

`platHostHealth()`'s block-device check compares live `/sys/block/*` (excluding
`loop*`/`zram*`/`sr*`) against a stored baseline at `/var/lib/solari/blockdev.baseline`. The
baseline is written automatically on first run on a host that has none. To re-baseline
deliberately (e.g. after adding, removing, or replacing a disk on purpose):

```sh
# on the client host, monitoring service stopped or between cycles:
rm /var/lib/solari/blockdev.baseline
# next collection cycle recreates it from the current /sys/block/* set
```

Do this **after** confirming the current device set is correct — re-baselining silently
adopts whatever's currently present as "expected", including an accidentally-missing disk.
If in doubt, `lsblk` first and compare against known-good inventory before deleting the
baseline file.

## Testing the pipeline end-to-end with a synthetic fault

This is the procedure from contract §"Integration & test" — run it once per environment
(new host, or after any change to the collector/ingest/bridge/notify chain) to prove the
whole path is live, not just individually-unit-tested:

1. **Trigger a synthetic host-health fault** on a test host, e.g. one of:
   - Read-only mount: `mount -o remount,ro /some/test/mount` (use a scratch mount, not `/`).
   - Failed unit: `systemd-run --unit=solari-test-fail --property=ExecStart=/bin/false` (or
     any oneshot guaranteed to exit non-zero) — leaves a `failed` unit behind.
   - dmesg-critical: harder to force safely; prefer the two above for a routine test, or
     inject a matching line via a controlled test harness if one exists rather than
     provoking real disk/I/O errors.
2. Wait one client report cycle; confirm the signal lands in `hostCurrent` for that node
   (query above).
3. Confirm an `alertEvent` row appears (`scope='host'`, matching `metric`, `severity` per the
   §4 table).
4. Confirm `deploy/alertbridge/` picked it up and published to RabbitMQ exchange
   `notify.events` with routing key `notify.<severity>` — check the bridge's checkpoint
   advanced and its log shows a publish, or tail the exchange directly.
5. Confirm `notifyd` consumed it, routed by severity (`crit` → `log,sms,push` by default,
   which includes the `apple` sender if enabled), and the durable `log` sender wrote a line —
   this is enough to prove the software path even if the Apple/SMS legs are hardware-gated
   in a given environment.
6. Confirm the **Apple channel** delivered (iMessage arriving via the Mac relay), if that
   leg is deployed in the environment under test.
7. Clean up: remount rw / clear the test failed unit (`systemctl reset-failed
   solari-test-fail`); confirm the corresponding `alertEvent` clears on the next report once
   the underlying condition is gone.
8. **HTTP-probe leg**: point a test `http` probe at a service you control, make it return a
   non-matching status (stop the service, or point at a `404`/`500` path), and confirm the
   probe result flips from `PROBE_OK` to `PROBE_PROTO_ERR` — then restore it and confirm it
   flips back. Finally, adopt Forgejo's real `:3000` target as `http`/`/|2xx` (or Keycloak's
   `:9000`/`/health/ready|200`) per "Adopting a service" above, and confirm a deliberately
   returned 500 on that real service now trips the probe — this is the actual regression
   test for the incident that motivated all of this.

## Related

- Interface contract: `docs/design/HOST_HEALTH_CONTRACT.md`
- notifyd (dispatch, routing, Apple sender): `deploy/notify/README.md`
- Alert-rule evaluation engine: `src/server/serverAlert.c`
