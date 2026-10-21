#!/usr/bin/env python3
"""Compare canonical primary/secondary zone contents without printing records.

Run from an operator workstation with passwordless SSH as the configured user to
both DNS hosts. The hosts need sudo access to named-checkconf/named-compilezone.
Only zone names and SHA-256 digests leave each host.
"""
import argparse
import re
import subprocess
import sys


HASH_LINE = re.compile(r"^([A-Za-z0-9._-]+)\t([0-9a-f]{64})$")


def read_hashes(output: str) -> dict[str, str]:
    result = {}
    for line in output.splitlines():
        match = HASH_LINE.fullmatch(line)
        if not match or match.group(1) in result:
            raise ValueError("remote verifier returned an invalid digest list")
        result[match.group(1)] = match.group(2)
    if not result:
        raise ValueError("remote verifier returned no zone digests")
    return result


PRIMARY_REMOTE = r'''#!/usr/bin/env python3
import hashlib, pathlib, re, subprocess, sys
ZONE_NAME = re.compile(r"^[A-Za-z0-9][A-Za-z0-9._-]{0,252}$")
ZONE_START = re.compile(r'^zone\s+"([A-Za-z0-9._-]+)"\s*\{', re.M)
TYPE = re.compile(r"\btype\s+(?:primary|master)\b", re.I)
FILE = re.compile(r'^\s*file\s+"([^"\n]+)"\s*;', re.M)
try:
    expanded = subprocess.run(["named-checkconf", "-p"], check=True,
        capture_output=True, text=True, timeout=30).stdout
    zones = {}
    for start in ZONE_START.finditer(expanded):
        tail = expanded[start.end():]
        end = re.search(r"^};\s*$", tail, re.M)
        if not end:
            raise ValueError
        block = tail[:end.start()]
        if not TYPE.search(block):
            continue
        name = start.group(1)
        match = FILE.search(block)
        if not ZONE_NAME.fullmatch(name) or ".." in name or not match:
            raise ValueError
        path = match.group(1)
        if (not path.startswith("/etc/bind/zones/db.") or
                not re.fullmatch(r"/etc/bind/zones/db\.[A-Za-z0-9._-]+", path)):
            raise ValueError
        if name in zones:
            raise ValueError
        zones[name] = path
    if not zones:
        raise ValueError
    for name, filename in sorted(zones.items()):
        path = pathlib.Path(filename)
        if path.is_symlink() or not path.is_file():
            raise ValueError
        output = subprocess.run(["named-compilezone", "-D", "-f", "text",
            "-F", "text", "-o", "-", name, filename], check=True,
            capture_output=True, timeout=30).stdout
        print(name + "\t" + hashlib.sha256(output).hexdigest())
except Exception:
    print("primary canonical zone verification failed", file=sys.stderr)
    sys.exit(1)
'''


SECONDARY_REMOTE = r'''#!/usr/bin/env python3
import hashlib, pathlib, re, subprocess, sys
CONFIG = pathlib.Path("/etc/bind/solarinet-secondary/zones.conf")
LINE = re.compile(r'^zone "([A-Za-z0-9._-]+)" \{ type secondary; primaries \{ 10\.0\.0\.20; \}; file "/var/cache/bind/solarinet-secondary/db\.([A-Za-z0-9._-]+)"; \};$')
try:
    zones = {}
    for line in CONFIG.read_text(encoding="utf-8").splitlines():
        if not line or line.startswith("// Generated from the active SolariNet primary zone list;"):
            continue
        match = LINE.fullmatch(line)
        if not match or match.group(1) != match.group(2):
            raise ValueError
        name = match.group(1)
        if ".." in name or name.startswith(".") or name in zones:
            raise ValueError
        zones[name] = f"/var/cache/bind/solarinet-secondary/db.{name}"
    if not zones:
        raise ValueError
    for name, filename in sorted(zones.items()):
        path = pathlib.Path(filename)
        if path.is_symlink() or not path.is_file():
            raise ValueError
        output = subprocess.run(["named-compilezone", "-D", "-f", "raw",
            "-F", "text", "-o", "-", name, filename], check=True,
            capture_output=True, timeout=30).stdout
        print(name + "\t" + hashlib.sha256(output).hexdigest())
except Exception:
    print("secondary canonical zone verification failed", file=sys.stderr)
    sys.exit(1)
'''


def remote_hashes(host: str, user: str, script: str, label: str) -> dict[str, str]:
    try:
        result = subprocess.run(
            ["ssh", "-o", "BatchMode=yes", "-o", "ConnectTimeout=8",
             f"{user}@{host}", "sudo", "-n", "python3", "-"],
            input=script, capture_output=True, text=True, timeout=300, check=True)
    except (OSError, subprocess.SubprocessError):
        raise RuntimeError(f"{label} remote canonicalization failed") from None
    try:
        return read_hashes(result.stdout)
    except ValueError as error:
        raise RuntimeError(f"{label} {error}") from None


def verify(primary: str, secondary: str, user: str) -> tuple[int, list[str]]:
    primary_hashes = remote_hashes(primary, user, PRIMARY_REMOTE, "primary")
    secondary_hashes = remote_hashes(secondary, user, SECONDARY_REMOTE, "secondary")
    if set(primary_hashes) != set(secondary_hashes):
        missing = sorted(set(primary_hashes) - set(secondary_hashes))
        extra = sorted(set(secondary_hashes) - set(primary_hashes))
        details = []
        if missing:
            details.append("secondary missing: " + ", ".join(missing))
        if extra:
            details.append("secondary extra: " + ", ".join(extra))
        return min(len(primary_hashes), len(secondary_hashes)), details
    mismatches = sorted(name for name in primary_hashes
                        if primary_hashes[name] != secondary_hashes[name])
    return len(primary_hashes), mismatches


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--primary", default="xenon")
    parser.add_argument("--secondary", default="chlorine")
    parser.add_argument("--user", default="jason")
    args = parser.parse_args()
    try:
        count, differences = verify(args.primary, args.secondary, args.user)
    except RuntimeError as error:
        print(str(error), file=sys.stderr)
        return 1
    if differences:
        print(f"canonical zone content differs in {len(differences)} place(s): "
              + ", ".join(differences), file=sys.stderr)
        return 1
    print(f"canonical zone contents including SOA match for {count} zones")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
