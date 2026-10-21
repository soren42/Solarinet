#!/usr/bin/env python3
"""Push changed primary-zone declarations to chlorine and verify AXFR state."""
import argparse
import importlib.util
import pathlib
import re
import subprocess
import sys
import time
from concurrent.futures import ThreadPoolExecutor

HERE = pathlib.Path(__file__).resolve().parent
SPEC = importlib.util.spec_from_file_location("gen_secondary", HERE / "gen-secondary.py")
GEN = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(GEN)
ZONE_RE = re.compile(r'^zone "([A-Za-z0-9._-]+)" \{ type secondary;', re.M)
PRIMARY_ZONE_RE = re.compile(
    r'^\s*zone\s+"([A-Za-z0-9._-]+)"\s*\{\s*type\s+(?:primary|master)\b', re.M)


def active_primary_zones(expanded_config: str | None = None) -> set[str]:
    """Read the active BIND primary-zone inventory independently of fragments.

    named-checkconf expands the production include graph. Its output is parsed
    in memory only; it can contain private key material and must never be logged.
    Tests may provide a fixture in place of the command output.
    """
    if expanded_config is None:
        result = subprocess.run(["named-checkconf", "-p"], capture_output=True,
                                text=True, timeout=15)
        if result.returncode:
            raise RuntimeError("could not inspect active BIND zone inventory")
        expanded_config = result.stdout
    return set(PRIMARY_ZONE_RE.findall(expanded_config))


def serial(server: str, zone: str) -> str | None:
    result = subprocess.run(["dig", "+time=2", "+tries=1", "+short",
                             f"@{server}", zone, "SOA"],
                            capture_output=True, text=True, timeout=5)
    fields = result.stdout.split()
    if result.returncode != 0 or len(fields) < 3 or not fields[2].isdigit():
        return None
    return fields[2]


def synchronize(primary_config: pathlib.Path, primary: str, secondary: str,
                ssh_host: str, helper: str, timeout_sec: int = 60,
                additional_configs: tuple[pathlib.Path, ...] = (),
                expanded_config: str | None = None) -> int:
    configs = (primary_config, *additional_configs)
    sources = []
    for path in configs:
        if path.is_symlink() or not path.is_file():
            raise ValueError("primary zone configs must be regular non-symlink files")
        sources.append(path.read_text(encoding="utf-8"))
    content = GEN.generate(sources, primary, "/var/cache/bind/solarinet-secondary")
    zones = sorted(set(ZONE_RE.findall(content)))
    if not zones:
        raise ValueError("generated secondary config is empty")
    active_zones = active_primary_zones(expanded_config)
    if not active_zones or set(zones) != active_zones:
        raise RuntimeError(
            "generated secondary zone set differs from active BIND primary inventory "
            f"(generated={len(zones)}, active={len(active_zones)})")

    subprocess.run(["ssh", "-o", "BatchMode=yes", "-o", "ConnectTimeout=8",
                    ssh_host, "sudo", "-n", helper],
                   input=content, text=True, capture_output=True, check=True, timeout=30)

    # SOA queries are network-bound. Bounded concurrency keeps a full zone list
    # within the service timeout while limiting load on the DNS hosts.
    deadline = time.monotonic() + timeout_sec
    pending = set(zones)
    with ThreadPoolExecutor(max_workers=32) as pool:
        while pending and time.monotonic() < deadline:
            def converged(zone: str) -> bool:
                expected = serial(primary, zone)
                if expected is None:
                    raise RuntimeError(f"primary SOA unavailable for {zone}")
                peers = [secondary]
                if zone == "akoria.net":
                    peers.append("10.1.0.10")  # radium is secondary for forward only
                actual = [serial(host, zone) for host in peers]
                return all(value == expected for value in actual)

            zones_this_round = iter(tuple(pending))
            while True:
                batch = []
                for _ in range(32):
                    zone = next(zones_this_round, None)
                    if zone is None:
                        break
                    batch.append(zone)
                if not batch or time.monotonic() >= deadline:
                    break
                futures = [(pool.submit(converged, zone), zone) for zone in batch]
                for future, zone in futures:
                    remaining = deadline - time.monotonic()
                    if remaining <= 0:
                        break
                    try:
                        if future.result(timeout=remaining):
                            pending.discard(zone)
                    except RuntimeError:
                        raise
                    except Exception:
                        # A transient network/query failure leaves the zone pending
                        # for the next bounded verification round.
                        continue
            if pending and time.monotonic() < deadline:
                time.sleep(min(2, max(0, deadline - time.monotonic())))
    if pending:
        raise RuntimeError("secondary SOA serial mismatch: " + ", ".join(sorted(pending)))
    return len(zones)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--primary-config", default="/etc/bind/named.conf.akoria")
    parser.add_argument("--additional-config", action="append", default=[],
                        help="additional primary-zone fragments (repeatable)")
    parser.add_argument("--primary", default="10.0.0.20")
    parser.add_argument("--secondary", default="10.7.0.10")
    parser.add_argument("--ssh-host", default="jason@chlorine")
    parser.add_argument("--helper", default="/usr/local/sbin/solari-bind9-update-zones")
    args = parser.parse_args()
    try:
        count = synchronize(pathlib.Path(args.primary_config), args.primary,
                            args.secondary, args.ssh_host, args.helper,
                            additional_configs=tuple(pathlib.Path(name)
                                                     for name in args.additional_config))
    except (OSError, ValueError, RuntimeError, subprocess.SubprocessError) as exc:
        print(f"secondary refresh failed: {exc}", file=sys.stderr)
        raise SystemExit(1)
    print(f"chlorine serves current SOA serials for {count} generated zones")


if __name__ == "__main__":
    main()
