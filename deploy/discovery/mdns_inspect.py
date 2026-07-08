#!/usr/bin/env python3
"""mdns_inspect — LIVE mDNS/zeroconf/Bonjour service inspector.

REPLACES the one-shot avahi_import.py file-import workflow with an active
browse: instead of reading a saved Avahi Browser export, this actually
listens on the wire for mDNS (RFC 6762) / DNS-SD (RFC 6763) traffic for a
bounded window (--timeout, default 8s) and feeds whatever hosts+services it
resolves straight into the SolariNet Discovery asset table (`discovered`),
same as an Avahi Browser export would have.

Two browse backends, tried in order, so the tool degrades gracefully
depending on what's installed on the probe host:

1. **python-zeroconf** (`pip install zeroconf`), if importable — a pure-Python
   mDNS/DNS-SD stack. `ZeroconfServiceTypes.find()` discovers the advertised
   service *types* on the LAN, then each type is browsed and every instance
   found is resolved (name, addresses, port) via `Zeroconf.get_service_info()`.
2. **avahi-browse(1)**, shelled out to (`avahi-browse -aprtk`: **a**ll types,
   **p**arsable, **r**esolve, **t**erminate-when-caught-up, **k** raw type
   names — no fuzzy service-type-database name translation), if the binary is
   on PATH. Its `=;iface;proto;name;type;domain;host;address;port;txt` lines
   are parsed the same way `serverScan.c`'s `scanEnrichMdns()` parses
   `avahi-browse -aprt` output (this script is the equivalent capability as a
   standalone, periodic, DB-writing tool rather than a per-host synchronous
   enrichment call).

Neither present -> logs a single line and exits 0 (fail-soft: a probe host
without mDNS tooling installed must never fail a discovery run over it).

The browse result is normalized into the exact `{"hosts": [{"name",
"addresses", "services": [{"type"}]}, ...]}` shape avahi_import.py's
load_export() produces from a file, and handed to avahi_import's OWN
import_export()/db_connect()/load_cfg() — the upsert logic (correlate by
IPv4; UPDATE mdnsName+mdnsServices+lastSeenAt on match; INSERT new
via='mdns' status='new'; IPv6-only/addressless skipped) is not reimplemented
here, it's reused verbatim by importing avahi_import as a module.

Usage:
  mdns_inspect.py [--once] [--timeout SECONDS] [--interval SECONDS]
                   [--dry-run] [--conf PATH]

--once runs a single browse-and-upsert pass and exits (this is what the
systemd oneshot unit / a discovery-probe hook invokes). Without --once the
process loops forever, sleeping --interval seconds (default 60) between
passes -- a standalone "keep the mDNS view fresh" mode for running outside
of systemd oneshot triggering.

Config: mdns_inspect.conf (see .example) -- same [db] shape as
avahi_import.conf; DB password: config, else $SOLARI_DB_PASS (source
run/db.env first, same convention as deploy/alertbridge/alertbridge.py).
"""
import argparse
import os
import re
import shutil
import signal
import subprocess
import sys
import time

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
if SCRIPT_DIR not in sys.path:
    sys.path.insert(0, SCRIPT_DIR)

import avahi_import as ai  # noqa: E402 -- reuse its config/db/upsert logic verbatim

DEFAULT_CONF = os.environ.get("MDNS_INSPECT_CONF",
                              os.path.join(SCRIPT_DIR, "mdns_inspect.conf"))
DEFAULT_TIMEOUT = 8
DEFAULT_INTERVAL = 60

_run = True


def log(msg):
    print(f"{time.strftime('%Y-%m-%dT%H:%M:%SZ', time.gmtime())} mdns_inspect: {msg}",
          flush=True)


def _handle_signal(signum, _frame):
    global _run
    log(f"received signal {signum}; stopping after current pass")
    _run = False


# --------------------------------------------------------------------------- #
# avahi-browse escaping                                                       #
# --------------------------------------------------------------------------- #
_DECIMAL_ESCAPE_RE = re.compile(r"\\(\d{3})")


