#!/usr/bin/env python3
"""avahi_import — one-shot/repeatable Avahi/mDNS discovery importer.

Reads an `avahi-browse`-style JSON export (see .example / README for the exact
shape) and AUGMENTS the SolariNet Discovery asset table (`discovered`) with
mDNS host names + a compact summary of advertised service types:

  {"hosts": [
    {"name": "hydrogen (617)",
     "addresses": ["10.0.1.50", "hydrogen.local.", "fd1f:...:47fc:4fe3"],
     "services": [{"type": "_airplay._tcp.", "port": 7000, "domain": null,
                   "name": "hydrogen", "data": {...}}, ...]}
  ]}

Correlation is by IPv4 address (the reliable key, same convention as
sor_reconcile_discovery.py): a host whose IPv4 already owns a `discovered` row
gets `mdnsName`/`mdnsServices` UPDATEd in place; a genuinely new IPv4 is
INSERTed as a fresh `discovered` row (kind='host', via='mdns', status='new').
IPv6-only / addressless entries (e.g. bare Matter/Thread devices) are skipped —
`discovered.ip` is the correlation key and there is nothing to correlate on.

A host with multiple IPv4 addresses (e.g. a dual-homed box) produces one
`discovered` row per address, all sharing the same mdnsName/mdnsServices —
mirrors how ARP/portscan discovery already keys one row per (ip, kind).

Idempotent: re-running the same export changes nothing beyond bumping
seenCount/lastSeenAt. Fail-soft: a malformed host entry is logged and skipped,
never aborts the run.

Usage: avahi_import.py [--file PATH] [--dry-run]
Config: avahi_import.conf [db] (see .example). DB password: config, else
$SOLARI_DB_PASS (systemd sources run/db.env — same convention as
deploy/alertbridge/alertbridge.py / deploy/systemd/solarinet-snmp.service).
"""
import argparse
import configparser
import json
import os
import re
import sys
import time

import pymysql

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
DEFAULT_CONF = os.environ.get("AVAHI_IMPORT_CONF",
                              os.path.join(SCRIPT_DIR, "avahi_import.conf"))
DEFAULT_EXPORT = os.environ.get("AVAHI_IMPORT_FILE",
                                os.path.join(SCRIPT_DIR, "flame-export.json"))

# Max length of the mdnsServices summary column (013_mdns_services.sql).
MDNS_SERVICES_MAXLEN = 512

IPV4_RE = re.compile(r"^\d{1,3}\.\d{1,3}\.\d{1,3}\.\d{1,3}$")


def log(msg):
    print(f"{time.strftime('%Y-%m-%dT%H:%M:%SZ', time.gmtime())} avahi_import: {msg}",
          flush=True)


# --------------------------------------------------------------------------- #
# config                                                                      #
# --------------------------------------------------------------------------- #
def load_cfg(path):
    c = configparser.ConfigParser()
    if not c.read(path):
        log(f"FATAL: cannot read config {path} "
            f"(copy avahi_import.conf.example to avahi_import.conf)")
        sys.exit(1)
    return c


def db_password(c):
    """DB password from config, else the SOLARI_DB_PASS env var — same
    convention as deploy/alertbridge/alertbridge.py's db_password()."""
    pw = c.get("db", "password", fallback="").strip()
    if pw and not pw.startswith("CHANGE"):
        return pw
    env = os.environ.get("SOLARI_DB_PASS")
    if env:
        return env
    log("WARNING: no DB password in config or $SOLARI_DB_PASS")
    return pw


def db_connect(c):
    return pymysql.connect(
        host=c.get("db", "host", fallback="127.0.0.1"),
        port=c.getint("db", "port", fallback=3306),
        user=c.get("db", "user", fallback="solari"),
        password=db_password(c),
        database=c.get("db", "name", fallback="solarinet"),
        autocommit=False, connect_timeout=10,
        read_timeout=30, write_timeout=30,
        cursorclass=pymysql.cursors.DictCursor,
    )


# --------------------------------------------------------------------------- #
# export parsing                                                              #
# --------------------------------------------------------------------------- #
def load_export(path):
    with open(path, "r") as f:
        doc = json.load(f)
    hosts = doc.get("hosts")
    if not isinstance(hosts, list):
        raise ValueError(f"expected top-level {{'hosts': [...]}}, got {type(doc).__name__}")
    return hosts


def ipv4_addresses(host):
    """Distinct IPv4 literals from host['addresses'] (skips *.local. names
    and IPv6 literals — mDNS hostnames and v6 addresses aren't the discovered
    ip correlation key)."""
    out = []
    for a in host.get("addresses") or []:
        a = (a or "").strip()
        if a and IPV4_RE.match(a) and a not in out:
            out.append(a)
    return out


