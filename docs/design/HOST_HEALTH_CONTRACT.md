# Interface Contract v1 — Host-Health Monitoring + HTTP Probe

*Shared design contract for the overnight remediation. Every work unit implements
against THIS document so parallel units integrate cleanly. Do not deviate from the
field names, TLV ranges, metric names, or file ownership below without updating this
file. Motivated by the 2026-07-07 cesium incident: a USB-attached git disk fell off
the bus, its btrfs went emergency-read-only, and Forgejo 500'd for ~26h while the
monitor stayed green — because the `:3000` check was bare `tcp` (socket stayed open)
and no host-local health signal (fs-readonly / missing device / SMART) is collected.*

## File ownership (no two units edit the same file)

| Unit | Owns (edits) | Reads (no edit) |
|---|---|---|
| **A · HTTP probe** | `src/monitor/probeNet.c`, `src/monitor/monitorConfig.c`, `src/server/serverAssets.c`, `src/server/serverProvision.c` (dispatch-spec only) | this contract |
| **B · client health collector** | `src/client/platOS.h`, `src/client/plat/platLinux.c`, `src/client/clientCollect.c`, `include/solari/solariMsg.h`, `lib/solari/solariMsg.c`, `src/client/CMakeLists.txt` | this contract |
| **C · server health ingest** | `src/server/serverIngest.c`, `src/server/serverDb.c`, `src/server/serverAlert.c`, `db/migrations/010_host_health.sql` | `include/solari/solariMsg.h` (B's struct) |
| **D · alert→MQ bridge** | `deploy/alertbridge/**` (new) | DB schema, `deploy/notify/notify.conf` |
| **E · backups** | `deploy/backups/**` (new) | — |

Cross-unit dependency: **C consumes the struct B defines** in `solariMsg.h`. Both
follow §1 field names exactly, so B's header edit + C's `serverIngest.c` edit merge
without conflict (different files). A owns `serverAssets.c` alone; C never touches it.

## §1 — Host-health wire struct (Unit B defines; Unit C consumes)

Add to `include/solari/solariMsg.h`, embedded as `solariHostHealth health;` in
`solariClientReport` (after existing fields):

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

**TLV codes:** allocate a new contiguous block in the client-report TLV enum.
**(As implemented: `include/solari/solariTlv.h`, block `0x1010`–`0x1014`.)** Encode scalars +
the four strings. **Backward compatibility is mandatory:** `solariMsgParseClientReport`
MUST tolerate a report with no health TLVs (older client) → zeroed `health`. Add a
round-trip unit test in `tests/unit/` mirroring `test_msg.c`.

## §2 — PAL collectors (Unit B) — `platOS.h` + `plat/platLinux.c`

Follow existing PAL conventions (see `platDiskFree`, `platListenServices`). Each returns
0 on success, fills the relevant `solariHostHealth` fields; **defensive**: a missing tool
or unreadable source yields a *zeroed* signal, never a crash or a false positive.

```c
int platHostHealth(solariHostHealth *h);   /* umbrella; calls the below */
```
- **fs-readonly:** parse `/proc/mounts`; flag mounts whose options contain `ro` that are
  NOT read-only-by-design (exclude `squashfs`, `iso9660`, `overlay` lowerdirs, cdrom).
  A NORMALLY-rw fs now `ro` is the signal (this is exactly what btrfs emergency-RO looked like).
- **block-device presence:** baseline the set of `/sys/block/*` (minus loop/zram/sr) at
  first run into `/var/lib/solari/blockdev.baseline`; on later runs flag any baseline
  device now absent. (Catches "sde has gone missing".)
- **SMART:** if `smartctl` present, `smartctl -H <dev>` per physical disk; parse the
  "SMART overall-health self-assessment test result"; count != PASSED. No smartctl → 0.
- **failed units:** if `systemctl` present, `systemctl --failed --no-legend --plain` →
  count + names. No systemd → 0.
- **dmesg-critical:** read new kernel messages since a stored cursor
  (`/var/lib/solari/dmesg.cursor`, seq or timestamp) via `journalctl -k` (preferred) or
  `/dev/kmsg`; count lines matching (case-insensitive) `btrfs.*(error|critical|emergency)`,
  `I/O error`, `EXT4-fs error`, `ata[0-9]+.*(error|reset)`, `Out of memory`, `md/raid.*fail`.
  Store the newest match in `dmesgCritSample`. Never re-count old lines.

Call `platHostHealth()` from `clientCollectReport()` (`clientCollect.c`) and assign into
the report's `health` field each cycle.

## §3 — Server ingest + storage (Unit C)

- `serverIngestClientReport()` (`serverIngest.c`): after parse, pass `report.health` into
  the DB write.
- `serverDbWriteClientReport()` (`serverDb.c`): persist health scalars + detail strings.
- **Migration `db/migrations/010_host_health.sql`:** add columns to `hostCurrent`:
  `fsReadonlyCount TINYINT, blockDevMissing TINYINT, smartFailCount TINYINT,
  failedUnitCount SMALLINT, dmesgCritCount SMALLINT, fsReadonlyList VARCHAR(256),
  smartFailList VARCHAR(256), failedUnitList VARCHAR(256), dmesgCritSample VARCHAR(256)`
  (all NULL/0 default so existing rows are valid). Add counters to `hostHistory` if cheap.
  Mirror the additions into `db/schema.sql`.

## §4 — Alert rules (Unit C) — `serverAlert.c`

Expose these host-scope metrics in `alertClientMetric()`: `health.fsReadonly`,
`health.blockDevMissing`, `health.smartFail`, `health.failedUnits`, `health.dmesgCrit`.
Seed `alertRule` rows (in migration 010 or a seed file) — host scope, immediate sustain:

| metric | op | threshold | severity |
|---|---|---|---|
| health.fsReadonly | > | 0 | **crit** |
| health.blockDevMissing | > | 0 | **crit** |
| health.smartFail | > | 0 | **crit** |
| health.failedUnits | > | 0 | warn |
| health.dmesgCrit | > | 0 | warn |

The existing `alertApply()` → `serverDbWriteAlertEvent()` path then records these with no
new emission code. Detail text (which mount / which device) goes in the event detail.

## §5 — HTTP probe completion (Unit A)

- `appCheckHttp()` (`probeNet.c`): accept an optional expected status in `appArg` as
  `path|status` (e.g. `/health/ready|200` or `/|2xx`). Default (no `|`) = 2xx-class OK.
  Exact code or `Nxx` class match → `PROBE_OK`; mismatch → `PROBE_PROTO_ERR`. A bare-`tcp`
  server returning 500 MUST now fail (this is the core fix).
- `monitorParseTarget()` (`monitorConfig.c`): widen the HTTP grammar to carry the
  `path|status` in `appArg`.
- `probeTypeForPort()` (`serverAssets.c`): map **80→http, 443→http, 3000→http, 8080→http,
  9000→http**; others unchanged. Return a sensible default `checkArg` per port
  (3000→`/|2xx`, 9000→`/health/ready|200`, else `/|2xx`).
- `assetAddService()` (`serverAssets.c`): pass the real `checkArg` (not NULL) to
  `serverDbUpsertProbeTarget`, and include the path in the dispatch-spec string so the
  monitor's HTTP branch receives it. Coordinate the spec format with
  `serverProvisionDispatchTarget()` (`serverProvision.c`): spec becomes
  `http:ip:port:path|status : label` (parser already splits on the first three colons for
  host:port, so keep path as the 4th field).

## §6 — Alert→MQ bridge + dead-man's-switch (Unit D) — `deploy/alertbridge/`

Python ops glue (accepted per CLAUDE.md), fail-soft, systemd unit on xenon.
- **Bridge:** poll `alertEvent` (MariaDB, xenon `solarinet` DB) for rows newer than a
  persisted checkpoint; for each, publish to RabbitMQ exchange `notify.events` with
  routing key `notify.<severity>` and a `{title, body, severity, source}` payload matching
  `deploy/notify/README.md`. crit/warn already route to Apple in `notify.conf`. Idempotent
  (checkpoint by max event id), never double-send.
- **Dead-man's-switch:** every cycle, query the last-report age per known node
  (`hostCurrent`) and per monitor/probe; if a node that was reporting goes silent beyond
  `3× sampleInterval` (min 120s), emit a synthetic `crit` "node X stopped reporting"
  (once, until it returns). This is the "monitor the monitor" guard — had it existed,
  cesium's silence itself would have alerted.
- Config `deploy/alertbridge/alertbridge.conf` (gitignored): DB creds from `run/db.env`,
  RabbitMQ url reused from `deploy/notify/notify.conf`. venv with pymysql + pika.

## §7 — Backups (Unit E) — `deploy/backups/`

- `solari-backup.sh` + systemd service+timer (nightly). On **cesium**: `mysqldump` the
  `sor` DB and (once Forgejo is relocated) `forgejo dump`, gz, to `/data/backups/`
  (sdb1 — a DIFFERENT spindle from the sda2 datadir). On **xenon**: `mysqldump` the
  `solarinet` DB to a durable path. Retention: keep 14 daily, prune older. Log a one-line
  summary; on failure publish a `warn` to `notify.events` (reuse the bridge's publisher or
  a tiny inline pika call). Idempotent, safe to run by hand.

## Integration & test (owner: main session)

1. Merge unit branches (clean by ownership), rebuild **`build-io`** (deployed tree), run
   `tests/unit`.
2. Apply migration 010 to `solarinet` (xenon) + seed alert rules.
3. Deploy: client→cesium, server+monitor→xenon; restart units.
4. **e2e synthetic-fault test:** simulate a read-only mount / a failed systemd unit on a
   test host → confirm the signal reaches `hostCurrent`, fires an `alertEvent`, the bridge
   publishes to `notify.events`, and the Apple channel delivers. Then adopt Forgejo `:3000`
   as `http` and confirm a 500 now trips the probe.
5. Review (Opus + Codex) on the integrated diff; apply fixes; checkpoint goals.
