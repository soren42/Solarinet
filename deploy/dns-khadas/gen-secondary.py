#!/usr/bin/env python3
"""Generate secondary zone declarations from SolariNet's primary zone list."""
import argparse
import ipaddress
import os
import pathlib
import re
import tempfile


ZONE_RE = re.compile(
    r'^\s*zone\s+"([A-Za-z0-9._-]+)"\s*\{\s*type\s+(?:primary|master)\s*;', re.M)


def generate(primary_config: str, primary_ip: str, zone_dir: str) -> str:
    primary = str(ipaddress.IPv4Address(primary_ip))
    path = pathlib.PurePosixPath(zone_dir)
    if (not zone_dir.startswith("/") or not re.fullmatch(r"/[A-Za-z0-9_./-]+", zone_dir)
            or ".." in path.parts or "." in path.parts):
        raise ValueError("zone directory must be an absolute BIND-safe path")
    # Accept multiple BIND fragments (for example Xenon's generated forward /
    # reverse list plus the SoR-managed bare-zone include and RPZ stanza).
    if isinstance(primary_config, str):
        primary_config = [primary_config]
    zones = sorted({zone for config in primary_config for zone in ZONE_RE.findall(config)})
    if not zones:
        raise ValueError("primary config contains no primary zones")
    if any(".." in zone or zone.startswith(".") for zone in zones):
        raise ValueError("invalid zone name in primary config")
    return "// Generated from the active SolariNet primary zone list; do not edit.\n" + "".join(
        f'zone "{zone}" {{ type secondary; primaries {{ {primary}; }}; '
        f'file "{zone_dir}/db.{zone}"; }};\n' for zone in zones
    )


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("primary_config", help="BIND primary zone config")
    parser.add_argument("output", help="destination zones.conf")
    parser.add_argument("--additional-config", action="append", default=[],
                        help="additional BIND primary-zone fragment (repeatable)")
    parser.add_argument("--primary", default="10.0.0.20")
    parser.add_argument("--zone-dir", default="/var/cache/bind/solarinet-secondary")
    args = parser.parse_args()
    sources = [pathlib.Path(name).read_text(encoding="utf-8")
               for name in [args.primary_config, *args.additional_config]]
    output = pathlib.Path(args.output)
    content = generate(sources, args.primary, args.zone_dir)
    fd, temporary = tempfile.mkstemp(prefix=".zones-", dir=output.parent)
    try:
        with os.fdopen(fd, "w", encoding="utf-8") as stream:
            stream.write(content)
            stream.flush()
            os.fsync(stream.fileno())
        os.replace(temporary, output)
        directory = os.open(output.parent, os.O_RDONLY)
        try:
            os.fsync(directory)
        finally:
            os.close(directory)
    finally:
        if os.path.exists(temporary):
            os.unlink(temporary)


if __name__ == "__main__":
    main()
