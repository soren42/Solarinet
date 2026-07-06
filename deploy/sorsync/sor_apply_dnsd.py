#!/usr/bin/env python3
"""sor_apply_dnsd — forward applier: SoR change → DNS.

Consumes `sor.events` (DNS-relevant tables), debounces a burst of changes, then
re-renders BIND zones FROM the SoR (gen-zones.py --source sor), and — only if the
rendered files differ from what BIND is serving — deploys them and `rndc reload`s.
Idempotent: it renders the whole view from current SoR state, so at-least-once
delivery and missed events self-heal (an initial render runs on startup too).

Note: gen-zones uses a date-based serial (YYYYMMDDnn); the primary (xenon) serves
fresh data on reload regardless, but same-day multi-change secondary AXFR needs a
monotonic serial — tracked as a follow-up.

Config: sorsync.conf [amqp] + [dns].
"""
import configparser
import filecmp
import glob
import os
import shutil
import signal
import subprocess
import sys
import time

import pika

CONF = os.environ.get("SORSYNC_CONF",
                      os.path.join(os.path.dirname(os.path.abspath(__file__)), "sorsync.conf"))
DNS_KEYS = ["sor.entities.*", "sor.ip_addresses.*", "sor.interfaces.*",
            "sor.dns_records.*", "sor.dns_zones.*", "sor.networks.*", "sor.subnets.*"]
_run = True


def log(msg):
    print(f"{time.strftime('%Y-%m-%dT%H:%M:%SZ', time.gmtime())} sor_apply_dns: {msg}", flush=True)


def load_cfg():
    c = configparser.ConfigParser()
    if not c.read(CONF):
        log(f"FATAL: cannot read {CONF}"); sys.exit(1)
    return c


def render(c):
    """Render zones from the SoR into zone_src_dir. Returns True on success."""
    env = dict(os.environ)
    env["NETDB_SOURCE"] = "sor"
    env["SOR_DB_PASSWORD"] = c.get("dns", "sor_db_password")
    py = c.get("dns", "render_python")
    gen = c.get("dns", "gen_zones")
    r = subprocess.run([py, gen, "--source", "sor"], env=env,
                       capture_output=True, text=True, timeout=120)
    if r.returncode != 0:
        log(f"render failed: {r.stderr.strip()[:300]}")
        return False
    return True


def deploy_if_changed(c):
    """Diff rendered files vs live BIND; copy changed + reload. Returns #changed."""
    src = c.get("dns", "zone_src_dir")
    dst = c.get("dns", "zone_dst_dir")
    named_dst = c.get("dns", "named_conf_akoria")
    reload_cmd = c.get("dns", "reload_cmd").split()

    changed = []
    # zone files: src/db.* -> dst/db.*
    for f in sorted(glob.glob(os.path.join(src, "db.*"))):
        live = os.path.join(dst, os.path.basename(f))
        if not (os.path.exists(live) and filecmp.cmp(f, live, shallow=False)):
            changed.append((f, live))
    # named.conf.akoria -> named_dst
    nc = os.path.join(src, "named.conf.akoria")
    if os.path.exists(nc) and not (os.path.exists(named_dst) and filecmp.cmp(nc, named_dst, shallow=False)):
        changed.append((nc, named_dst))

    if not changed:
        return 0
    for f, live in changed:
        subprocess.run(["sudo", "cp", f, live], check=True, timeout=30)
    subprocess.run(reload_cmd, check=True, timeout=30)
    log(f"deployed {len(changed)} changed zone file(s) + reloaded: "
        f"{', '.join(os.path.basename(l) for _, l in changed)}")
    return len(changed)


def apply(c):
    if render(c):
        n = deploy_if_changed(c)
        if n == 0:
            log("rendered; no zone change (no reload)")


def run():
    c = load_cfg()
    exchange = c.get("amqp", "exchange", fallback="sor.events")
    debounce = c.getfloat("dns", "debounce_sec", fallback=4.0)
    reconnect = c.getfloat("amqp", "reconnect_delay_sec", fallback=5.0)
    queue = "sor.apply.dns"

    log("initial render on startup")
    try:
        apply(c)
    except Exception as e:  # noqa: BLE001
        log(f"initial render error: {e!r}")

    while _run:
        try:
            conn = pika.BlockingConnection(pika.URLParameters(c.get("amqp", "url")))
            ch = conn.channel()
            ch.exchange_declare(exchange=exchange, exchange_type="topic", durable=True)
            ch.queue_declare(queue=queue, durable=True)
            for k in DNS_KEYS:
                ch.queue_bind(exchange=exchange, queue=queue, routing_key=k)
            log(f"consuming {queue} (debounce={debounce}s)")
            dirty = False
            for method, _props, _body in ch.consume(queue, inactivity_timeout=debounce):
                if not _run:
                    break
                if method is None:            # debounce window elapsed
                    if dirty:
                        apply(c)
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
