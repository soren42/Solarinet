#!/usr/bin/env python3
"""sor_apply_pihole — render-target applier: SoR host records → Pi-hole local DNS.

Consumes `sor.events` (host-relevant tables), debounces, renders the SoR host set
(view `v_dns_hosts`) as a Pi-hole `custom.list` (one `IP fqdn` line per host), and
pushes it to each configured Pi-hole over ssh — writing + `reloaddns` ONLY when the
file actually changes. Gives the Pi-holes authoritative local A records straight
from the SoR (belt-and-suspenders with the BIND conditional-forward; local records
win, so a host still resolves at the Pi-hole even if BIND is unreachable).

Idempotent (renders the whole file from current SoR state). Each target is tried
even if another fails; the broker event remains unacked until all configured
targets succeed. Runs on xenon,
which has key-based ssh + passwordless sudo on the Pi-holes.

Config: sorsync.conf [amqp] + [sor] + [pihole] and per-target [pihole.<name>].
"""
import configparser
import hashlib
import os
import shlex
import signal
import subprocess
import sys
import time

import pika
import pymysql
from delivery import consume_after_apply

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
    """Diff + write custom.list; return changed, unchanged, or failed."""
    sec = f"pihole.{name}"
    ssh_host = c.get(sec, "ssh")
    path = c.get(sec, "custom_list")
    reload_cmd = c.get(sec, "reload")
    try:
        cur = subprocess.run(ssh_args(ssh_host) + [f"sudo cat {shlex.quote(path)} 2>/dev/null"],
                             capture_output=True, text=True, timeout=20)
        if cur.returncode == 0 and cur.stdout == content:
            # A previous write may have succeeded while reload failed. Re-run
            # reload before acknowledging this event; it is idempotent.
            reload_result = subprocess.run(ssh_args(ssh_host) + [reload_cmd],
                                           capture_output=True, text=True, timeout=30)
            if reload_result.returncode != 0:
                log(f"{name}: reload failed: {reload_result.stderr.strip()[:200]}")
                return "failed"
            return "unchanged"
        # Verify bytes before an atomic rename. A failed upload leaves the old
        # custom.list intact; a failed reload is retried on the next delivery.
        temp_path = path + ".sor-new"
        digest = hashlib.sha256(content.encode()).hexdigest()
        write = (f"sudo tee {shlex.quote(temp_path)} >/dev/null && "
                 f"printf '%s  %s\\n' {shlex.quote(digest)} {shlex.quote(temp_path)} "
                 f"| sudo sha256sum -c --status - && "
                 f"sudo mv -f {shlex.quote(temp_path)} {shlex.quote(path)} && "
                 f"{reload_cmd}")
        r = subprocess.run(ssh_args(ssh_host) + [write], input=content,
                           capture_output=True, text=True, timeout=45)
        if r.returncode != 0:
            log(f"{name}: apply failed: {r.stderr.strip()[:200]}")
            return "failed"
        log(f"{name}: pushed {content.count(chr(10))} records + reloaded")
        return "changed"
    except Exception as e:  # noqa: BLE001
        log(f"{name}: soft-fail: {e!r}")
        return "failed"


def apply_all(c):
    targets = [t.strip() for t in c.get("pihole", "targets").split(",") if t.strip()]
    if not targets:
        raise ValueError("no Pi-hole targets configured")
    content = render(c)
    changed = failed = 0
    for name in targets:
        outcome = apply_target(c, name, content)
        if outcome == "changed":
            changed += 1
        elif outcome == "failed":
            failed += 1
    if changed == 0 and failed == 0:
        log("rendered; no Pi-hole change")
    if failed:
        log(f"{failed} Pi-hole target(s) failed; delivery remains unacknowledged")
    return failed == 0


def run():
    c = load_cfg()
    exchange = c.get("amqp", "exchange", fallback="sor.events")
    debounce = c.getfloat("pihole", "debounce_sec", fallback=4.0)
    reconcile = c.getfloat("pihole", "reconcile_interval_sec", fallback=3600.0)
    reconnect = c.getfloat("amqp", "reconnect_delay_sec", fallback=5.0)
    queue = "sor.apply.pihole"

    while _run:
        conn = None
        try:
            conn = pika.BlockingConnection(pika.URLParameters(c.get("amqp", "url")))
            ch = conn.channel()
            ch.exchange_declare(exchange=exchange, exchange_type="topic", durable=True)
            ch.queue_declare(queue=queue, durable=True)
            for k in HOST_KEYS:
                ch.queue_bind(exchange=exchange, queue=queue, routing_key=k)
            log(f"consuming {queue} (debounce={debounce}s, reconcile={reconcile}s)")
            consume_after_apply(ch, queue, debounce, reconcile,
                                lambda: apply_all(c), lambda: _run)
        except Exception as e:  # noqa: BLE001
            log(f"error: {e!r}; reconnecting in {reconnect}s")
            if _run:
                time.sleep(reconnect)
        finally:
            if conn is not None:
                try:
                    conn.close()  # requeues unacked deliveries
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
