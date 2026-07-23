# SolariNet alert bridge + dead-man's-switch (alertbridge)

Implements `HOST_HEALTH_CONTRACT.md` §6 (Unit D). A single fail-soft Python
daemon on **xenon** that closes two gaps exposed by the 2026-07-07 cesium
incident (a disk fell off the bus, Forgejo 500'd for ~26h, and nothing paged):

1. **Bridge** — every `alertEvent` the C server writes now becomes a real
   notification. It polls `alertEvent` and republishes each new row to the
   `notify.events` broker exchange, where `notifyd` fans it out (crit/warn
   already route to the Apple/iMessage channel in `notify.conf`).
2. **Dead-man's-switch** — the guard that was missing entirely. It watches how
   long each node has gone without reporting and pages **crit** if a node that
   *was* reporting goes silent. Had this existed, cesium's silence itself would
   have alerted, regardless of any per-service probe.

Like `deploy/notify` and `deploy/sorsync`, this is ops/infra glue around the
message broker — Python, not part of the C `solariServer`/`solariMonitor` core
(accepted per `CLAUDE.md`).

## How it works

```
                 xenon                                     benzene
  +--------------------------------+            +---------------------------+
  |  solarinet DB (MariaDB)        |            |  RabbitMQ (vhost solari)  |
  |    alertEvent  ---------------\ |            |                           |
  |    hostCurrent  ----------\    \|  publish   |  topic exchange           |
  |    probeCurrent ----\      \    +----------> |  notify.events            |
  +---------------------|-------|---+  notify.<sev>          |               |
                        |       |                            v               |
                 alertbridge.py |                    (notifyd consumes,      |
                 (bridge + DMS) |                     routes crit/warn ->    |
                                |                     Apple/SMS/log)         |
                                +--------------------------------------------+
```

### Bridge (`alertEvent` -> `notify.events`)

- Polls `SELECT ... FROM alertEvent WHERE eventId > checkpoint ORDER BY eventId`
  (joined to `node` for the fqdn and `alertRule` for the metric/op/threshold).
- Publishes each row as a notify-contract message
  (`deploy/notify/README.md`): `{title, body, severity, source}`, routing key
  `notify.<severity>`, persistent (`delivery_mode=2`).
- **Idempotent / at-least-once:** the checkpoint (max published `eventId`) lives
  in the state file and advances *one event at a time, only after that event's
  publish returns*. A crash between publish and checkpoint re-sends that one
  event rather than dropping it; an already-checkpointed event is never
  re-published.

`alertEvent` rows are firings (each `INSERT` is one alert firing; clears are
`clearedAt` *updates*, not new rows), so keying on new `eventId`s means every
new fired alert is published exactly once in the normal path.

### Dead-man's-switch

Each cycle it computes the freshest last-report age per node:

- **host clients** from `hostCurrent.sampledAt`,
- **monitor vantages** from `MAX(probeCurrent.sampledAt)` per `monitorNode`.

A node whose age exceeds **`max(120s, 3 x sample_interval_sec)`** (the client
report cadence; `client.conf` `sampleIntervalSec` defaults to 15 -> 120s floor)
gets **one** synthetic `crit` `node <fqdn> stopped reporting`. The node is added
to the state file's `silent` set so it is not paged again while still down; when
it reports inside the threshold again it is removed (re-armed) and an `info`
recovery is emitted. Nodes that have never reported (no row) can't trip it.

## Config

Copy the sample and edit:

```sh
cp deploy/alertbridge/alertbridge.conf.example deploy/alertbridge/alertbridge.conf
chmod 600 deploy/alertbridge/alertbridge.conf
$EDITOR deploy/alertbridge/alertbridge.conf
```

- `[db]` — the local xenon `solarinet` MariaDB. Leave `password` blank to take
  it from `$SOLARI_DB_PASS` (the systemd unit sources `run/db.env`, same as
  `deploy/systemd/solarinet-snmp.service`).
- `[amqp]` — reuse the **exact** `url` from `deploy/notify/notify.conf` so
  alerts land on the same broker `notifyd` drains; `exchange = notify.events`.
- `[bridge]` — `poll_interval_sec`, `batch`, `source` tag.
- `[deadman]` — `enabled`, and `sample_interval_sec` (match `client.conf`).
- `[state]` — path to the JSON state file (checkpoint + silent set); defaults to
  `alertbridge.state.json` next to the config (gitignored).

`alertbridge.conf`, `alertbridge.state.json`, and `.venv/` are gitignored.

## Deploy (on xenon)

```sh
# deps (pika/pymysql aren't stdlib; PEP 668 blocks a bare pip on Debian):
python3 -m venv deploy/alertbridge/.venv
deploy/alertbridge/.venv/bin/pip install -r deploy/alertbridge/requirements.txt

cp deploy/alertbridge/alertbridge.conf.example deploy/alertbridge/alertbridge.conf
$EDITOR deploy/alertbridge/alertbridge.conf     # AMQP url, DB, thresholds
chmod 600 deploy/alertbridge/alertbridge.conf

# run/db.env must exist and export SOLARI_DB_PASS (see deploy/dashboard/README.md):
#   printf 'export SOLARI_DB_PASS=…\n' > run/db.env && chmod 600 run/db.env

sudo cp deploy/alertbridge/alertbridge.service /etc/systemd/system/
sudo systemctl daemon-reload
sudo systemctl enable --now alertbridge
journalctl -u alertbridge -f
```

The unit assumes the repo at `/home/jason/Code/Solarinet` running as `jason`
(same convention as the other SolariNet units) — adjust `User`/`Group`/paths if
deploying elsewhere.

## Test

- `python3 -m py_compile deploy/alertbridge/alertbridge.py` — syntax gate.
- **Bridge:** `INSERT` a row into `alertEvent` (severity `crit`, a real
  `nodeId`, some `detail`); within `poll_interval_sec` the bridge publishes
  `notify.crit` and `notifyd` delivers it. Confirm the checkpoint in the state
  file advanced; re-inserting nothing produces no re-sends.
- **Dead-man's-switch:** with a node's `hostCurrent.sampledAt` older than the
  threshold (or stop its agent), expect one `crit` `node <fqdn> stopped
  reporting`; let it report again and expect the `info` recovery + re-arm. The
  loop tolerates the DB or broker being down — it logs and reconnects, never
  crashing.
