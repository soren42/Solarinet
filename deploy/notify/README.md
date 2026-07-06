# SolariNet notification service (notifyd)

An MQ-driven dispatcher: it consumes alert messages from RabbitMQ and fans
them out to pluggable senders (SMS, push, a durable log/journald record),
without callers ever needing to know which transport is actually in use.
Callers (monitoring, the dashboard, anything else on SolariNet) only need to
publish a JSON message to the `notify.events` exchange — notifyd does the
rest.

This service is deliberately separate from the C `solariServer`/`solariMonitor`
core (same pattern as the PHP dashboard tier): it's ops/infra glue around a
message broker, written in Python for the same reason the dashboard is PHP —
it isn't the monitoring agent/protocol itself.

## Architecture

```
   publishers                 RabbitMQ (benzene, vhost "solari")           notifyd
 (monitoring, dashboard,   ->  topic exchange "notify.events"   ->   durable queue "notify.dispatch"
  scripts, ...)                routing keys: notify.sms,              bound with notify.#
                                notify.push, notify.log, ...
                                                                            |
                                                                            v
                                                                  Dispatcher.dispatch()
                                                                    picks sender(s) by
                                                                    message.channel or
                                                                    severity routing
                                                                            |
                                                     +----------------------+----------------------+
                                                     v                      v                       v
                                                senders/log.py        senders/sms_tachyon.py   senders/push.py
                                               (always works,         (Tachyon SBC, SSH,        (stub — future
                                                file + journald)       method TBD)               dashboard/Tab5 push)
```

