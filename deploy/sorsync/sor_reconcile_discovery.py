#!/usr/bin/env python3
"""sor_reconcile_discovery — reverse path: detection → SoR.

Folds ADOPTED, named hosts from the monitoring DB's `discovered` table into the
SoR as first-class entities + IP addresses, tagged with the `solarinet_monitor`
provenance source. Correlation is by IP (the reliable key): a discovery whose IP
already owns a live `ip_addresses` row is left alone; a genuinely new one is
inserted. Writing the SoR fires the CDC triggers, so a newly-detected host flows
onward through sor_emitd → sor_apply_dns into DNS automatically — closing the loop.

Conservative by design: only status='adopted' (operator-curated) rows with a real
hostname are synced, so random transient scan hits never pollute the SoR.

Usage: sor_reconcile_discovery.py [--once] [--dry-run]
Config: sorsync.conf [sor] + [discovery].
"""
import argparse
import configparser
import os
import signal
import sys
import time

import pymysql

CONF = os.environ.get("SORSYNC_CONF",
                      os.path.join(os.path.dirname(os.path.abspath(__file__)), "sorsync.conf"))
_run = True


def log(msg):
    print(f"{time.strftime('%Y-%m-%dT%H:%M:%SZ', time.gmtime())} sor_reconcile: {msg}", flush=True)


def load_cfg():
    c = configparser.ConfigParser()
    if not c.read(CONF):
        log(f"FATAL: cannot read {CONF}"); sys.exit(1)
    return c


def short_name(host, mdns, ip):
    """First DNS label of the best available name; None if only an IP is known."""
    for cand in (host, mdns):
        if cand and cand.strip() and cand.strip() != ip:
            return cand.strip().split(".")[0].lower()
    return None


def aton_arg(ip):
    """INET6_ATON argument matching how the SoR stores addresses: IPv4 as a
    v4-mapped ::ffff: 16-byte value (the seed's convention), IPv6 verbatim."""
    return ip if ":" in ip else f"::ffff:{ip}"


def reconcile(cfg, dry_run):
    mon = pymysql.connect(
        host=cfg.get("discovery", "mon_host", fallback="127.0.0.1"),
        port=cfg.getint("discovery", "mon_port", fallback=3306),
        user=cfg.get("discovery", "mon_user"), password=cfg.get("discovery", "mon_password"),
        database=cfg.get("discovery", "mon_name", fallback="solarinet"),
        autocommit=True, connect_timeout=10, cursorclass=pymysql.cursors.DictCursor)
    sor = pymysql.connect(
        host=cfg.get("sor", "host"), port=cfg.getint("sor", "port", fallback=3306),
        user=cfg.get("sor", "user"), password=cfg.get("sor", "password"),
        database=cfg.get("sor", "name", fallback="sor"),
        autocommit=False, connect_timeout=10, cursorclass=pymysql.cursors.DictCursor)
    added = skipped = 0
    try:
        with mon.cursor() as mc:
            mc.execute("SELECT discId, ip, host, mdnsName, deviceRole "
                       "FROM discovered WHERE status='adopted' AND ip IS NOT NULL")
            rows = mc.fetchall()
        with sor.cursor() as sc:
            sc.execute("SELECT id FROM sources WHERE slug='solarinet_monitor'")
            src = sc.fetchone()["id"]
            for r in rows:
                name = short_name(r["host"], r["mdnsName"], r["ip"])
                if not name:
                    continue                                   # nameless: not a DNS-worthy entity
                sc.execute("SELECT entity_id FROM ip_addresses "
                           "WHERE address=INET6_ATON(%s) AND deleted_at IS NULL", (aton_arg(r["ip"]),))
                if sc.fetchone():
                    skipped += 1
                    continue                                   # IP already owned in the SoR
                if dry_run:
                    log(f"WOULD add {name} ({r['ip']})")
                    added += 1
                    continue
                sc.execute("INSERT INTO entities (name, entity_type, source_id, asserted_kind) "
                           "VALUES (%s,'other',%s,'machine')", (name, src))
                eid = sc.lastrowid
                sc.execute("INSERT INTO ip_addresses (entity_id, address, is_primary, source_id, asserted_kind) "
                           "VALUES (%s, INET6_ATON(%s), 1, %s, 'machine')", (eid, aton_arg(r["ip"]), src))
                log(f"added {name} ({r['ip']}) entity={eid}")
                added += 1
        if dry_run:
            sor.rollback()
        else:
            sor.commit()
    finally:
        mon.close(); sor.close()
    log(f"reconcile complete: {added} {'would-add' if dry_run else 'added'}, {skipped} already present")
    return added


def _stop(*_):
    global _run
    _run = False


if __name__ == "__main__":
    ap = argparse.ArgumentParser()
    ap.add_argument("--once", action="store_true", help="run one pass and exit")
    ap.add_argument("--dry-run", action="store_true", help="report changes without writing")
    args = ap.parse_args()
    signal.signal(signal.SIGTERM, _stop)
    signal.signal(signal.SIGINT, _stop)
    cfg = load_cfg()
    interval = cfg.getfloat("discovery", "interval_sec", fallback=60.0)
    while _run:
        try:
            reconcile(cfg, args.dry_run)
        except Exception as e:  # noqa: BLE001
            log(f"error: {e!r}")
        if args.once or args.dry_run:
            break
        for _ in range(int(interval)):
            if not _run:
                break
            time.sleep(1)
