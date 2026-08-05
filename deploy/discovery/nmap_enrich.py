#!/usr/bin/env python3
"""nmap_enrich — active-recon (nmap) intel enricher for the SolariNet
Discovery asset table (`discovered`).

*** AUTHORIZED SCANNING NOTICE ***
This tool runs `nmap`/HTTP probes against hosts on Akoria, the operator's
OWN homelab network. Active-recon (port scan, OS fingerprint, service/banner
grab) here is normal, expected network administration -- NOT offensive
tooling against a third party. Only ever point this at RFC1918 /
operator-owned segments (see `_is_private_ipv4()` below).

Many `discovered` rows -- especially the ones ARP/portscan turned up with
nothing but a bare IPv4 -- have no OS, no service list, no banner: hard to
recognize, hard to adopt into monitoring. This tool closes that gap with a
real `nmap -sV -O` sweep (plus a quick HTTP HEAD/GET for common web ports)
per candidate host, and stores what it learns:

  - `openPorts` -- compact "port/proto/service/product/version" summary,
    comma-joined (e.g. "22/tcp/ssh/OpenSSH/9.6,80/tcp/http/nginx/1.24").
  - `osGuess`   -- nmap's best OS-fingerprint match (name only, truncated),
    or NULL if `-O` couldn't identify one (needs raw-socket privilege --
    see "Privilege" below -- or the host just didn't answer distinctively).
  - `banners`   -- newline-joined `service: banner-text` lines: nmap script/
    version-detection banner output (SSH ident string, SMTP/FTP/telnet
    greeting, etc.) plus, for 80/443/8080/3000, a quick HTTP HEAD/GET
    capturing the `Server:` header and `<title>` (a stand-in for an
    issue/motd banner on web-managed gear).
  - `nmapEnrichedAt` -- when this pass touched the row; also the "already
    recently scanned, skip it" freshness gate for the next run.

See `db/migrations/015_nmap_enrich.sql`.

Complements, does NOT duplicate, `oui_enrich.py`: that tool fills `vendor`
from `mac` via a LOCAL offline OUI table (no network I/O). This tool is the
one that actually touches the wire. Run both in the same post-scan hook (see
README.md "Wiring into a discovery probe") -- a scan populates `mac` (which
`oui_enrich.py` then resolves) and gives this tool fresh, or newly bare,
IPv4 candidates to sweep.

Selection (candidates, most-empty-first):
  - `ip` must look like a routable, private (RFC1918) IPv4 -- link-local/
    loopback/multicast and anything that doesn't parse are skipped; this
    tool only ever points nmap at the operator's own segments.
  - `nmapEnrichedAt IS NULL OR nmapEnrichedAt < NOW() - INTERVAL <cooldown>`
    (`--cooldown-hours`, default 24) -- a freshly-enriched row is skipped so
    repeated runs don't keep re-scanning the same hosts.
  - Ordered so rows missing the most (`openPorts`/`osGuess` both NULL) are
    scanned first, then capped at `--limit` (default 25) per run -- nmap OS
    detection + a 200-port sweep is not cheap; this is a rate-limited,
    incremental crawl of the discovery table, not a one-shot bulk scan.
  - The discovery ~/20 (4096-host) CIDR cap enforced by `serverScan.c`
    (`SCAN_MAX_HOSTS`) bounds how big `discovered` can ever get from a single
    scan; `--limit` additionally bounds how much of it this tool chews on in
    one pass, same "small, incremental, rate-limited" posture as
    `oui_enrich.py`/`mdns_inspect.py`.

Privilege: `nmap -O` (OS fingerprinting) and SYN-based `-sV` need raw-socket
access (CAP_NET_RAW), so this script expects to run as root or under `sudo`
(the systemd unit's `ExecStart` uses `sudo nmap` accordingly -- see
`solari-nmap-enrich.service`). Without it, nmap silently falls back to a
degraded (connect-scan, no OS guess) mode rather than erroring, so this
script does not itself enforce a privilege check -- it just stores whatever
nmap was able to determine, same fail-soft posture as everywhere else in
this directory.

Idempotent / fail-soft: each host is scanned and stored independently -- one
host timing out, refusing to be OS-fingerprinted, or having no open ports at
all never aborts the run, and only that host's row is left as-is (or gets a
`nmapEnrichedAt` bump with whatever partial info was gathered) rather than
raising. Only rows past the freshness cooldown are re-touched, so re-running
this tool is safe.

Usage: nmap_enrich.py [--dry-run] [--conf PATH] [--limit N]
                       [--cooldown-hours H] [--host-timeout SECONDS]
Config: nmap_enrich.conf [db] (see .example). DB password: config, else
$SOLARI_DB_PASS (systemd sources run/db.env -- same convention as
deploy/discovery/oui_enrich.py / avahi_import.py).
"""
import argparse
import configparser
import ipaddress
import os
import re
import shutil
import subprocess
import sys
import time
import xml.etree.ElementTree as ET
from urllib.request import Request, urlopen
from urllib.error import URLError, HTTPError