notifyd owns and declares a **durable queue** (`notify.dispatch` by default)
bound to the pre-existing **durable topic exchange `notify.events`** with
one or more binding keys (default `notify.#`, i.e. everything). It does not
create the exchange (that's provisioned once on the broker); it does
passively rely on it existing.

Each consumed message is decoded, validated minimally, and handed to
`Dispatcher.dispatch()`, which:
1. Picks which sender(s) should handle it — see "Routing" below.
2. Resolves recipients (`to` on the message, else the sender's
   `default_*_to` from config).
3. Calls each sender's `send(message, cfg)`. A sender should never raise for
   an expected failure (e.g. Tachyon unreachable) — it logs and returns
   `False`, and the message is still ack'd (the `log` sender's write is the
   durable record of "this alert happened", separate from "delivery to a
   human succeeded").

## Message contract

Published as JSON to exchange `notify.events`, any routing key under
`notify.*` (e.g. `notify.sms`, `notify.push`, `notify.log`, or just
`notify.events` itself — notifyd's default binding `notify.#` catches all of
these).

```json
{
  "severity": "crit",
  "title": "xenon: disk /var 94% full",
  "body": "Free space on /var dropped below 10% at 2026-07-06T05:58:00Z.",
  "channel": "auto",
  "to": ["+15555550100"],
  "source": "monitoring",
  "ts": 1783483080
}
```

| field      | type              | required | notes |
|------------|-------------------|----------|-------|
| `severity` | `"crit"\|"warn"\|"info"` | no (default `info`) | drives severity-based routing (see below) |
| `title`    | string            | one of title/body required | short summary |
| `body`     | string            | one of title/body required | full text |
| `channel`  | `"sms"\|"push"\|"log"\|"auto"` | no (default `auto`) | `auto` = route by severity; an explicit sender name pins delivery to just that sender |
| `to`       | array of strings  | no | recipients (phone numbers for sms, device/topic ids for push); falls back to `notify.conf`'s `[defaults]` if omitted |
| `source`   | string            | no | free-form provenance tag (`monitoring`, `dashboard`, ...), included in the log record |
| `ts`       | number (unix epoch, seconds) | no | defaults to receipt time if omitted |

Only `title` or `body` is strictly required for notifyd to accept the
message — everything else has a sane default. Unparsable JSON or a message
missing both `title` and `body` is logged and dropped (ack'd, not requeued)
rather than crash-looping the consumer.

## Routing

`notify.conf`'s `[routing]` section maps severity to an ordered list of
sender names, e.g.:

```ini
[routing]
crit = log,sms,push
warn = log,push
info = log
```

- If a message sets an explicit `channel` other than `auto` (and that sender
  is enabled), it's used directly, bypassing severity routing.
- Otherwise notifyd looks up `[routing]` by the message's `severity`.
- If neither applies, `[defaults] default_channel` is used.
- The `log` sender is normally included in every row — it's the durable
  record regardless of whether SMS/push actually reached anyone.

## Sender interface (how to add a sender)

A sender is a Python module under `senders/` exposing one function:

```python
def send(message: dict, cfg) -> bool:
    """Attempt delivery. Return True on success, False on (soft) failure.

    message is the parsed JSON payload plus two dispatcher-added keys:
      - message["_to"]: list[str], resolved recipients for this sender
      - message["_routing_key"]: the AMQP routing key it arrived on

    Must not raise for expected failure modes (unreachable host, missing
    config, etc.) -- catch, log, return False. Let unexpected exceptions
    propagate (the dispatcher logs and treats them as a failed send).
    """
```

To add a new sender:
1. Create `senders/<yourname>.py` with a `send(message, cfg)` function.
2. Register it in `senders/__init__.py`'s `REGISTRY` dict (short config
   name -> module path).
3. Add it to `notify.conf`'s `[senders] enabled=` and reference it from
   `[routing]` and/or a message's `channel`.
4. Give it its own `[sender.<yourname>]` config section if it needs
   settings — notifyd passes that section (or `{}` if absent) as `cfg`.

Existing senders:
- **`senders/log.py`** (`log`) — always works. Writes a JSON line per
  notification to a file (`[sender.log] path=`, default
  `/var/log/solarinet/notify.log`) and to the Python logger (captured by
  journald under systemd). This is the fallback of last resort and should
  stay enabled in every routing row.
- **`senders/sms_tachyon.py`** (`sms`) — see below.
- **`senders/push.py`** (`push`) — interface-only stub; always returns
  `False` until a real transport exists. Present so `enabled=push` and
  `channel: "push"` don't error out before the dashboard/Tab5 push surface
  is built.

## Open question: Tachyon SMS

The SMS sender talks to a Tachyon SBC (`tachyon.akoria.net` / `10.6.6.10`,
a Particle Tachyon board with a cellular modem), but **the actual mechanism
for sending an SMS from it is not yet decided**. `senders/sms_tachyon.py`
implements a small seam, `send_sms(number, text, cfg)`, and two candidate
backends behind `[sender.sms] method=`, both currently stubbed to log what
they *would* run and return `False`:

- **`mmcli_ssh`** (default) — assumes ModemManager manages the modem on
  Tachyon; would SSH in and run `mmcli --messaging --create-sms` /
  `mmcli --sms=N --send`. Needs confirming `ModemManager` is actually
  running there and the modem shows up in `mmcli -L`.
- **`at_ssh`** — assumes no ModemManager; would SSH in and drive the modem's
  AT-command serial port directly (`AT+CMGF=1`, `AT+CMGS="<number>"`,
  body + Ctrl-Z on `[sender.sms] at_device=`, default `/dev/ttyUSB2`). Needs
  confirming which `/dev/ttyUSB*` port actually accepts AT commands (modems
  usually expose several ports; only one takes `AT+CMGS`) and the baud rate.
- **Not yet stubbed**: Particle's own cloud API, if Tachyon is claimed to a
  Particle account with a device-side function that can trigger an SMS via
  its cellular stack. Worth checking before investing in the raw-serial
  path, but needs Particle account/device-ID info this repo doesn't have.

**To finish it**: SSH to Tachyon, run `systemctl status ModemManager` and
`mmcli -L` (or `ls /dev/ttyUSB*` + probe with `screen`/`minicom`) to see
which interface is actually present, then replace the corresponding
`_send_via_mmcli_ssh` / `_send_via_at_ssh` body in
`deploy/notify/senders/sms_tachyon.py` with the real subprocess call (a
`_run_ssh()` helper already exists for this) and fill in the matching
`[sender.sms]` fields in `notify.conf`.

Until then, an SMS-routed message still gets recorded by the `log` sender
(if `log` is in that severity's routing list) and the sms sender's failure
is a normal logged `False`, not a crash.

## Config

Copy `notify.conf.example` to `notify.conf` (gitignored — holds the AMQP
password) and edit:

```sh
cp deploy/notify/notify.conf.example deploy/notify/notify.conf
$EDITOR deploy/notify/notify.conf
chmod 600 deploy/notify/notify.conf
```

See the comments in `notify.conf.example` for every key: `[amqp]` (broker
URL, exchange/queue/binding names), `[defaults]` (fallback recipients and
channel), `[routing]` (severity -> sender list), `[senders]` (which sender
modules to load), and one `[sender.<name>]` section per sender.

## Deploy

```sh
# dependencies (pika isn't in the stdlib; PEP 668 blocks a bare pip install
# on Debian's system Python, so use a venv):
python3 -m venv deploy/notify/.venv
deploy/notify/.venv/bin/pip install pika

cp deploy/notify/notify.conf.example deploy/notify/notify.conf
$EDITOR deploy/notify/notify.conf   # AMQP password, recipients, Tachyon host
chmod 600 deploy/notify/notify.conf

sudo cp deploy/notify/notify.service /etc/systemd/system/
sudo systemctl daemon-reload
sudo systemctl enable --now notify
journalctl -u notify -f
```

The `notify.service` unit as committed assumes the repo lives at
`/home/jason/Code/Solarinet` and runs as user `jason` (same convention as
`deploy/dashboard/solarinet-server.service`) — adjust `User`/`Group`/paths
if deploying elsewhere (e.g. xenon).

## Verification performed

- `python3 -m py_compile` clean on `notifyd.py` and all of `senders/*.py`.
- Live smoke test against the real broker
  (`amqp://solari:***@10.5.2.50:5672/solari`, vhost `solari`): started
  `notifyd.py`, confirmed it connected and bound `notify.dispatch` to
  `notify.events` with key `notify.#`; published a test message via pika to
  routing key `notify.log`; notifyd consumed it, routed it to the `log`
  sender (channel `auto` + default `[routing] info = log`), and the sender
  wrote the expected JSON line to its log file and to the process log. No
  errors, clean ack.
- SMS sender was **not** smoke-tested end-to-end (by design — see "Open
  question" above); its stub paths were exercised via manual review, not a
  live SSH call to Tachyon.
