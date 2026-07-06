#!/usr/bin/env python3
"""sor_emitd — SoR change emitter (transactional-outbox → MQ).

Polls the `sor_outbox` table (populated by CDC triggers in the same transaction
as each change) and publishes every new row to the `sor.events` topic exchange
with routing key `sor.<table>.<action>`. A checkpoint in `sor_sync_state`
(`emit_checkpoint`) is advanced only after a successful publish, so delivery is
at-least-once — downstream appliers (e.g. sor_apply_dns) must be idempotent, and
they are (they re-render the whole view from current SoR state).

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

CONF = os.environ.get("SORSYNC_CONF",
                      os.path.join(os.path.dirname(os.path.abspath(__file__)), "sorsync.conf"))
_run = True


def log(msg):
    print(f"{time.strftime('%Y-%m-%dT%H:%M:%SZ', time.gmtime())} sor_emitd: {msg}", flush=True)


def load_cfg():
    c = configparser.ConfigParser()
    if not c.read(CONF):
        log(f"FATAL: cannot read {CONF}")
        sys.exit(1)
    return c


def db_connect(c):
    return pymysql.connect(
        host=c.get("sor", "host", fallback="10.1.0.200"),
        port=c.getint("sor", "port", fallback=3306),
        user=c.get("sor", "user", fallback="solari"),
        password=c.get("sor", "password"),
        database=c.get("sor", "name", fallback="sor"),
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
        cur.execute("UPDATE sor_sync_state SET v=%s WHERE k='emit_checkpoint'", (v,))


def run():
    c = load_cfg()
    exchange = c.get("amqp", "exchange", fallback="sor.events")
    poll = c.getfloat("amqp", "poll_interval_sec", fallback=2.0)
    batch = c.getint("amqp", "batch", fallback=500)
    reconnect = c.getfloat("amqp", "reconnect_delay_sec", fallback=5.0)

    while _run:
        db = mq = ch = None
        try:
            db = db_connect(c)
            params = pika.URLParameters(c.get("amqp", "url"))
            mq = pika.BlockingConnection(params)
            ch = mq.channel()
            ch.exchange_declare(exchange=exchange, exchange_type="topic", durable=True)
            cp = get_checkpoint(db)
            log(f"connected; checkpoint={cp} exchange={exchange}")
            while _run:
                with db.cursor() as cur:
                    cur.execute(
                        "SELECT id, table_name, row_id, action, "
                        "UNIX_TIMESTAMP(changed_at) ts FROM sor_outbox "
                        "WHERE id > %s ORDER BY id LIMIT %s", (cp, batch))
                    rows = cur.fetchall()
                for r in rows:
                    rk = f"sor.{r['table_name']}.{r['action']}"
                    body = json.dumps({
                        "outbox_id": r["id"], "table": r["table_name"],
                        "row_id": r["row_id"], "action": r["action"], "ts": r["ts"],
                    })
                    ch.basic_publish(
                        exchange=exchange, routing_key=rk, body=body,
                        properties=pika.BasicProperties(delivery_mode=2, content_type="application/json"))
                    cp = r["id"]
                if rows:
                    set_checkpoint(db, cp)
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
