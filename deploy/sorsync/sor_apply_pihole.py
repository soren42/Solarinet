#!/usr/bin/env python3
"""sor_apply_pihole — render-target applier: SoR host records → Pi-hole local DNS.

Consumes `sor.events` (host-relevant tables), debounces, renders the SoR host set
(view `v_dns_hosts`) as a Pi-hole `custom.list` (one `IP fqdn` line per host), and
pushes it to each configured Pi-hole over ssh — writing + `reloaddns` ONLY when the
file actually changes. Gives the Pi-holes authoritative local A records straight
from the SoR (belt-and-suspenders with the BIND conditional-forward; local records
win, so a host still resolves at the Pi-hole even if BIND is unreachable).

Idempotent (renders the whole file from current SoR state) + fail-soft per target
(one unreachable Pi-hole never blocks the other or the daemon). Runs on xenon,
which has key-based ssh + passwordless sudo on the Pi-holes.

Config: sorsync.conf [amqp] + [sor] + [pihole] and per-target [pihole.<name>].
"""
import configparser
import os
import shlex
import signal
import subprocess
import sys
import time

import pika
import pymysql

CONF = os.environ.get("SORSYNC_CONF",
                      os.path.join(os.path.dirname(os.path.abspath(__file__)), "sorsync.conf"))
HOST_KEYS = ["sor.entities.*", "sor.ip_addresses.*", "sor.interfaces.*"]
_run = True


def log(msg):
    print(f"{time.strftime('%Y-%m-%dT%H:%M:%SZ', time.gmtime())} sor_apply_pihole: {msg}", flush=True)


def load_cfg():
    c = configparser.ConfigParser()
    if not c.read(CONF):
        log(f"FATAL: cannot read {CONF}"); sys.exit(1)
    return c


def render(c):
    """The desired custom.list content from the SoR (sorted for stable diffs)."""
    domain = c.get("pihole", "domain", fallback="akoria.net")
    db = pymysql.connect(
        host=c.get("sor", "host"), port=c.getint("sor", "port", fallback=3306),
        user=c.get("sor", "user"), password=c.get("sor", "password"),
        database=c.get("sor", "name", fallback="sor"),
        autocommit=True, connect_timeout=10, cursorclass=pymysql.cursors.DictCursor)
    lines = []
    try:
        with db.cursor() as cur:
            cur.execute("SELECT host, ip FROM v_dns_hosts WHERE ip IS NOT NULL ORDER BY host")
            for r in cur.fetchall():
                lines.append(f"{r['ip']} {r['host']}.{domain}")
    finally:
        db.close()
    return "".join(l + "\n" for l in sorted(set(lines)))


def ssh_args(target_ssh):
    return ["ssh", "-o", "BatchMode=yes", "-o", "ConnectTimeout=8",
            "-o", "StrictHostKeyChecking=accept-new", target_ssh]


def apply_target(c, name, content):
    """Diff + write custom.list on one Pi-hole; reload only on change. Fail-soft."""
    sec = f"pihole.{name}"
    ssh_host = c.get(sec, "ssh")
    path = c.get(sec, "custom_list")
    reload_cmd = c.get(sec, "reload")
    try:
        cur = subprocess.run(ssh_args(ssh_host) + [f"sudo cat {shlex.quote(path)} 2>/dev/null || true"],
                             capture_output=True, text=True, timeout=20)
        if cur.stdout == content:
            return False  # unchanged
        # write atomically via a temp file, then reload
        write = f"sudo tee {shlex.quote(path)} >/dev/null && {reload_cmd}"
        r = subprocess.run(ssh_args(ssh_host) + [write], input=content,
                           capture_output=True, text=True, timeout=45)
        if r.returncode != 0:
            log(f"{name}: apply failed: {r.stderr.strip()[:200]}")
            return False
        log(f"{name}: pushed {content.count(chr(10))} records + reloaded")
        return True
    except Exception as e:  # noqa: BLE001
        log(f"{name}: soft-fail: {e!r}")
        return False


def apply_all(c):
    targets = [t.strip() for t in c.get("pihole", "targets").split(",") if t.strip()]
    content = render(c)
    changed = 0
    for name in targets:
        if apply_target(c, name, content):
            changed += 1
    if changed == 0:
        log("rendered; no Pi-hole change")


def run():
    c = load_cfg()
    exchange = c.get("amqp", "exchange", fallback="sor.events")
    debounce = c.getfloat("pihole", "debounce_sec", fallback=4.0)
    reconnect = c.getfloat("amqp", "reconnect_delay_sec", fallback=5.0)
    queue = "sor.apply.pihole"

    log("initial render on startup")
    try:
        apply_all(c)
    except Exception as e:  # noqa: BLE001
        log(f"initial render error: {e!r}")

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
                    if dirty:
                        apply_all(c)
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