try:
    import pymysql
except ImportError:
    pymysql = None

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
DEFAULT_CONF = os.environ.get("NMAP_ENRICH_CONF",
                              os.path.join(SCRIPT_DIR, "nmap_enrich.conf"))

DEFAULT_LIMIT = 25
DEFAULT_COOLDOWN_HOURS = 24
DEFAULT_HOST_TIMEOUT = 60  # seconds, nmap --host-timeout
DEFAULT_TOP_PORTS = 200

# discovered.osGuess VARCHAR(255) (015_nmap_enrich.sql).
OSGUESS_MAXLEN = 255
# discovered.openPorts / banners are TEXT -- generous but still bounded so a
# pathological host (hundreds of open ports/huge banner) can't blow past a
# sane row size.
OPENPORTS_MAXLEN = 8000
BANNERS_MAXLEN = 8000

HTTP_PORTS = (80, 443, 8080, 3000)
HTTP_TIMEOUT = 4  # seconds, per HEAD/GET probe

# The discovery scan cap enforced by serverScan.c (SCAN_MAX_HOSTS = 4096,
# i.e. CIDRs must be /20 or narrower) -- noted here only as context for why
# `discovered` itself is bounded; this script's own --limit is the knob that
# matters for a single run.
SCAN_MAX_HOSTS = 4096


def log(msg):
    print(f"{time.strftime('%Y-%m-%dT%H:%M:%SZ', time.gmtime())} nmap_enrich: {msg}",
          flush=True)


# --------------------------------------------------------------------------- #
# config / db -- same shape as oui_enrich.py / avahi_import.py               #
# --------------------------------------------------------------------------- #
def load_cfg(path):
    c = configparser.ConfigParser()
    if not c.read(path):
        log(f"FATAL: cannot read config {path} "
            f"(copy nmap_enrich.conf.example to nmap_enrich.conf)")
        sys.exit(1)
    return c


def db_password(c):
    """DB password from config, else the SOLARI_DB_PASS env var -- same
    convention as deploy/discovery/oui_enrich.py's db_password()."""
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
        read_timeout=90, write_timeout=30,
        cursorclass=pymysql.cursors.DictCursor,
    )


# --------------------------------------------------------------------------- #
# candidate selection                                                        #
# --------------------------------------------------------------------------- #
def _is_private_ipv4(ip):
    """True for an RFC1918/private/operator-owned-looking IPv4 -- link-local,
    loopback, multicast, reserved and anything unparsable are excluded. This
    tool only ever points nmap at the operator's own segments."""
    try:
        addr = ipaddress.ip_address(ip)
    except ValueError:
        return False
    if addr.version != 4:
        return False
    if not addr.is_private:
        return False
    if addr.is_loopback or addr.is_link_local or addr.is_multicast or addr.is_reserved:
        return False
    return True


