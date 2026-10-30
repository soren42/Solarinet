#!/usr/bin/env python3
"""Fail if a known SolariNet daemon has code, executable, or cwd in Code/ checkout."""
from __future__ import annotations

import argparse
import os
from pathlib import Path

CHECKOUT = "/home/jason/Code"
DAEMON_NAMES = {
    "solariServer", "solariScanWorker", "solariClient", "solariMonitor", "solariSnmpPoll",
    "sor_emitd.py", "sor_apply_dnsd.py", "sor_apply_pihole.py", "sor_reconcile_discovery.py",
    "notifyd.py", "alertbridge.py", "unifipolld.py", "mdns_inspect.py",
}


def read(path: Path) -> bytes:
    try:
        return path.read_bytes()
    except (OSError, PermissionError):
        return b""


def under_checkout(value: str) -> bool:
    value = value.removesuffix(" (deleted)")
    return value == CHECKOUT or value.startswith(CHECKOUT + "/")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--proc-root", type=Path, default=Path("/proc"), help=argparse.SUPPRESS)
    args = parser.parse_args()
    offenders: list[tuple[str, str, str]] = []
    for proc in args.proc_root.iterdir():
        if not proc.name.isdigit():
            continue
        comm = read(proc / "comm").decode(errors="replace").strip()
        argv = [part.decode(errors="replace") for part in read(proc / "cmdline").split(b"\0") if part]
        if not argv:
            argv = [comm]
        relevant = comm.lower().startswith("solari") or any(
            Path(arg).name in DAEMON_NAMES for arg in argv
        )
        if not relevant:
            continue
        bad = []
        for label in ("cwd", "exe"):
            try:
                target = os.readlink(proc / label)
            except OSError:
                continue
            if under_checkout(target):
                bad.append(f"{label}={target}")
        for arg in argv:
            if under_checkout(arg) and (Path(arg).suffix == ".py" or Path(arg).name in DAEMON_NAMES
                                        or "/build-io/" in arg):
                bad.append(f"script={arg}")
        if bad:
            offenders.append((proc.name, comm or "?", ", ".join(sorted(set(bad)))))
    if offenders:
        for pid, comm, paths in offenders:
            print(f"FAIL pid={pid} comm={comm} {paths}")
        return 1
    print("PASS: no SolariNet daemon code, executable, or cwd resolves under /home/jason/Code")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
