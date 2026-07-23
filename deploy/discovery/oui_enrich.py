#!/usr/bin/env python3
"""oui_enrich — MAC -> vendor OUI enricher for the SolariNet Discovery table.

Reads `discovered` rows that have a non-empty `mac` but a NULL/empty
`vendor`, resolves the OUI (the first 3 octets / 6 hex digits of the MAC) to
a manufacturer name via a LOCAL, offline lookup, and UPDATEs `vendor` +
`enrichedAt`. Never calls out to the network (no MAC-vendor web API) — same
"local/offline enrichment" posture as the rest of deploy/discovery/.

OUI source, tried in order:

1. **`/usr/share/nmap/nmap-mac-prefixes`** (present on xenon — nmap is
   already installed for `serverScan.c`'s enrichment path) — plain text,
   one entry per line: `XXXXXX Vendor Name`, first 6 hex chars = OUI.
2. **python `manuf` lib** (`pip install manuf`), if importable, as a
   fallback/alternate table.

If neither source is available, logs a clear message and exits 0 (there is
nothing pending to do without a table; a probe host missing both must never
fail a discovery run over it — same convention as mdns_inspect.py's
no-backend case).

MAC normalization: strip `:`, `-`, `.` separators, uppercase, take the first
6 hex characters (the OUI). Malformed/short MACs are skipped (fail-soft,
logged, don't abort the run).

Idempotent: only rows with `vendor IS NULL OR vendor = ''` are selected, so
re-running is a no-op for already-enriched rows. `--dry-run` reports would-be
updates and a coverage count without writing; `--conf` selects a config file.

Usage: oui_enrich.py [--dry-run] [--conf PATH]
Config: oui_enrich.conf [db] (see .example). DB password: config, else
$SOLARI_DB_PASS (systemd sources run/db.env — same convention as
deploy/alertbridge/alertbridge.py / deploy/discovery/avahi_import.py).
"""
import argparse
import configparser
import os
import re
import sys
import time

try:
    import pymysql
except ImportError:
    pymysql = None

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
DEFAULT_CONF = os.environ.get("OUI_ENRICH_CONF",
                              os.path.join(SCRIPT_DIR, "oui_enrich.conf"))

NMAP_MAC_PREFIXES = os.environ.get("OUI_ENRICH_NMAP_PREFIXES",
                                    "/usr/share/nmap/nmap-mac-prefixes")

# discovered.vendor VARCHAR(64) (007_discovery_enrichment.sql).
VENDOR_MAXLEN = 64

HEX6_RE = re.compile(r"^[0-9A-F]{6}$")


def log(msg):
    print(f"{time.strftime('%Y-%m-%dT%H:%M:%SZ', time.gmtime())} oui_enrich: {msg}",
          flush=True)


# --------------------------------------------------------------------------- #
# MAC normalization                                                          #
# --------------------------------------------------------------------------- #
def normalize_oui(mac):
    """Strip `:`/`-`/`.` separators, uppercase, take the first 6 hex chars
    (the OUI). Returns None if the result isn't 6 valid hex digits (e.g. a
    malformed or too-short MAC — a locally-administered/randomized MAC is
    still 6 valid hex digits and is left to the table lookup to simply not
    match)."""
    if not mac:
        return None
    cleaned = re.sub(r"[:\-.]", "", mac.strip()).upper()
    oui = cleaned[:6]
    if not HEX6_RE.match(oui):
        return None
    return oui


# --------------------------------------------------------------------------- #
# OUI table sources                                                          #
# --------------------------------------------------------------------------- #
def load_nmap_prefixes(path):
    """Parse `/usr/share/nmap/nmap-mac-prefixes`: `XXXXXX Vendor Name` per
    line, `#`-comments and blank lines ignored. Returns {OUI: vendor} or
    None if the file isn't readable/present."""
    table = {}
    try:
        with open(path, "r", errors="replace") as f:
            for line in f:
                line = line.strip()
                if not line or line.startswith("#"):
                    continue
                parts = line.split(None, 1)
                if len(parts) != 2:
                    continue
                oui, vendor = parts
                oui = oui.strip().upper()
                if HEX6_RE.match(oui):
                    table[oui] = vendor.strip()
    except OSError as e:
        log(f"nmap-mac-prefixes not usable at {path}: {e!r}")
        return None
    if not table:
        return None
    log(f"loaded {len(table)} OUI(s) from {path}")
    return table


def load_manuf():
    """Fallback: the python `manuf` lib, if importable. Returns a lookup
    callable(oui) -> vendor|None, or None if `manuf` isn't installed."""
    try:
        from manuf import manuf  # noqa: PLC0415 — optional dependency
    except ImportError:
        return None
    parser = manuf.MacParser(update=False)
    log("using manuf library for OUI lookup")

    def lookup(oui):
        # manuf wants a MAC-shaped string; a bare OUI with trailing zeros
        # resolves the same vendor.
        fake_mac = f"{oui[0:2]}:{oui[2:4]}:{oui[4:6]}:00:00:00"
        return parser.get_manuf_long(fake_mac) or parser.get_manuf(fake_mac)

    return lookup


