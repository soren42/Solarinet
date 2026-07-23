#!/usr/bin/env python3
"""Render BIND zones from the Akoria network source-of-truth.

The interim source is netdb/akoria-hosts.yml. `load_source()` is the ONLY seam
that knows the source format — swap its body for a MySQL query later and every
downstream renderer is unchanged. Emits into netdb/zones/:
  - db.akoria.net           forward zone (A + CNAME)
  - db.<rev>.in-addr.arpa   one reverse (PTR) zone per /24 in use
  - named.conf.akoria       zone{} declarations for BIND
Serial is date-based (YYYYMMDDnn); bump nn for multiple renders in a day.
"""
import os, re, sys, datetime, yaml

HERE = os.path.dirname(os.path.abspath(__file__))
SRC  = os.path.join(HERE, "akoria-hosts.yml")
OUT  = os.path.join(HERE, "zones")
HOSTMASTER = "hostmaster.akoria.net."
ZONEDIR    = "/etc/bind/zones"          # where the files land on the BIND host
# Authoritative nameservers for akoria.net (must be real A-record hosts, never
# CNAMEs). xenon = primary/master, radium = secondary (plain-BIND AXFR works).
NS_HOSTS   = ["xenon", "radium"]
SECONDARIES = ["10.1.0.10", "10.0.0.11"]  # radium + steel (ZimaBlade) = akoria.net secondaries


def load_source(path=SRC):
    """Source-of-truth boundary. Returns (domain, hosts, ifaces, cnames).

    Two backends, selected by the NETDB_SOURCE env var (or --source arg):
      * "yaml" (default) -- read akoria-hosts.yml, the interim source-of-truth.
      * "sor"            -- render FROM the System of Record (MariaDB `sor` on
                            cesium) via sor/source_sor.py. This closes the loop:
                            DNS becomes a rendered view of the SoR. Requires
                            SOR_DB_PASSWORD (see sor/source_sor.py).
    """
    backend = os.environ.get("NETDB_SOURCE", "yaml").lower()
    if backend == "sor":
        sys.path.insert(0, os.path.join(HERE, "sor"))
        from source_sor import load_source as sor_load_source
        return sor_load_source()
    d = yaml.safe_load(open(path))
    return d["domain"], d["hosts"], d.get("ifaces", {}), d["cnames"]


def _prev_serial(origin):
    """Read the serial from the previously-rendered zone file (OUT persists
    between runs), so we can bump monotonically within the same day."""
    try:
        with open(os.path.join(OUT, f"db.{origin}")) as f:
            for line in f:
                m = re.search(r"(\d{10,})\s*;\s*serial", line)
                if m:
                    return int(m.group(1))
    except FileNotFoundError:
        pass
    return None


def serial(origin):
    """Monotonic YYYYMMDDnn: date-based, but strictly increasing so multiple
    same-day changes each bump the serial (secondaries only AXFR on a higher
    serial — a static nn silently breaks replication for same-day edits)."""
    base = int(datetime.date.today().strftime("%Y%m%d")) * 100
    prev = _prev_serial(origin)
    return prev + 1 if (prev is not None and prev >= base) else base + 1


def _soa(origin):
    primary = f"{NS_HOSTS[0]}.akoria.net."
    return [f"$ORIGIN {origin}.", "$TTL 300", "",
            f"@   IN SOA {primary} {HOSTMASTER} (",
            f"            {serial(origin)} ; serial",
            "            3600       ; refresh",
            "            600        ; retry",
            "            604800     ; expire",
            "            300 )      ; minimum", ""]


def forward(domain, hosts, ifaces, cnames):
    L = _soa(domain)
    for h in NS_HOSTS:
        L.append(f"@              IN NS    {h}")
    L.append("")
    L.append("; ---- hosts (identity) ----")
    for n in sorted(hosts):
        L.append(f"{n:<14} IN A     {hosts[n]['ip']}")
    L += ["", "; ---- extra interfaces ----"]
    for n in sorted(ifaces):
        L.append(f"{n:<14} IN A     {ifaces[n]}")
    L += ["", "; ---- functional aliases (CNAME) ----"]
    for fn in sorted(cnames):
        L.append(f"{fn:<14} IN CNAME {cnames[fn]}")
    return "\n".join(L) + "\n"


def reverses(domain, hosts, ifaces):
    a = {n: v["ip"] for n, v in hosts.items()}
    a.update(ifaces)
    by24 = {}
    for name, ip in a.items():
        o = ip.split(".")
        by24.setdefault(".".join(o[:3]), []).append((int(o[3]), name))
    out = {}
    for net, recs in by24.items():
        o = net.split(".")
        rev = f"{o[2]}.{o[1]}.{o[0]}.in-addr.arpa"
        L = _soa(rev) + [f"@ IN NS {h}.akoria.net." for h in NS_HOSTS] + [""]
        for last, name in sorted(recs):
            L.append(f"{last:<4} IN PTR {name}.{domain}.")
        out[rev] = "\n".join(L) + "\n"
    return out


def named_conf(domain, revs):
    xfer = "; ".join(f"{ip}" for ip in SECONDARIES)
    L = [f'zone "{domain}" {{ type primary; file "{ZONEDIR}/db.{domain}"; '
         f'allow-transfer {{ {xfer}; }}; also-notify {{ {xfer}; }}; notify yes; }};']
    for rev in sorted(revs):
        L.append(f'zone "{rev}" {{ type primary; file "{ZONEDIR}/db.{rev}"; '
                 f'allow-transfer {{ {xfer}; }}; also-notify {{ {xfer}; }}; notify yes; }};')
    return "\n".join(L) + "\n"


def main():
    # allow `gen-zones.py --source sor` as a shorthand for NETDB_SOURCE=sor
    if "--source" in sys.argv:
        i = sys.argv.index("--source")
        if i + 1 < len(sys.argv):
            os.environ["NETDB_SOURCE"] = sys.argv[i + 1]
    domain, hosts, ifaces, cnames = load_source()
    os.makedirs(OUT, exist_ok=True)
    # Render BEFORE truncating the file: forward()/reverses() read the prior
    # serial out of the existing OUT files, and open(...,"w") would truncate
    # the forward zone before forward() could read its old serial.
    fwd = forward(domain, hosts, ifaces, cnames)
    open(os.path.join(OUT, f"db.{domain}"), "w").write(fwd)
    revs = reverses(domain, hosts, ifaces)
    for rev, txt in revs.items():
        open(os.path.join(OUT, f"db.{rev}"), "w").write(txt)
    open(os.path.join(OUT, "named.conf.akoria"), "w").write(named_conf(domain, revs))
    print(f"rendered db.{domain} + {len(revs)} reverse zones + named.conf.akoria")
    print(f"  A records: {len(hosts) + len(ifaces)}   CNAMEs: {len(cnames)}   reverse /24s: {len(revs)}")


if __name__ == "__main__":
    main()
