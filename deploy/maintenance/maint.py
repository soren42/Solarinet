#!/usr/bin/env python3
"""maint — schedule / list / cancel SolariNet maintenance (planned-outage) windows.

A host under an ACTIVE window is expected to be down: the alert-bridge suppresses
its notifications AND the dead-man's-switch, and Opie skips investigating it — so
powering a box down for an install doesn't crit-page you. Times are server-local.

Usage:
  maint.py schedule --host benzene --reason "Oculink + ARC B580 eGPU" --hours 4
  maint.py schedule --host cesium  --from "2026-07-08 14:00" --to "2026-07-08 18:00"
  maint.py schedule --all --reason "rack power work" --hours 2      # whole fleet
  maint.py list [--all-history]
  maint.py cancel <windowId>

DB creds: SOLARI_DB_PASS (source run/db.env first) or --password.
"""
import argparse
import datetime
import os
import sys

# The server clock is UTC, but you think in local time. Windows are STORED in UTC
# (to match MySQL NOW()), but the CLI ACCEPTS and DISPLAYS your local timezone so
# "--from 3:00 PM" and the listing both mean what you expect.
try:
    from zoneinfo import ZoneInfo
    LOCAL_TZ = ZoneInfo(os.environ.get("SOLARI_TZ", "America/New_York"))
except Exception:  # noqa: BLE001 — fall back to fixed EDT if tzdata is missing
    LOCAL_TZ = datetime.timezone(datetime.timedelta(hours=-4), "EDT")
UTC = datetime.timezone.utc


def now_utc_naive():
    return datetime.datetime.now(UTC).replace(tzinfo=None)


def local_to_utc(dt_naive_local):
    return dt_naive_local.replace(tzinfo=LOCAL_TZ).astimezone(UTC).replace(tzinfo=None)


def utc_to_local(dt_naive_utc):
    return dt_naive_utc.replace(tzinfo=UTC).astimezone(LOCAL_TZ)

try:
    import pymysql
except ImportError:
    sys.exit("pymysql not installed — run with the alertbridge venv:\n"
             "  source /home/jason/Code/Solarinet/run/db.env && "
             "/home/jason/Code/Solarinet/deploy/alertbridge/.venv/bin/python "
             "/home/jason/Code/Solarinet/deploy/maintenance/maint.py ...")


def connect(args):
    pw = args.password or os.environ.get("SOLARI_DB_PASS", "")
    return pymysql.connect(host=args.host_db, port=args.port, user=args.user,
                           password=pw, database=args.db, autocommit=True,
                           cursorclass=pymysql.cursors.DictCursor, connect_timeout=8)


def resolve_target(cur, host):
    """(nodeId|None, fqdn, ip|None). A host may be a client node, a probe-only
    target (e.g. benzene — monitored by port checks, no client), or both. We match
    client-node alerts by nodeId and probe-target alerts by IP, so resolve both."""
    import socket
    cur.execute("SELECT nodeId, hostFqdn FROM node WHERE hostFqdn=%s OR hostFqdn LIKE %s "
                "ORDER BY (hostFqdn=%s) DESC LIMIT 1", (host, host + ".%", host))
    node = cur.fetchone()
    ip = None
    for name in (host, host if host.endswith(".akoria.net") else host + ".akoria.net"):
        try:
            ip = socket.gethostbyname(name); break
        except OSError:
            continue
    fqdn = (node["hostFqdn"] if node else None) or host
    return (node["nodeId"] if node else None), fqdn, ip


def parse_dt(s):
    """Parse a local-time 'YYYY-MM-DD HH:MM' into a naive-UTC datetime for storage."""
    for fmt in ("%Y-%m-%d %H:%M", "%Y-%m-%d %H:%M:%S", "%Y-%m-%dT%H:%M"):
        try:
            return local_to_utc(datetime.datetime.strptime(s, fmt))
        except ValueError:
            continue
    sys.exit(f"bad datetime {s!r} (use local 'YYYY-MM-DD HH:MM')")