def build_resolver(nmap_path=NMAP_MAC_PREFIXES):
    """Returns a callable(oui) -> vendor|None using the best available local
    source, or None if no source is available at all."""
    table = load_nmap_prefixes(nmap_path)
    if table is not None:
        return lambda oui: table.get(oui)

    manuf_lookup = load_manuf()
    if manuf_lookup is not None:
        return manuf_lookup

    return None


# --------------------------------------------------------------------------- #
# config / db                                                                 #
# --------------------------------------------------------------------------- #
def load_cfg(path):
    c = configparser.ConfigParser()
    if not c.read(path):
        log(f"FATAL: cannot read config {path} "
            f"(copy oui_enrich.conf.example to oui_enrich.conf)")
        sys.exit(1)
    return c


def db_password(c):
    """DB password from config, else the SOLARI_DB_PASS env var — same
    convention as deploy/discovery/avahi_import.py's db_password()."""
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
# enrich                                                                      #
# --------------------------------------------------------------------------- #
def enrich(db, resolve, dry_run):
    updated = unresolved = skipped = errors = 0
    with db.cursor() as cur:
        try:
            cur.execute(
                "SELECT discId, mac FROM discovered "
                "WHERE mac IS NOT NULL AND mac <> '' "
                "AND (vendor IS NULL OR vendor = '')")
            rows = cur.fetchall()
        except Exception as e:  # noqa: BLE001 — top-level select must not crash the run
            log(f"FATAL: cannot select candidate rows: {e!r}")
            raise

        log(f"{len(rows)} candidate row(s) with mac set and vendor unset")

        for row in rows:
            disc_id, mac = row["discId"], row["mac"]
            oui = normalize_oui(mac)
            if oui is None:
                log(f"SKIP discId={disc_id} mac={mac!r}: not a valid MAC/OUI")
                skipped += 1
                continue

            try:
                vendor = resolve(oui)
            except Exception as e:  # noqa: BLE001 — fail-soft per row
                log(f"ERROR resolving discId={disc_id} mac={mac!r} oui={oui}: {e!r}")
                errors += 1
                continue

            if not vendor:
                unresolved += 1
                continue

            vendor = vendor.strip()[:VENDOR_MAXLEN]

            if dry_run:
                log(f"WOULD update discId={disc_id} mac={mac!r} oui={oui} -> vendor={vendor!r}")
                updated += 1
                continue

            try:
                cur.execute(
                    "UPDATE discovered SET vendor=%s, enrichedAt=UTC_TIMESTAMP() "
                    "WHERE discId=%s", (vendor, disc_id))
                updated += 1
            except Exception as e:  # noqa: BLE001 — fail-soft per row
                log(f"ERROR updating discId={disc_id} mac={mac!r}: {e!r}")
                errors += 1

    if dry_run:
        db.rollback()
    else:
        db.commit()
    log(f"enrich complete: {updated} {'would-update' if dry_run else 'updated'}, "
        f"{unresolved} unresolved (OUI not in table), {skipped} skipped (bad mac), "
        f"{errors} errors")
    return updated, unresolved, skipped, errors


# --------------------------------------------------------------------------- #
# coverage (for --dry-run reporting / checkOk)                                #
# --------------------------------------------------------------------------- #
def coverage(db):
    """(rows with mac set, rows with mac set AND vendor set) — printed after
    a run so an operator can see overall coverage move, e.g. the 0/115 gap
    this tool exists to close."""
    with db.cursor() as cur:
        cur.execute("SELECT COUNT(*) AS n FROM discovered WHERE mac IS NOT NULL AND mac <> ''")
        with_mac = cur.fetchone()["n"]
        cur.execute(
            "SELECT COUNT(*) AS n FROM discovered "
            "WHERE mac IS NOT NULL AND mac <> '' AND vendor IS NOT NULL AND vendor <> ''")
        with_vendor = cur.fetchone()["n"]
    return with_mac, with_vendor


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                  formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--conf", default=DEFAULT_CONF,
                     help=f"config file (default: {DEFAULT_CONF})")
    ap.add_argument("--dry-run", action="store_true",
                     help="report changes without writing")
    ap.add_argument("--nmap-prefixes", default=NMAP_MAC_PREFIXES,
                     help=f"path to nmap-mac-prefixes (default: {NMAP_MAC_PREFIXES})")
    args = ap.parse_args()

    if pymysql is None:
        log("FATAL: pymysql not installed (pip install pymysql in deploy/discovery/.venv)")
        sys.exit(1)

    resolve = build_resolver(args.nmap_prefixes)
    if resolve is None:
        log(f"no local OUI source available: {args.nmap_prefixes} not readable and "
            f"the 'manuf' python lib is not importable. Install nmap "
            f"(provides nmap-mac-prefixes) or `pip install manuf`. Exiting gracefully.")
        sys.exit(0)

    cfg = load_cfg(args.conf)
    try:
        db = db_connect(cfg)
    except Exception as e:  # noqa: BLE001 — top-level: nothing to do without a DB
        log(f"FATAL: cannot connect to DB: {e!r}")
        sys.exit(1)

    try:
        _, _, _, errors = enrich(db, resolve, args.dry_run)
        with_mac, with_vendor = coverage(db)
        log(f"coverage: {with_vendor}/{with_mac} discovered rows with mac now have vendor")
    finally:
        db.close()

    sys.exit(1 if errors else 0)


if __name__ == "__main__":
    main()