def select_candidates(db, cooldown_hours, limit):
    """Rows with a routable private IPv4 and either never nmap-enriched or
    past the cooldown window, most-empty-first, capped at `limit`."""
    with db.cursor() as cur:
        cur.execute(
            "SELECT discId, ip, host, mac, vendor FROM discovered "
            "WHERE ip IS NOT NULL AND ip <> '' "
            "AND (nmapEnrichedAt IS NULL "
            "     OR nmapEnrichedAt < UTC_TIMESTAMP() - INTERVAL %s HOUR) "
            "ORDER BY (openPorts IS NULL) DESC, (osGuess IS NULL) DESC, "
            "         nmapEnrichedAt IS NULL DESC, lastSeenAt DESC",
            (cooldown_hours,))
        rows = cur.fetchall()

    candidates = [r for r in rows if _is_private_ipv4(r["ip"])]
    skipped_public = len(rows) - len(candidates)
    if skipped_public:
        log(f"skipped {skipped_public} row(s) with a non-private/unparsable ip "
            f"(this tool only scans the operator's own private segments)")

    return candidates[:limit]


# --------------------------------------------------------------------------- #
# nmap invocation + XML parsing                                              #
# --------------------------------------------------------------------------- #
def run_nmap(ip, top_ports, host_timeout):
    """Runs `nmap -sV -O --top-ports N -T4 --host-timeout Ss -oX -` against a
    single host and returns the raw XML text, or None if nmap couldn't be
    run/produced nothing (fail-soft -- caller just skips this host)."""
    if not shutil.which("nmap"):
        log("FATAL: nmap not found on PATH")
        return None

    cmd = ["nmap", "-sV", "-O", "--top-ports", str(top_ports), "-T4",
           "--host-timeout", f"{host_timeout}s", "-oX", "-", ip]
    try:
        proc = subprocess.run(cmd, capture_output=True, text=True,
                               timeout=host_timeout + 30, check=False)
    except subprocess.TimeoutExpired:
        log(f"WARNING: nmap hard-timed-out (hung past its own --host-timeout) for {ip}")
        return None
    except OSError as e:
        log(f"WARNING: nmap failed to launch for {ip}: {e!r}")
        return None

    if proc.returncode not in (0, None):
        log(f"WARNING: nmap exited {proc.returncode} for {ip}: "
            f"{(proc.stderr or '').strip()[:200]}")
    out = proc.stdout or ""
    if not out.strip():
        return None
    return out


def parse_nmap_xml(xml_text):
    """Parses one nmap `-oX -` run's XML for a single host. Returns
    (osGuess|None, [port summary strings], [banner lines]) -- never raises;
    a malformed/partial doc yields empty results (fail-soft)."""
    os_guess = None
    ports = []
    banners = []

    try:
        root = ET.fromstring(xml_text)
    except ET.ParseError as e:
        log(f"WARNING: could not parse nmap XML: {e!r}")
        return os_guess, ports, banners

    host_el = root.find("host")
    if host_el is None:
        return os_guess, ports, banners

    # --- OS guess: best (highest-accuracy) osmatch, if any ---
    os_el = host_el.find("os")
    if os_el is not None:
        best = None
        best_acc = -1
        for match in os_el.findall("osmatch"):
            try:
                acc = int(match.get("accuracy", "0"))
            except ValueError:
                acc = 0
            if acc > best_acc:
                best_acc = acc
                best = match.get("name")
        if best:
            os_guess = f"{best} ({best_acc}%)" if best_acc >= 0 else best

    # --- ports: proto/portid/state=open, service name/product/version ---
    ports_el = host_el.find("ports")
    if ports_el is not None:
        for port_el in ports_el.findall("port"):
            state_el = port_el.find("state")
            if state_el is None or state_el.get("state") != "open":
                continue
            proto = port_el.get("protocol", "tcp")
            portid = port_el.get("portid", "?")
            svc_el = port_el.find("service")
            name = product = version = ""
            if svc_el is not None:
                name = svc_el.get("name", "") or ""
                product = svc_el.get("product", "") or ""
                version = svc_el.get("version", "") or ""
            bits = [f"{portid}/{proto}"]
            if name:
                bits.append(name)
            if product:
                bits.append(product)
            if version:
                bits.append(version)
            ports.append("/".join(bits))

            if svc_el is not None:
                extra = svc_el.get("extrainfo", "") or ""
                if extra:
                    banners.append(f"{name or portid}: {extra}")

            # NSE script output (e.g. banner-grab scripts version detection
            # already triggers, ssh-hostkey, http-title, etc.)
            for script_el in port_el.findall("script"):
                sid = script_el.get("id", "script")
                out = (script_el.get("output") or "").strip()
                if out:
                    out = re.sub(r"\s+", " ", out)[:300]
                    banners.append(f"{name or portid}/{sid}: {out}")

    return os_guess, ports, banners


