#!/usr/bin/env python3
"""sor_apply_ad — render-target applier: SoR host records → AD DNS (akoria.org).

Consumes `sor.events` (host-relevant tables), debounces, and reconciles the
AD-managed **akoria.org** forward zone against the SoR host set (view
`v_dns_hosts`) via `samba-tool dns`. Runs ON the DC (radium): local samba-tool,
AD admin creds from a root-only file — so the unit runs as root.

DELETION SCOPE (safety-critical): the applier keeps a state file of the host names
IT has added, and will ONLY delete a name that (a) it previously added AND (b) is
no longer in the SoR AND (c) is a plain host label (no leading '_' / dot) AND (d)
is not in the protect list (the DC's own name by default). It therefore can never
touch AD infrastructure records (SOA/NS/_msdcs/_tcp/gc/DomainDnsZones/…) or names
it didn't create. New names are added; changed IPs updated. Idempotent, fail-soft.

Config: sorsync.conf [amqp] + [sor] + [ad].
"""
import configparser
import json
import os
import re
import signal
import subprocess
import sys
import time

import pika
import pymysql

CONF = os.environ.get("SORSYNC_CONF",
                      os.path.join(os.path.dirname(os.path.abspath(__file__)), "sorsync.conf"))
HOST_KEYS = ["sor.entities.*", "sor.ip_addresses.*", "sor.interfaces.*"]
LABEL_RE = re.compile(r"^[a-z0-9][a-z0-9-]{0,62}$")
_run = True


def log(msg):
    print(f"{time.strftime('%Y-%m-%dT%H:%M:%SZ', time.gmtime())} sor_apply_ad: {msg}", flush=True)


def load_cfg():
    c = configparser.ConfigParser()
    if not c.read(CONF):
        log(f"FATAL: cannot read {CONF}"); sys.exit(1)
    return c


def _creds(c):
    pw = open(c.get("ad", "admin_pw_file", fallback="/root/.ad-admin-pw")).read().strip()
    return f"{c.get('ad', 'admin_user', fallback='administrator')}%{pw}"


def _sh(c, *args):
    """Run samba-tool dns <args>; return (rc, stdout). Never raises."""
    dc = c.get("ad", "dc", fallback="localhost")
    zone = c.get("ad", "zone", fallback="akoria.org")
    cmd = ["samba-tool", "dns", args[0], dc, zone, *args[1:], "-U", _creds(c)]
    try:
        r = subprocess.run(cmd, capture_output=True, text=True, timeout=30)
        return r.returncode, r.stdout + r.stderr
    except Exception as e:  # noqa: BLE001
        return 1, repr(e)


def ad_query_a(c, name):
    """Current A record IP for name in the zone, or None."""
    rc, out = _sh(c, "query", name, "A")
    if rc != 0:
        return None
    m = re.search(r"A:\s+([\d.]+)", out)
    return m.group(1) if m else None


def sor_hosts(c):
    db = pymysql.connect(
        host=c.get("sor", "host"), port=c.getint("sor", "port", fallback=3306),
        user=c.get("sor", "user"), password=c.get("sor", "password"),
        database=c.get("sor", "name", fallback="sor"),
        autocommit=True, connect_timeout=10, cursorclass=pymysql.cursors.DictCursor)
    try:
        with db.cursor() as cur:
            cur.execute("SELECT host, ip FROM v_dns_hosts WHERE ip IS NOT NULL")
            return {r["host"]: r["ip"] for r in cur.fetchall()
                    if LABEL_RE.match(r["host"] or "")}
    finally:
        db.close()


def _state_path(c):
    return c.get("ad", "state_file", fallback=os.path.join(os.path.dirname(CONF), "ad-managed.json"))


def load_state(c):
    try:
        return set(json.load(open(_state_path(c))))
    except Exception:  # noqa: BLE001
        return set()


def save_state(c, names):
    try:
        json.dump(sorted(names), open(_state_path(c), "w"))
    except Exception as e:  # noqa: BLE001
        log(f"state save failed: {e!r}")


def reconcile(c):
    protect = {p.strip() for p in c.get("ad", "protect", fallback="radium").split(",") if p.strip()}
    desired = sor_hosts(c)
    managed = load_state(c)
    added = updated = deleted = 0

    for name, ip in sorted(desired.items()):
        cur = ad_query_a(c, name)
        if cur is None:
            rc, out = _sh(c, "add", name, "A", ip)
            if rc == 0:
                added += 1
            else:
                log(f"add {name} {ip} failed: {out.strip()[:160]}")
        elif cur != ip:
            rc, out = _sh(c, "update", name, "A", cur, ip)
            if rc == 0:
                updated += 1
            else:
                log(f"update {name} {cur}->{ip} failed: {out.strip()[:160]}")

    # delete only names WE added that are gone from the SoR (never infra/DC)
    for name in sorted(managed - set(desired)):
        if name in protect or not LABEL_RE.match(name):
            continue
        cur = ad_query_a(c, name)
        if cur is not None:
            rc, out = _sh(c, "delete", name, "A", cur)
            if rc == 0:
                deleted += 1
            else:
                log(f"delete {name} {cur} failed: {out.strip()[:160]}")

    save_state(c, set(desired))
    if added or updated or deleted:
        log(f"reconciled akoria.org: +{added} ~{updated} -{deleted}")
    else:
        log("reconciled akoria.org: no change")


def run():
    c = load_cfg()
    exchange = c.get("amqp", "exchange", fallback="sor.events")
    debounce = c.getfloat("ad", "debounce_sec", fallback=4.0)
    reconnect = c.getfloat("amqp", "reconnect_delay_sec", fallback=5.0)
    # Periodic self-heal: reconcile even when idle, so out-of-band drift (a manual
    # AD edit, events missed during a long outage) self-corrects. This is the job
    # the retired hourly akoria-dns-reconcile.timer used to do. 0 disables it.
    reconcile_interval = c.getfloat("ad", "reconcile_interval_sec", fallback=3600.0)
    queue = "sor.apply.ad"

    log("initial reconcile on startup")
    try:
        reconcile(c)
    except Exception as e:  # noqa: BLE001
        log(f"initial reconcile error: {e!r}")
    last_reconcile = time.monotonic()

    while _run:
        try:
            conn = pika.BlockingConnection(pika.URLParameters(c.get("amqp", "url")))
            ch = conn.channel()
            ch.exchange_declare(exchange=exchange, exchange_type="topic", durable=True)
            ch.queue_declare(queue=queue, durable=True)
            for k in HOST_KEYS:
                ch.queue_bind(exchange=exchange, queue=queue, routing_key=k)
            log(f"consuming {queue} (debounce={debounce}s)")
            dirty = False
            for method, _props, _body in ch.consume(queue, inactivity_timeout=debounce):
                if not _run:
                    break
                if method is None:
                    due = reconcile_interval > 0 and (time.monotonic() - last_reconcile) >= reconcile_interval
                    if dirty or due:
                        reconcile(c)
                        last_reconcile = time.monotonic()
                        dirty = False
                    continue
                dirty = True
                ch.basic_ack(method.delivery_tag)
            ch.cancel(); conn.close()
        except Exception as e:  # noqa: BLE001
            log(f"error: {e!r}; reconnecting in {reconnect}s")
            time.sleep(reconnect)


def _stop(*_):
    global _run
    _run = False
    log("shutting down")


if __name__ == "__main__":
    signal.signal(signal.SIGTERM, _stop)
    signal.signal(signal.SIGINT, _stop)
    run()