def _unescape_avahi(s):
    """avahi-browse -p escapes non-ASCII/space/`;`/`\\` bytes as `\\ddd` --
    ddd is the byte's value in DECIMAL (e.g. space -> \\032, '(' -> \\040,
    ')' -> \\041; a multi-byte UTF-8 char is one \\ddd per byte, e.g. U+2019
    RIGHT SINGLE QUOTATION MARK -> \\226\\128\\153). Decode back to text;
    never raise -- a malformed escape just passes through as-is."""
    if not s:
        return s
    try:
        raw = _DECIMAL_ESCAPE_RE.sub(lambda m: chr(int(m.group(1))), s)
        return raw.encode("latin-1", errors="replace").decode("utf-8", errors="replace")
    except Exception:  # noqa: BLE001 -- cosmetic decoding only, never fatal
        return s


# --------------------------------------------------------------------------- #
# backend 1: python-zeroconf                                                  #
# --------------------------------------------------------------------------- #
def browse_zeroconf(timeout):
    """Live browse via python-zeroconf. Returns the avahi_import hosts list,
    or None if the zeroconf package isn't importable (caller falls back)."""
    try:
        from zeroconf import Zeroconf, ServiceBrowser, ServiceListener
        from zeroconf import ZeroconfServiceTypes
    except ImportError:
        return None

    type_budget = max(1.0, timeout * 0.4)
    browse_budget = max(1.0, timeout - type_budget)

    zc = Zeroconf()
    hosts = {}  # ip -> {"name": ..., "addresses": [ip], "services": [{"type": t}]}

    try:
        try:
            svc_types = list(ZeroconfServiceTypes.find(zc=zc, timeout=type_budget))
        except Exception as e:  # noqa: BLE001 -- a broken type query must not abort the browse
            log(f"WARNING: zeroconf service-type discovery failed: {e!r}")
            svc_types = []

        found = []  # (type_, name)

        class _Listener(ServiceListener):
            def add_service(self, zconf, type_, name):
                found.append((type_, name))

            def update_service(self, zconf, type_, name):
                pass

            def remove_service(self, zconf, type_, name):
                pass

        browsers = []
        if svc_types:
            listener = _Listener()
            browsers = [ServiceBrowser(zc, t, listener) for t in svc_types]
            time.sleep(browse_budget)
        for b in browsers:
            try:
                b.cancel()
            except Exception:  # noqa: BLE001 -- best-effort teardown
                pass

        for type_, name in found:
            try:
                info = zc.get_service_info(type_, name, timeout=2000)
            except Exception as e:  # noqa: BLE001 -- one bad resolve must not abort the browse
                log(f"WARNING: zeroconf resolve failed for {name} ({type_}): {e!r}")
                continue
            if not info:
                continue
            svc_type = (type_[:-1] if type_.endswith(".") else type_)
            friendly = info.server.rstrip(".") if info.server else name
            for addr in info.parsed_scoped_addresses() if hasattr(info, "parsed_scoped_addresses") \
                    else info.parsed_addresses():
                addr = addr.split("%", 1)[0]  # strip an IPv6 scope id if present
                if not ai.IPV4_RE.match(addr):
                    continue
                h = hosts.setdefault(addr, {"name": friendly, "addresses": [addr], "services": []})
                if not h["name"]:
                    h["name"] = friendly
                if not any(s["type"] == svc_type for s in h["services"]):
                    h["services"].append({"type": svc_type})
    finally:
        zc.close()

    return list(hosts.values())


