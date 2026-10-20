#!/usr/bin/env python3
"""Render BIND zones from the Akoria network source-of-truth.

Production uses the SoR backend; netdb/akoria-hosts.yml is a legacy fixture.
`load_source()` selects the backend. Emits into --output (netdb/zones/ by default):
  - db.akoria.net           forward zone (A + CNAME)
  - db.<rev>.in-addr.arpa   one reverse (PTR) zone per /24 in use
  - named.conf.akoria       zone{} declarations for BIND
Unchanged zones retain their applied SOA serial. Changed zones advance from
--previous, which must identify the last applied generation in production.
"""
import argparse, ipaddress, os, re, sys, yaml
from zone_serial import SERIAL_TOKEN, atomicWrite, stampZone

HERE = os.path.dirname(os.path.abspath(__file__))
SRC  = os.path.join(HERE, "akoria-hosts.yml")
OUT  = os.path.join(HERE, "zones")
HOSTMASTER = "hostmaster.akoria.net."
ZONEDIR    = "/etc/bind/zones"          # where the files land on the BIND host
# Authoritative nameservers for akoria.net (must be real A-record hosts, never
# CNAMEs). xenon = primary/master, radium = secondary (plain-BIND AXFR works).
NS_HOSTS   = ["xenon", "radium"]
SECONDARIES = ["10.1.0.10", "10.0.0.11"]  # radium + steel (ZimaBlade) = akoria.net secondaries
# Zone-apex A records: the bare domain (e.g. https://akoria.net/) resolves to a
# host IP. akoria.net -> xenon (10.0.0.20), the web / service-directory host.
APEX_A = {"akoria.net": "10.0.0.20"}


def load_source(path=SRC):
    """Source-of-truth boundary. Returns (domain, hosts, ifaces, cnames).

    Two backends, selected by the NETDB_SOURCE env var (or --source arg):
      * "yaml" (default) -- read akoria-hosts.yml, the legacy offline fixture.
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


def _soa(origin):
    primary = f"{NS_HOSTS[0]}.akoria.net."
    return [f"$ORIGIN {origin}.", "$TTL 300", "",
            f"@   IN SOA {primary} {HOSTMASTER} (",
            f"            {SERIAL_TOKEN} ; serial",
            "            3600       ; refresh",
            "            600        ; retry",
            "            604800     ; expire",
            "            300 )      ; minimum", ""]


def forward(domain, hosts, ifaces, cnames):
    L = _soa(domain)
    for h in NS_HOSTS:
        L.append(f"@              IN NS    {h}")
    L.append("")
    if domain in APEX_A:
        L.append("; ---- zone apex ----")
        L.append(f"@              IN A     {APEX_A[domain]}")
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
    global ZONEDIR
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", choices=["sor", "yaml"])
    parser.add_argument("--output", default=OUT)
    parser.add_argument("--previous", help="last applied zone directory; defaults to output")
    parser.add_argument("--zone-dir", default=ZONEDIR,
                        help="BIND runtime zone path in generated declarations")
    args = parser.parse_args()
    if args.source:
        os.environ["NETDB_SOURCE"] = args.source
    if not re.fullmatch(r"/[A-Za-z0-9_./-]+", args.zone_dir):
        parser.error("zone directory must be an absolute BIND-safe path")
    ZONEDIR = args.zone_dir
    domain, hosts, ifaces, cnames = load_source()
    label = re.compile(r"[A-Za-z0-9_-]+(?:\.[A-Za-z0-9_-]+)*\.?$")
    for name in [domain, *hosts, *ifaces, *cnames, *cnames.values()]:
        if not isinstance(name, str) or not label.fullmatch(name):
            raise ValueError(f"invalid DNS name: {name!r}")
    for address in [*(host["ip"] for host in hosts.values()), *ifaces.values()]:
        ipaddress.IPv4Address(address)
    previous = args.previous or args.output
    zones = {domain: forward(domain, hosts, ifaces, cnames)}
    revs = reverses(domain, hosts, ifaces)
    zones.update(revs)
    # Finish all serial decisions before writing, even for the legacy in-place
    # invocation. Production callers render into a new isolated generation.
    zones = {origin: stampZone(text, os.path.join(previous, f"db.{origin}"))
             for origin, text in zones.items()}
    os.makedirs(args.output, exist_ok=True)
    for origin, text in zones.items():
        atomicWrite(os.path.join(args.output, f"db.{origin}"), text)
    atomicWrite(os.path.join(args.output, "named.conf.akoria"), named_conf(domain, revs))
    print(f"rendered db.{domain} + {len(revs)} reverse zones + named.conf.akoria")
    print(f"  A records: {len(hosts) + len(ifaces)}   CNAMEs: {len(cnames)}   reverse /24s: {len(revs)}")


if __name__ == "__main__":
    main()