# --------------------------------------------------------------------------- #
# HTTP header/title probe (for the common web ports)                        #
# --------------------------------------------------------------------------- #
_TITLE_RE = re.compile(r"<title[^>]*>(.*?)</title>", re.IGNORECASE | re.DOTALL)


def probe_http(ip, port):
    """Quick HEAD (falling back to GET) against http(s)://ip:port/, returns a
    compact banner string ("Server: nginx; title: ...") or None. Never
    raises -- any failure (closed port, TLS error, timeout) is just "no
    banner from this port", not an error for the run."""
    scheme = "https" if port in (443, 8443) else "http"
    url = f"{scheme}://{ip}:{port}/"
    body = ""
    headers = {}
    for method in ("HEAD", "GET"):
        try:
            req = Request(url, method=method, headers={"User-Agent": "solarinet-nmap-enrich/1.0"})
            with urlopen(req, timeout=HTTP_TIMEOUT) as resp:  # noqa: S310 -- internal LAN probe by design
                headers = dict(resp.headers.items())
                if method == "GET":
                    body = resp.read(4096).decode("utf-8", errors="replace")
            break
        except HTTPError as e:
            # Still got a response (e.g. 401/403/404) -- headers are useful.
            headers = dict(e.headers.items()) if e.headers else {}
            if method == "GET":
                try:
                    body = e.read(4096).decode("utf-8", errors="replace")
                except Exception:  # noqa: BLE001 -- best-effort body read
                    body = ""
            break
        except (URLError, TimeoutError, ConnectionError, OSError, ValueError):
            continue
        except Exception:  # noqa: BLE001 -- any other transport/TLS oddity: no banner, not an error
            continue

    if not headers and not body:
        return None

    server = headers.get("Server", "") or headers.get("server", "")
    title_match = _TITLE_RE.search(body) if body else None
    title = re.sub(r"\s+", " ", title_match.group(1)).strip()[:120] if title_match else ""

    bits = []
    if server:
        bits.append(f"Server: {server}")
    if title:
        bits.append(f"title: {title}")
    if not bits:
        return None
    return f"http:{port}: {'; '.join(bits)}"


# --------------------------------------------------------------------------- #
# enrich one host / the run                                                  #
# --------------------------------------------------------------------------- #
def enrich_host(row, top_ports, host_timeout):
    """Scans one candidate row. Returns (osGuess|None, openPorts str|None,
    banners str|None) -- always returns a tuple (possibly all-None), never
    raises (fail-soft, caught again by the caller as a belt-and-suspenders)."""
    ip = row["ip"]
    xml_text = run_nmap(ip, top_ports, host_timeout)
    os_guess = None
    port_list = []
    banner_list = []
    if xml_text:
        os_guess, port_list, banner_list = parse_nmap_xml(xml_text)

    open_port_nums = set()
    for p in port_list:
        m = re.match(r"^(\d+)/", p)
        if m:
            open_port_nums.add(int(m.group(1)))

    for port in HTTP_PORTS:
        if port not in open_port_nums:
            continue
        try:
            hb = probe_http(ip, port)
        except Exception as e:  # noqa: BLE001 -- fail-soft, one bad probe never aborts the host
            log(f"WARNING: http probe {ip}:{port} raised {e!r}")
            hb = None
        if hb:
            banner_list.append(hb)

    open_ports_str = ",".join(port_list)[:OPENPORTS_MAXLEN] if port_list else None
    banners_str = "\n".join(banner_list)[:BANNERS_MAXLEN] if banner_list else None
    os_guess_str = os_guess[:OSGUESS_MAXLEN] if os_guess else None

    return os_guess_str, open_ports_str, banners_str


