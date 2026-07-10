#!/usr/bin/env python3
"""sor_apply_dnsd — forward applier: SoR change → DNS.

Consumes `sor.events` (DNS-relevant tables), debounces a burst of changes, then
re-renders BIND zones FROM the SoR (gen-zones.py --source sor), and — only if the
rendered files differ from what BIND is serving — deploys them and `rndc reload`s.
Idempotent: it renders the whole view from current SoR state, so at-least-once
delivery and missed events self-heal (an initial render runs on startup too).

It also renders single-label "bare-name" master zones (gen-bare.py) so dumb SMB /
legacy clients that query a bare hostname (e.g. NAS-X, no akoria.net suffix, DHCP
search domain ignored) still resolve — the behaviour Pi-hole's dnsmasq provided.
Bare zones are master copies on BOTH resolvers, so changed ones are pushed to
steel and its container is SIGHUP-reloaded (xenon is served locally).

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
    # bare-name single-label zones (dumb SMB clients) — derived from the forward
    # zone we just rendered. Non-fatal: main zones still deploy if this fails.
    gen_bare = c.get("dns", "gen_bare", fallback="")
    if gen_bare:
        src = c.get("dns", "zone_src_dir")
        rb = subprocess.run([py, gen_bare, os.path.join(src, "db.akoria.net"), src],
                            capture_output=True, text=True, timeout=60)
        if rb.returncode != 0:
            log(f"bare-zone render failed (main zones unaffected): {rb.stderr.strip()[:200]}")
    return True


def deploy_if_changed(c):
    """Diff rendered files vs live BIND; copy changed + reload.

    Returns the list of changed destination basenames (empty if nothing changed),
    so the caller can decide whether the bare-name set also needs pushing to steel.
    """
    src = c.get("dns", "zone_src_dir")
    dst = c.get("dns", "zone_dst_dir")
    named_dst = c.get("dns", "named_conf_akoria")
    named_bare_dst = c.get("dns", "named_conf_bare", fallback="")
    reload_cmd = c.get("dns", "reload_cmd").split()

    changed = []
    # zone files: src/db.* -> dst/db.*  (includes db.bare-* single-label zones)
    for f in sorted(glob.glob(os.path.join(src, "db.*"))):
        live = os.path.join(dst, os.path.basename(f))
        if not (os.path.exists(live) and filecmp.cmp(f, live, shallow=False)):
            changed.append((f, live))
    # named.conf.akoria -> named_dst ; named.conf.bare -> named_bare_dst
    for name, dstpath in (("named.conf.akoria", named_dst), ("named.conf.bare", named_bare_dst)):
        if not dstpath:
            continue
        nc = os.path.join(src, name)
        if os.path.exists(nc) and not (os.path.exists(dstpath) and filecmp.cmp(nc, dstpath, shallow=False)):
            changed.append((nc, dstpath))

    if not changed:
        return []
    for f, live in changed:
        subprocess.run(["sudo", "cp", f, live], check=True, timeout=30)
    subprocess.run(reload_cmd, check=True, timeout=30)
    names = [os.path.basename(l) for _, l in changed]
    log(f"deployed {len(names)} changed file(s) on xenon + reloaded: {', '.join(names)}")
    return names


def push_bare_to_steel(c, changed_names):
    """Bare-name zones are master copies on steel too (not AXFR'd), so any changed
    db.bare-*/named.conf.bare must be copied over and the container SIGHUP-reloaded."""
    bare = [n for n in changed_names if n.startswith("db.bare-") or n == "named.conf.bare"]
    steel = c.get("dns", "steel_host", fallback="")
    if not bare or not steel:
        return
    src = c.get("dns", "zone_src_dir")
    cfgdir = c.get("dns", "steel_config_dir")          # mounts at /etc/bind in the container
    reload_cmd = c.get("dns", "steel_reload_cmd").split()
    ssh = ["ssh", "-o", "BatchMode=yes", "-o", "ConnectTimeout=8", steel]
    try:
        for name in bare:
            localf = os.path.join(src, name)
            remote = (f"{steel}:{cfgdir}/named.conf.bare" if name == "named.conf.bare"
                      else f"{steel}:{cfgdir}/zones/{name}")
            subprocess.run(["scp", "-o", "BatchMode=yes", "-o", "ConnectTimeout=8", localf, remote],
                           check=True, timeout=30)
        subprocess.run(ssh + reload_cmd, check=True, timeout=30)
        log(f"pushed {len(bare)} bare-zone file(s) to steel + reloaded")
    except subprocess.SubprocessError as e:
        log(f"steel bare-zone push failed (xenon still authoritative): {e!r}")


def apply(c):
    if render(c):
        changed = deploy_if_changed(c)
        if not changed:
            log("rendered; no zone change (no reload)")
        else:
            push_bare_to_steel(c, changed)


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