def cmd_schedule(conn, args):
    now = now_utc_naive()   # stored/compared in UTC to match MySQL NOW()
    if args.hours:
        start, end = now, now + datetime.timedelta(hours=args.hours)
    elif args.from_ and args.to:
        start, end = parse_dt(args.from_), parse_dt(args.to)
    else:
        sys.exit("give --hours N, or both --from and --to")
    if end <= start:
        sys.exit("end must be after start")
    scope = "all" if args.all else "node"
    node_id, fqdn, ip = None, None, None
    with conn.cursor() as cur:
        if scope == "node":
            if not args.host:
                sys.exit("--host is required (or use --all)")
            node_id, fqdn, ip = resolve_target(cur, args.host)
            if node_id is None and ip is None:
                sys.exit(f"can't resolve {args.host!r} to a node or an IP — give a host that "
                         f"has reported, or one that resolves in DNS")
        cur.execute(
            "INSERT INTO maintenanceWindow (scope,nodeId,hostFqdn,ipAddr,reason,startsAt,endsAt,status,createdBy) "
            "VALUES (%s,%s,%s,%s,%s,%s,%s,'scheduled',%s)",
            (scope, node_id, fqdn, ip, args.reason, start, end, args.by))
        wid = cur.lastrowid
    kind = []
    if node_id is not None: kind.append("client-node")
    if ip: kind.append(f"probe-target {ip}")
    tgt = "ALL HOSTS" if scope == "all" else f"{fqdn} ({', '.join(kind) or 'unresolved'})"
    ls, le = utc_to_local(start), utc_to_local(end)
    print(f"scheduled window #{wid}: {tgt}  {ls:%Y-%m-%d %I:%M %p} -> {le:%I:%M %p %Z}"
          f"  ({args.reason or 'maintenance'})")
    active = start <= now <= end
    print("  -> ACTIVE NOW: suppression in effect." if active
          else "  -> starts later; suppression begins at the start time.")


def cmd_list(conn, args):
    where = "" if args.all_history else \
        "WHERE status IN ('scheduled','active') AND endsAt >= NOW()"
    with conn.cursor() as cur:
        cur.execute(
            f"SELECT windowId,scope,hostFqdn,reason,startsAt,endsAt,status,"
            f"       (NOW() BETWEEN startsAt AND endsAt AND status IN ('scheduled','active')) AS live "
            f"FROM maintenanceWindow {where} ORDER BY startsAt DESC LIMIT 50")
        rows = cur.fetchall()
    if not rows:
        print("no maintenance windows"); return
    for r in rows:
        tgt = "ALL" if r["scope"] == "all" else (r["hostFqdn"] or "?")
        flag = "● LIVE" if r["live"] else f"  {r['status']}"
        s, e = utc_to_local(r["startsAt"]), utc_to_local(r["endsAt"])
        print(f"#{r['windowId']:<4} {flag:8} {tgt:<22} "
              f"{s:%m-%d %I:%M %p}->{e:%I:%M %p}  {r['reason'] or ''}")


def cmd_cancel(conn, args):
    with conn.cursor() as cur:
        n = cur.execute("UPDATE maintenanceWindow SET status='cancelled' "
                        "WHERE windowId=%s AND status IN ('scheduled','active')", (args.windowId,))
    print(f"cancelled window #{args.windowId}" if n else
          f"no active window #{args.windowId}")


def main():
    p = argparse.ArgumentParser(description=__doc__,
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--host-db", default="127.0.0.1"); p.add_argument("--port", type=int, default=3306)
    p.add_argument("--user", default="solari"); p.add_argument("--db", default="solarinet")
    p.add_argument("--password", default=None)
    sub = p.add_subparsers(dest="cmd", required=True)

    s = sub.add_parser("schedule")
    s.add_argument("--host"); s.add_argument("--all", action="store_true")
    s.add_argument("--reason", default=None)
    s.add_argument("--hours", type=float, default=None)
    s.add_argument("--from", dest="from_", default=None); s.add_argument("--to", default=None)
    s.add_argument("--by", default=os.environ.get("USER", "operator"))
    s.set_defaults(fn=cmd_schedule)

    ls = sub.add_parser("list"); ls.add_argument("--all-history", action="store_true")
    ls.set_defaults(fn=cmd_list)

    c = sub.add_parser("cancel"); c.add_argument("windowId", type=int)
    c.set_defaults(fn=cmd_cancel)

    args = p.parse_args()
    conn = connect(args)
    try:
        args.fn(conn, args)
    finally:
        conn.close()


if __name__ == "__main__":
    main()