def enrich(db, rows, top_ports, host_timeout, dry_run):
    updated = errors = no_data = 0

    with db.cursor() as cur:
        for row in rows:
            disc_id, ip = row["discId"], row["ip"]
            try:
                os_guess, open_ports, banners = enrich_host(row, top_ports, host_timeout)
            except Exception as e:  # noqa: BLE001 -- one bad host must never abort the run
                log(f"ERROR scanning discId={disc_id} ip={ip}: {e!r}")
                errors += 1
                continue

            if not (os_guess or open_ports or banners):
                log(f"NODATA discId={disc_id} ip={ip}: nmap produced no usable ports/os/banner "
                    f"(host down, filtered, or needs root for -O)")
                no_data += 1
                # Still stamp nmapEnrichedAt so a persistently-silent host
                # doesn't get re-swept every single run within the cooldown.
                if not dry_run:
                    try:
                        cur.execute(
                            "UPDATE discovered SET nmapEnrichedAt=UTC_TIMESTAMP() "
                            "WHERE discId=%s", (disc_id,))
                    except Exception as e:  # noqa: BLE001 -- fail-soft per row
                        log(f"ERROR stamping discId={disc_id}: {e!r}")
                        errors += 1
                continue

            if dry_run:
                log(f"WOULD update discId={disc_id} ip={ip}: "
                    f"osGuess={os_guess!r} openPorts={open_ports!r} "
                    f"banners={(banners or '')[:200]!r}")
                updated += 1
                continue

            try:
                cur.execute(
                    "UPDATE discovered SET osGuess=%s, openPorts=%s, banners=%s, "
                    "nmapEnrichedAt=UTC_TIMESTAMP() WHERE discId=%s",
                    (os_guess, open_ports, banners, disc_id))
                updated += 1
            except Exception as e:  # noqa: BLE001 -- fail-soft per row
                log(f"ERROR updating discId={disc_id} ip={ip}: {e!r}")
                errors += 1

    if dry_run:
        db.rollback()
    else:
        db.commit()

    log(f"enrich complete: {updated} {'would-update' if dry_run else 'updated'}, "
        f"{no_data} scanned-but-no-data, {errors} errors")
    return updated, no_data, errors


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                  formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--conf", default=DEFAULT_CONF,
                     help=f"config file (default: {DEFAULT_CONF})")
    ap.add_argument("--dry-run", action="store_true",
                     help="report changes without writing")
    ap.add_argument("--limit", type=int, default=DEFAULT_LIMIT,
                     help=f"max hosts to scan this run (default: {DEFAULT_LIMIT})")
    ap.add_argument("--cooldown-hours", type=float, default=DEFAULT_COOLDOWN_HOURS,
                     help="skip rows nmap-enriched more recently than this "
                          f"(default: {DEFAULT_COOLDOWN_HOURS})")
    ap.add_argument("--host-timeout", type=int, default=DEFAULT_HOST_TIMEOUT,
                     help=f"nmap --host-timeout seconds per host (default: {DEFAULT_HOST_TIMEOUT})")
    ap.add_argument("--top-ports", type=int, default=DEFAULT_TOP_PORTS,
                     help=f"nmap --top-ports N (default: {DEFAULT_TOP_PORTS})")
    args = ap.parse_args()

    if pymysql is None:
        log("FATAL: pymysql not installed (pip install pymysql in deploy/discovery/.venv)")
        sys.exit(1)

    if not shutil.which("nmap"):
        log("nmap not found on PATH -- nothing to do, exiting cleanly")
        sys.exit(0)

    cfg = load_cfg(args.conf)
    try:
        db = db_connect(cfg)
    except Exception as e:  # noqa: BLE001 -- top-level: nothing to do without a DB
        log(f"FATAL: cannot connect to DB: {e!r}")
        sys.exit(1)

    try:
        rows = select_candidates(db, args.cooldown_hours, args.limit)
        log(f"{len(rows)} candidate host(s) selected (limit={args.limit}, "
            f"cooldown={args.cooldown_hours}h)")
        if not rows:
            sys.exit(0)
        _, _, errors = enrich(db, rows, args.top_ports, args.host_timeout, args.dry_run)
    finally:
        db.close()

    sys.exit(1 if errors else 0)


if __name__ == "__main__":
    main()
