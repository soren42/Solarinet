#!/usr/bin/env python3
"""Reject server binaries that depend on build-only or sanitizer runtimes."""
from __future__ import annotations
import argparse, re, shutil, subprocess
from pathlib import Path

NAMES = ("solariServer", "solariScanWorker")
FORBIDDEN_NEEDED = re.compile(r"lib(?:nng|mbed(?:tls|crypto|x509)|asan|ubsan)(?:[.-]|$)", re.I)

def inspect(binary: Path) -> None:
    if not binary.is_file():
        raise ValueError(f"release binary is missing: {binary}")
    result = subprocess.run(["readelf", "-d", str(binary)], capture_output=True, text=True, check=True)
    needed = re.findall(r"Shared library: \[([^]]+)\]", result.stdout)
    forbidden = [name for name in needed if FORBIDDEN_NEEDED.search(name)]
    if forbidden:
        raise ValueError(f"{binary.name} has non-portable shared dependencies: {forbidden}")
    rpaths = re.findall(r"\((RPATH|RUNPATH)\).*?\[([^]]+)\]", result.stdout)
    if rpaths:
        raise ValueError(f"{binary.name} embeds runtime search paths: {rpaths}")
    ldd = subprocess.run(["ldd", str(binary)], capture_output=True, text=True, check=True)
    if "not found" in ldd.stdout:
        raise ValueError(f"{binary.name} has unresolved runtime dependencies:\n{ldd.stdout}")
    print(f"PASS {binary.name}: {len(needed)} shared dependencies resolved; no sanitizer/temporary NNG or mbedTLS dependency")

def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("bin_dir", type=Path)
    args = parser.parse_args()
    if shutil.which("readelf") is None or shutil.which("ldd") is None:
        parser.error("readelf and ldd are required for Linux release-binary validation")
    try:
        for name in NAMES:
            inspect(args.bin_dir / name)
    except (OSError, subprocess.CalledProcessError, ValueError) as exc:
        parser.error(str(exc))
    return 0

if __name__ == "__main__":
    raise SystemExit(main())
