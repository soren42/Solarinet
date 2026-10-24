#!/usr/bin/env python3
"""Render bare-name zones into an isolated directory, retaining applied serials.

Usage: gen-bare.py FORWARD_ZONE OUTPUT [--previous APPLIED_ZONES]
The runtime zone path defaults to /etc/bind/zones and is configurable for an
atomic release directory. Calls without --transfer authorize the active Khadas
secondary only. Add further addresses explicitly after they are provisioned. No source or previous-
generation file is modified.
"""
import argparse
import ipaddress
import pathlib
import re
import sys

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[2] / "netdb"))
from zone_serial import SERIAL_TOKEN, atomicWrite, stampZone


def renderNames(source):
    """Resolve local A/CNAME chains; reject cycles instead of dropping records."""
    addresses, aliases = {}, {}
    for line in pathlib.Path(source).read_text(encoding="utf-8").splitlines():
        match = re.match(r"^(\S+)\s+IN\s+A\s+(\S+)", line)
        if match:
            addresses[match[1].lower()] = str(ipaddress.IPv4Address(match[2]))
        match = re.match(r"^(\S+)\s+IN\s+CNAME\s+(\S+)", line)
        if match:
            aliases[match[1].lower()] = match[2].rstrip(".").split(".")[0].lower()

    def resolve(name, visited):
        if name in addresses:
            return addresses[name]
        if name in visited:
            raise ValueError(f"CNAME cycle while rendering bare zone: {name}")
        if name not in aliases:
            return None
        return resolve(aliases[name], visited | {name})

    result = {}
    for name in sorted(addresses.keys() | aliases.keys()):
        if name in ("@", "*") or not re.fullmatch(r"[a-z0-9_-]{1,63}", name):
            continue
        address = resolve(name, set())
        if address is not None:
            result[name] = address
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source")
    parser.add_argument("output")
    parser.add_argument("--previous")
    parser.add_argument("--zone-dir", default="/etc/bind/zones")
    parser.add_argument("--transfer", action="append", default=None,
                        help="IP authorized for AXFR and NOTIFY; repeatable")
    args = parser.parse_args()
    if not re.fullmatch(r"/[A-Za-z0-9_./-]+", args.zone_dir):
        parser.error("zone directory must be an absolute BIND-safe path")
    names = renderNames(args.source)
    if not names:
        raise ValueError("forward zone contains no usable bare-name records")
    transfers = args.transfer if args.transfer is not None else ["10.7.0.10"]
    transfer_ips = sorted({str(ipaddress.IPv4Address(address)) for address in transfers})
    transfer = "; ".join(transfer_ips)
    previous = pathlib.Path(args.previous or args.output)
    output = pathlib.Path(args.output)
    zones, stanzas = {}, []
    for name, address in names.items():
        template = (f"$ORIGIN {name}.\n$TTL 300\n"
                    "@ IN SOA xenon.akoria.net. hostmaster.akoria.net. (\n"
                    f"  {SERIAL_TOKEN} ; serial\n"
                    "  3600 600 604800 300 )\n"
                    f"@ IN NS xenon.akoria.net.\n@ IN A {address}\n")
        filename = f"db.bare-{name}"
        zones[filename] = stampZone(template, previous / filename)
        policy = (f" allow-transfer {{ {transfer}; }}; also-notify {{ {transfer}; }}; notify yes;"
                  if transfer else " allow-transfer { none; }; notify no;")
        stanzas.append(f'zone "{name}" {{ type primary; file "{args.zone_dir}/{filename}";{policy} }};')
    for filename, content in zones.items():
        atomicWrite(output / filename, content)
    atomicWrite(output / "named.conf.bare",
                "// Generated bare-name zones; replace as a complete generation.\n"
                + "\n".join(stanzas) + "\n")
    print(f"bare zones: {len(names)} names")


if __name__ == "__main__":
    main()
