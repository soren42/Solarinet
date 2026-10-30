#!/usr/bin/env python3
"""sor_emitd — SoR change emitter (transactional-outbox → MQ).

Polls the `sor_outbox` table (populated by CDC triggers in the same transaction
as each change) and publishes every new row to the `sor.events` topic exchange
with routing key `sor.<table>.<action>`. A checkpoint in `sor_sync_state`
(`emit_checkpoint`) advances only after broker confirmation of every mandatory-
routed persistent message. Broker outages, negative acknowledgments, and missing
routes leave the checkpoint behind for retry. Consumers must separately
acknowledge only after applying.

Tables listed in `[amqp] optional_tables` have CDC triggers but no consumer
yet; their events are still confirmed by the broker but published without the
mandatory flag, so an unbound route cannot stall the whole outbox.

Config: sorsync.conf  ([sor] db creds, [amqp] url/exchange/poll).
"""
import configparser
import json
import os
import signal
import sys
import time

import pika
import pymysql
from db_config import database_options

CONF = os.environ.get("SORSYNC_CONF",
                      os.path.join(os.path.dirname(os.path.abspath(__file__)), "sorsync.conf"))
_run = True
# CDC tables with no applier bound today (migrations 024, 026). Override in
# config. test_every_cdc_table_is_bound_or_optional keeps this list complete.
DEFAULT_OPTIONAL_TABLES = ("sources,functions,entity_functions,function_incompatibility,"
                           "vlans,dhcp_ranges,external_refs")


def log(msg):
    print(f"{time.strftime('%Y-%m-%dT%H:%M:%SZ', time.gmtime())} sor_emitd: {msg}", flush=True)


def load_cfg():
    c = configparser.ConfigParser()
    if not c.read(CONF):
        log(f"FATAL: cannot read {CONF}")
        sys.exit(1)
    return c


def db_connect(c):
    db = database_options(c, "sor_emit")
    return pymysql.connect(
        host=db["host"], port=db["port"], user=db["user"],
        password=db["password"], database=db["name"],
        autocommit=True, connect_timeout=10,
        cursorclass=pymysql.cursors.DictCursor,
    )


def get_checkpoint(db):
    with db.cursor() as cur:
        cur.execute("SELECT v FROM sor_sync_state WHERE k='emit_checkpoint'")
        row = cur.fetchone()
        return int(row["v"]) if row else 0


def set_checkpoint(db, v):
    with db.cursor() as cur:
        # A missing state row must not turn a successful UPDATE of zero rows
        # into a false checkpoint. Preserve monotonicity if two emitters race.
        cur.execute(
            "INSERT INTO sor_sync_state (k,v) VALUES ('emit_checkpoint',%s) "
            "ON DUPLICATE KEY UPDATE v=GREATEST(v,VALUES(v))", (v,))


def optional_tables(c):
    raw = c.get("amqp", "optional_tables", fallback=DEFAULT_OPTIONAL_TABLES)
    return frozenset(t.strip() for t in raw.split(",") if t.strip())


def publish_batch(ch, db, rows, exchange, checkpoint, optional=frozenset()):
    """Confirm every route before moving the durable outbox checkpoint.

    Any failure raises and leaves the DB checkpoint unchanged. Already-confirmed
    messages may be repeated after reconnect; consumers render idempotently.
    """
    confirmed = checkpoint
    for r in rows:
        rk = f"sor.{r['table_name']}.{r['action']}"
        body = json.dumps({
            "outbox_id": r["id"], "table": r["table_name"],
            "row_id": r["row_id"], "action": r["action"], "ts": r["ts"],
        })
        # BlockingChannel in confirm mode returns None on success and raises
        # UnroutableError or NackError on failure. A missing bound queue is a
        # failure, including during startup before consumers declare theirs,
        # except for tables explicitly declared consumerless.
        ch.basic_publish(
            exchange=exchange, routing_key=rk, body=body,
            mandatory=r["table_name"] not in optional,
            properties=pika.BasicProperties(delivery_mode=2, content_type="application/json"))
        confirmed = r["id"]
    if rows:
        set_checkpoint(db, confirmed)
    return confirmed


def run():
    c = load_cfg()
    exchange = c.get("amqp", "exchange", fallback="sor.events")
    poll = c.getfloat("amqp", "poll_interval_sec", fallback=2.0)
    batch = c.getint("amqp", "batch", fallback=500)
    reconnect = c.getfloat("amqp", "reconnect_delay_sec", fallback=5.0)
    optional = optional_tables(c)

    while _run:
        db = mq = ch = None
        try:
            db = db_connect(c)
            params = pika.URLParameters(c.get("amqp", "url"))
            mq = pika.BlockingConnection(params)
            ch = mq.channel()
            ch.exchange_declare(exchange=exchange, exchange_type="topic", durable=True)
            ch.confirm_delivery()
            cp = get_checkpoint(db)
            log(f"connected; checkpoint={cp} exchange={exchange}")
            while _run:
                with db.cursor() as cur:
                    cur.execute(
                        "SELECT id, table_name, row_id, action, "
                        "UNIX_TIMESTAMP(changed_at) ts FROM sor_outbox "
                        "WHERE id > %s ORDER BY id LIMIT %s", (cp, batch))
                    rows = cur.fetchall()
                if rows:
                    cp = publish_batch(ch, db, rows, exchange, cp, optional)
                    log(f"published {len(rows)} event(s); checkpoint={cp}")
                else:
                    mq.process_data_events(0)  # keep heartbeats alive
                    time.sleep(poll)
        except Exception as e:  # noqa: BLE001 — daemon: log + reconnect
            log(f"error: {e!r}; reconnecting in {reconnect}s")
            time.sleep(reconnect)
        finally:
            for x in (ch, mq, db):
                try:
                    x and x.close()
                except Exception:
                    pass


def _stop(*_):
    global _run
    _run = False
    log("shutting down")


if __name__ == "__main__":
    signal.signal(signal.SIGTERM, _stop)
    signal.signal(signal.SIGINT, _stop)
    run()