def service_summary(host):
    """Comma-joined, de-duplicated, order-preserving list of advertised
    service types with the trailing DNS-SD dot stripped
    ("_airplay._tcp." -> "_airplay._tcp"), truncated to fit the column."""
    types = []
    for svc in host.get("services") or []:
        t = (svc.get("type") or "").strip()
        if not t:
            continue
        t = t[:-1] if t.endswith(".") else t
        if t not in types:
            types.append(t)
    summary = ",".join(types)
    if len(summary) > MDNS_SERVICES_MAXLEN:
        # Truncate on a whole-entry boundary rather than mid-token.
        out, total = [], 0
        for t in types:
            add = len(t) + (1 if out else 0)
            if total + add > MDNS_SERVICES_MAXLEN:
                break
            out.append(t)
            total += add
        summary = ",".join(out)
    return summary or None


def friendly_name(host):
    name = (host.get("name") or "").strip()
    return name or None


# --------------------------------------------------------------------------- #
# import                                                                      #
# --------------------------------------------------------------------------- #
def import_export(db, hosts, dry_run):
    updated = inserted = skipped = errors = 0
    with db.cursor() as cur:
        for h in hosts:
            try:
                name = friendly_name(h)
                services = service_summary(h)
                addrs = ipv4_addresses(h)
            except Exception as e:  # noqa: BLE001 — one bad entry must not abort the run
                log(f"ERROR parsing host entry {h!r}: {e!r}; skipping")
                errors += 1
                continue
            if not addrs:
                skipped += 1
                continue  # no IPv4 to correlate on (e.g. bare Matter/Thread device)

            for ip in addrs:
                try:
                    cur.execute(
                        "SELECT discId, mdnsName, mdnsServices, host FROM discovered "
                        "WHERE ip=%s ORDER BY (kind='host') DESC LIMIT 1", (ip,))
                    row = cur.fetchone()
                except Exception as e:  # noqa: BLE001 — fail-soft per row
                    log(f"ERROR looking up ip={ip}: {e!r}; skipping")
                    errors += 1
                    continue

                if row:
                    if row["mdnsName"] == name and row["mdnsServices"] == services:
                        continue  # already up to date; still counts as "seen" but nothing to write
                    if dry_run:
                        log(f"WOULD update discId={row['discId']} ip={ip} "
                            f"mdnsName={name!r} mdnsServices={services!r}")
                        updated += 1
                        continue
                    try:
                        cur.execute(
                            "UPDATE discovered SET mdnsName=%s, mdnsServices=%s, "
                            "seenCount=seenCount+1, lastSeenAt=UTC_TIMESTAMP() "
                            "WHERE discId=%s", (name, services, row["discId"]))
                        updated += 1
                    except Exception as e:  # noqa: BLE001 — fail-soft per row
                        log(f"ERROR updating discId={row['discId']} ip={ip}: {e!r}")
                        errors += 1
                    continue

                # No existing row for this ip at all: insert a fresh mDNS discovery.
                if dry_run:
                    log(f"WOULD insert new host ip={ip} mdnsName={name!r} "
                        f"mdnsServices={services!r}")
                    inserted += 1
                    continue
                try:
                    cur.execute(
                        "INSERT INTO discovered "
                        "(host, ip, kind, via, mdnsName, mdnsServices, status, "
                        " seenCount, firstSeenAt, lastSeenAt) "
                        "VALUES (%s, %s, 'host', 'mdns', %s, %s, 'new', "
                        " 1, UTC_TIMESTAMP(), UTC_TIMESTAMP())",
                        (name, ip, name, services))
                    inserted += 1
                except Exception as e:  # noqa: BLE001 — fail-soft per row
                    log(f"ERROR inserting ip={ip}: {e!r}")
                    errors += 1

    if dry_run:
        db.rollback()
    else:
        db.commit()
    log(f"import complete: {updated} {'would-update' if dry_run else 'updated'}, "
        f"{inserted} {'would-insert' if dry_run else 'inserted'}, "
        f"{skipped} skipped (no IPv4), {errors} errors")
    return updated, inserted, skipped, errors


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                  formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--file", default=DEFAULT_EXPORT,
                     help=f"Avahi Browser export JSON (default: {DEFAULT_EXPORT})")
    ap.add_argument("--conf", default=DEFAULT_CONF,
                     help=f"config file (default: {DEFAULT_CONF})")
    ap.add_argument("--dry-run", action="store_true",
                     help="report changes without writing")
    args = ap.parse_args()

    try:
        hosts = load_export(args.file)
    except (OSError, ValueError, json.JSONDecodeError) as e:
        log(f"FATAL: cannot load export {args.file}: {e!r}")
        sys.exit(1)
    log(f"loaded {len(hosts)} host(s) from {args.file}")

    cfg = load_cfg(args.conf)
    try:
        db = db_connect(cfg)
    except Exception as e:  # noqa: BLE001 — top-level: nothing to do without a DB
        log(f"FATAL: cannot connect to DB: {e!r}")
        sys.exit(1)

    try:
        _, _, _, errors = import_export(db, hosts, args.dry_run)
    finally:
        db.close()

    sys.exit(1 if errors else 0)


if __name__ == "__main__":
    main()
