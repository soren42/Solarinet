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
Config: sorsync.conf [sor_discovery] + [discovery].
"""
import argparse
import configparser
import os
import signal
import sys
import time

import pymysql
from db_config import database_options

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


def connect_sor(config):
    db = database_options(config, "sor_discovery")
    return pymysql.connect(
        host=db["host"], port=db["port"], user=db["user"], password=db["password"],
        database=db["name"], autocommit=False, connect_timeout=10,
        cursorclass=pymysql.cursors.DictCursor)


# uname -m spellings -> hardware_models.arch enum values.
ARCH_ALIASES = {"aarch64": "arm64", "armv7l": "armhf", "armv6l": "armhf",
                "amd64": "x86_64", "i686": "i386", "i586": "i386"}

_LINUX_WORDS = ("linux", "debian", "ubuntu", "fedora", "rocky", "alma", "raspbian",
                "opensuse", "centos", "red hat", "arch", "alpine", "zimaos", "casaos")


def os_family(os_name):
    low = os_name.lower()
    if any(w in low for w in _LINUX_WORDS):
        return "linux"
    if "bsd" in low:
        return "bsd"
    if "mac" in low or "darwin" in low:
        return "macos"
    if "windows" in low:
        return "windows"
    return "other"


def reconcile(cfg, dry_run):
    mon = pymysql.connect(
        host=cfg.get("discovery", "mon_host", fallback="127.0.0.1"),
        port=cfg.getint("discovery", "mon_port", fallback=3306),
        user=cfg.get("discovery", "mon_user"), password=cfg.get("discovery", "mon_password"),
        database=cfg.get("discovery", "mon_name", fallback="solarinet"),
        autocommit=True, connect_timeout=10, cursorclass=pymysql.cursors.DictCursor)
    sor = connect_sor(cfg)
    added = skipped = enriched = 0
    try:
        with mon.cursor() as mc:
            mc.execute("SELECT discId, ip, host, mdnsName, deviceRole "
                       "FROM discovered WHERE status='adopted' AND ip IS NOT NULL")
            rows = mc.fetchall()
            mc.execute("SELECT hostFqdn, osName, arch FROM node "
                       "WHERE state <> 'retired' AND hostFqdn IS NOT NULL")
            node_facts = mc.fetchall()
        with sor.cursor() as sc:
            sc.execute("SELECT id FROM sources WHERE slug='solarinet_monitor'")
            src = sc.fetchone()["id"]
            # Reconcile enrolled monitoring facts by canonical short host name.
            # OS names reflect the running node and refresh on every pass;
            # hardware_models.arch is filled only when it has no known value.
            for fact in node_facts:
                name = (fact["hostFqdn"] or "").split(".")[0].lower()
                os_name = (fact["osName"] or "").strip()
                arch = ARCH_ALIASES.get((fact["arch"] or "").strip().lower(),
                                        (fact["arch"] or "").strip().lower())
                if not name or not os_name and not arch:
                    continue
                if dry_run:
                    log(f"WOULD enrich {name} (OS={bool(os_name)}, arch={bool(arch)})")
                    enriched += 1
                    continue
                sc.execute("SELECT e.id, e.os_id FROM entities e "
                           "WHERE LOWER(e.name)=%s AND e.deleted_at IS NULL LIMIT 1", (name,))
                entity = sc.fetchone()
                if not entity:
                    continue
                os_id = entity["os_id"]
                changed = False
                if os_name:
                    # operating_systems has no unique key (migration 001), so
                    # look up before inserting or every pass would add a row.
                    sc.execute("SELECT id FROM operating_systems WHERE name=%s AND version='' "
                               "AND deleted_at IS NULL ORDER BY id LIMIT 1", (os_name,))
                    found = sc.fetchone()
                    if found:
                        os_id = found["id"]
                    else:
                        sc.execute("INSERT INTO operating_systems (family,name,version,source_id,asserted_kind) "
                                   "VALUES (%s,%s,'',%s,'machine')",
                                   (os_family(os_name), os_name, src))
                        os_id = sc.lastrowid
                    if os_id != entity["os_id"]:
                        sc.execute("UPDATE entities SET os_id=%s, asserted_at=UTC_TIMESTAMP() "
                                   "WHERE id=%s", (os_id, entity["id"]))
                        changed = True
                if arch in {"x86_64", "arm64", "armhf", "riscv64", "i386"}:
                    sc.execute("UPDATE hardware_models hm JOIN hardware_units hu "
                               "ON hu.model_id=hm.id JOIN entities e "
                               "ON e.hardware_unit_id=hu.id "
                               "SET hm.arch=%s WHERE e.id=%s AND hm.arch IS NULL",
                               (arch, entity["id"]))
                    changed = changed or bool(sc.rowcount)
                # hardware_models has no CDC trigger. Touch its entity once when
                # model arch first becomes known so the existing SoR outbox
                # wakes the DNS renderer; subsequent passes stay quiet.
                if changed and os_id == entity["os_id"]:
                    sc.execute("UPDATE entities SET asserted_at=UTC_TIMESTAMP() WHERE id=%s",
                               (entity["id"],))
                enriched += 1
            for r in rows:
                name = short_name(r["host"], r["mdnsName"], r["ip"])
                if not name:
                    continue                                   # nameless: not a DNS-worthy entity
                # Any row counts, tombstoned included: a soft-deleted address is
                # an SoR decision that stale discovery must not resurrect.
                sc.execute("SELECT entity_id, deleted_at FROM ip_addresses "
                           "WHERE address=INET6_ATON(%s) ORDER BY deleted_at IS NULL DESC LIMIT 1",
                           (aton_arg(r["ip"]),))
                owner = sc.fetchone()
                if owner:
                    if owner["deleted_at"] is not None:
                        log(f"skip {name} ({r['ip']}): address is tombstoned in the SoR")
                    skipped += 1
                    continue                                   # IP already owned in the SoR
                # A new entity must not reuse a live host name or CNAME alias;
                # either would render conflicting records and fail the zone.
                sc.execute("SELECT 1 FROM entities WHERE LOWER(name)=%s AND deleted_at IS NULL "
                           "UNION ALL SELECT 1 FROM v_dns_cnames WHERE LOWER(alias)=%s LIMIT 1",
                           (name, name))
                if sc.fetchone():
                    log(f"skip {name} ({r['ip']}): name already used by an SoR host or alias")
                    skipped += 1
                    continue
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
    log(f"reconcile complete: {added} {'would-add' if dry_run else 'added'}, {skipped} already present, {enriched} node facts reconciled")
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