# --------------------------------------------------------------------------- #
# backend 2: avahi-browse                                                     #
# --------------------------------------------------------------------------- #
def browse_avahi(timeout):
    """Live browse by shelling out to avahi-browse -aprtk. Returns the
    avahi_import hosts list, or None if the binary isn't on PATH (caller
    falls back to the "nothing available" fail-soft exit)."""
    if not shutil.which("avahi-browse"):
        return None

    cmd = ["avahi-browse", "-aprtk"]
    try:
        # -t makes avahi-browse terminate on its own once the cache looks
        # complete; --timeout is still a hard backstop against a daemon that
        # never settles (e.g. a very chatty LAN).
        proc = subprocess.run(cmd, capture_output=True, text=True,
                               timeout=timeout, check=False)
    except subprocess.TimeoutExpired as e:
        out = (e.stdout or "")
        log(f"avahi-browse hit the {timeout}s backstop timeout; "
            f"using what it produced so far")
        return _parse_avahi_output(out)
    except OSError as e:
        log(f"WARNING: avahi-browse failed to launch: {e!r}")
        return None

    if proc.returncode not in (0, None):
        log(f"WARNING: avahi-browse exited {proc.returncode}: "
            f"{(proc.stderr or '').strip()[:200]}")
    return _parse_avahi_output(proc.stdout or "")


def _parse_avahi_output(out):
    hosts = {}  # ip -> {"name": ..., "addresses": [ip], "services": [{"type": t}]}
    for line in out.splitlines():
        if not line.startswith("=;"):
            continue  # only "resolved" lines carry an address; "+;" are bare sightings
        fields = line.split(";")
        if len(fields) < 9:
            continue
        _flag, _iface, proto, name, svc_type, _domain, hostname, address, _port = fields[:9]
        if proto != "IPv4" or not address:
            continue
        if not ai.IPV4_RE.match(address):
            continue
        name = _unescape_avahi(name)
        hostname = _unescape_avahi(hostname).rstrip(".")
        svc_type = _unescape_avahi(svc_type)
        friendly = hostname or name
        h = hosts.setdefault(address, {"name": friendly, "addresses": [address], "services": []})
        if not h["name"]:
            h["name"] = friendly
        if svc_type and not any(s["type"] == svc_type for s in h["services"]):
            h["services"].append({"type": svc_type})
    return list(hosts.values())


# --------------------------------------------------------------------------- #
# one pass                                                                    #
# --------------------------------------------------------------------------- #
def run_pass(cfg, timeout, dry_run):
    hosts = browse_zeroconf(timeout)
    backend = "python-zeroconf"
    if hosts is None:
        hosts = browse_avahi(timeout)
        backend = "avahi-browse"
    if hosts is None:
        log("no mDNS backend available (neither python-zeroconf nor "
            "avahi-browse found) -- nothing to do, exiting cleanly")
        return None

    log(f"{backend}: browsed {timeout}s, resolved {len(hosts)} host(s) with an IPv4 address")

    try:
        db = ai.db_connect(cfg)
    except Exception as e:  # noqa: BLE001 -- nothing to do this pass without a DB
        log(f"ERROR: cannot connect to DB: {e!r}; skipping this pass")
        return None

    try:
        _, _, _, errors = ai.import_export(db, hosts, dry_run)
    finally:
        db.close()
    return errors


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                  formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--once", action="store_true",
                     help="run a single browse-and-upsert pass and exit "
                          "(what the systemd oneshot unit / a discovery-probe hook uses)")
    ap.add_argument("--timeout", type=float, default=DEFAULT_TIMEOUT,
                     help=f"seconds to actively browse mDNS per pass (default: {DEFAULT_TIMEOUT})")
    ap.add_argument("--interval", type=float, default=DEFAULT_INTERVAL,
                     help=f"seconds to sleep between passes when not --once (default: {DEFAULT_INTERVAL})")
    ap.add_argument("--dry-run", action="store_true",
                     help="report changes without writing")
    ap.add_argument("--conf", default=DEFAULT_CONF,
                     help=f"config file (default: {DEFAULT_CONF})")
    args = ap.parse_args()

    cfg = ai.load_cfg(args.conf)

    signal.signal(signal.SIGTERM, _handle_signal)
    signal.signal(signal.SIGINT, _handle_signal)

    had_errors = False
    while True:
        errors = run_pass(cfg, args.timeout, args.dry_run)
        if errors:
            had_errors = True
        if args.once or not _run:
            break
        log(f"sleeping {args.interval}s until next pass")
        time.sleep(args.interval)
        if not _run:
            break

    sys.exit(1 if had_errors else 0)


if __name__ == "__main__":
    main()
