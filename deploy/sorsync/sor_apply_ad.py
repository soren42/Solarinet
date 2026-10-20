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
import tempfile
import time

import pika
import pymysql
from delivery import consume_after_apply

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
        if "WERR_DNS_ERROR_NAME_DOES_NOT_EXIST" in out:
            return None
        raise RuntimeError(f"AD DNS query failed for {name}: {out.strip()[:160]}")
    m = re.search(r"A:\s+([\d.]+)", out)
    if not m:
        raise RuntimeError(f"AD DNS query returned no A record for {name}: {out.strip()[:160]}")
    return m.group(1)


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
        with open(_state_path(c), encoding="utf-8") as state_file:
            state = json.load(state_file)
    except FileNotFoundError:
        return set()
    # Older code stored every desired SoR name, including pre-existing AD
    # records it did not create. Those names cannot safely be deletion-eligible.
    if isinstance(state, list):
        log("legacy AD managed-name state is untrusted; retaining records but "
            "resetting deletion ownership")
        return set()
    if not isinstance(state, dict) or state.get("version") != 2 or not isinstance(state.get("managed"), list):
        raise ValueError("invalid AD managed-name state format")
    names = state["managed"]
    if any(not isinstance(name, str) or not LABEL_RE.match(name) for name in names):
        raise ValueError("invalid AD managed-name entry")
    return set(names)


def save_state(c, names):
    path = _state_path(c)
    directory = os.path.dirname(path) or "."
    fd, temp_path = tempfile.mkstemp(prefix=".ad-managed-", dir=directory)
    try:
        with os.fdopen(fd, "w", encoding="utf-8") as state_file:
            json.dump({"version": 2, "managed": sorted(names)}, state_file)
            state_file.flush()
            os.fsync(state_file.fileno())
        os.replace(temp_path, path)
        dir_fd = os.open(directory, os.O_RDONLY | os.O_DIRECTORY)
        try:
            os.fsync(dir_fd)
        finally:
            os.close(dir_fd)
    finally:
        if os.path.exists(temp_path):
            os.unlink(temp_path)


def reconcile(c):
    protect = {p.strip() for p in c.get("ad", "protect", fallback="radium").split(",") if p.strip()}
    desired = sor_hosts(c)
    managed = load_state(c)
    added = updated = deleted = failed = 0

    for name, ip in sorted(desired.items()):
        cur = ad_query_a(c, name)
        if cur is None:
            rc, out = _sh(c, "add", name, "A", ip)
            if rc == 0:
                added += 1
                managed.add(name)
            else:
                failed += 1
                log(f"add {name} {ip} failed: {out.strip()[:160]}")
        elif cur != ip:
            rc, out = _sh(c, "update", name, "A", cur, ip)
            if rc == 0:
                updated += 1
            else:
                failed += 1
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
                managed.discard(name)
            else:
                failed += 1
                log(f"delete {name} {cur} failed: {out.strip()[:160]}")
        else:
            managed.discard(name)

    # Only names this applier actually created are deletion-eligible. Persist
    # successful additions even when another operation failed, then leave the
    # broker event unacked for retry. A pre-existing AD record is never claimed.
    save_state(c, managed)
    if added or updated or deleted:
        log(f"reconciled akoria.org: +{added} ~{updated} -{deleted}")
    else:
        log("reconciled akoria.org: no change")
    if failed:
        log(f"{failed} AD DNS operation(s) failed; delivery remains unacknowledged")
    return failed == 0


def run():
    c = load_cfg()
    exchange = c.get("amqp", "exchange", fallback="sor.events")
    debounce = c.getfloat("ad", "debounce_sec", fallback=4.0)
    reconnect = c.getfloat("amqp", "reconnect_delay_sec", fallback=5.0)
    reconcile_interval = c.getfloat("ad", "reconcile_interval_sec", fallback=3600.0)
    queue = "sor.apply.ad"

    while _run:
        conn = None
        try:
            conn = pika.BlockingConnection(pika.URLParameters(c.get("amqp", "url")))
            ch = conn.channel()
            ch.exchange_declare(exchange=exchange, exchange_type="topic", durable=True)
            ch.queue_declare(queue=queue, durable=True)
            for k in HOST_KEYS:
                ch.queue_bind(exchange=exchange, queue=queue, routing_key=k)
            log(f"consuming {queue} (debounce={debounce}s, reconcile={reconcile_interval}s)")
            consume_after_apply(ch, queue, debounce, reconcile_interval,
                                lambda: reconcile(c), lambda: _run)
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
