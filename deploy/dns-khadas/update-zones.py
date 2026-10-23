#!/usr/bin/env python3
"""Privileged, atomic BIND secondary-zone config update; config arrives on stdin."""
import fcntl
import os
import pathlib
import re
import stat
import subprocess
import sys
import tempfile

CONFIG_DIR = pathlib.Path("/etc/bind/solarinet-secondary")
OPTIONS = CONFIG_DIR / "options.conf"
ZONES = CONFIG_DIR / "zones.conf"
LOCK = CONFIG_DIR / ".zones.lock"
ZONE_RE = re.compile(
    r'^zone "([A-Za-z0-9._-]+)" \{ type secondary; primaries \{ 10\.0\.0\.20; \}; '
    r'file "/var/cache/bind/solarinet-secondary/db\.([A-Za-z0-9._-]+)"; \};$'
)


def validate(text: str) -> None:
    if len(text.encode("utf-8")) > 65536:
        raise ValueError("zone config exceeds 64 KiB")
    zones = set()
    for line in text.splitlines():
        if not line or line.startswith("// Generated from the active SolariNet primary zone list; do not edit."):
            continue
        match = ZONE_RE.fullmatch(line)
        if not match or match.group(1) != match.group(2):
            raise ValueError("zone config contains an unsupported declaration")
        zone = match.group(1)
        if ".." in zone or zone.startswith(".") or zone in zones:
            raise ValueError("zone config contains an invalid or duplicate name")
        zones.add(zone)
    if not zones or len(zones) > 256:
        raise ValueError("zone config must contain 1 to 256 zones")


def check_directory() -> None:
    info = CONFIG_DIR.lstat()
    if (not stat.S_ISDIR(info.st_mode) or info.st_uid != 0
            or info.st_mode & 0o022):
        raise ValueError("BIND config directory must be root-owned and not group/world writable")
    for path in (OPTIONS, ZONES):
        info = path.lstat()
        if (not stat.S_ISREG(info.st_mode) or info.st_uid != 0
                or info.st_mode & 0o022):
            raise ValueError(f"BIND config file has unsafe ownership or mode: {path.name}")


def named_check(candidate: pathlib.Path) -> None:
    fd, check_path = tempfile.mkstemp(prefix=".check-", dir=CONFIG_DIR, text=True)
    try:
        with os.fdopen(fd, "w", encoding="utf-8") as check:
            check.write(f'include "{OPTIONS}";\ninclude "{candidate}";\n')
            check.flush()
            os.fsync(check.fileno())
        subprocess.run(["named-checkconf", check_path],
                       check=True, timeout=10, capture_output=True, text=True)
    finally:
        try:
            os.unlink(check_path)
        except FileNotFoundError:
            pass


def write_atomic(data: bytes) -> None:
    fd, temporary = tempfile.mkstemp(prefix=".zones-", dir=CONFIG_DIR)
    try:
        os.fchmod(fd, 0o644)
        os.fchown(fd, 0, 0)
        with os.fdopen(fd, "wb") as output:
            output.write(data)
            output.flush()
            os.fsync(output.fileno())
        os.replace(temporary, ZONES)
        directory_fd = os.open(CONFIG_DIR, os.O_RDONLY | os.O_DIRECTORY)
        try:
            os.fsync(directory_fd)
        finally:
            os.close(directory_fd)
    finally:
        try:
            os.unlink(temporary)
        except FileNotFoundError:
            pass


def apply(text: str) -> None:
    validate(text)
    check_directory()
    lock_fd = os.open(LOCK, os.O_CREAT | os.O_RDWR | os.O_NOFOLLOW, 0o600)
    try:
        info = os.fstat(lock_fd)
        if not stat.S_ISREG(info.st_mode) or info.st_uid != 0 or info.st_mode & 0o077:
            raise ValueError("zone update lock is not a private root-owned file")
        lock_stream = os.fdopen(lock_fd, "r+")
        lock_fd = -1
        with lock_stream as lock:
            fcntl.flock(lock, fcntl.LOCK_EX)
            old = ZONES.read_bytes()
            fd, candidate_name = tempfile.mkstemp(prefix=".candidate-", dir=CONFIG_DIR)
            candidate = pathlib.Path(candidate_name)
            try:
                os.fchmod(fd, 0o600)
                with os.fdopen(fd, "w", encoding="utf-8") as stream:
                    stream.write(text)
                    stream.flush()
                    os.fsync(stream.fileno())
                named_check(candidate)
                if old == text.encode("utf-8"):
                    return
                write_atomic(text.encode("utf-8"))
                try:
                    named_check(ZONES)
                    subprocess.run(["/usr/bin/systemctl", "reload", "solari-bind9.service"],
                                   check=True, timeout=20, capture_output=True, text=True)
                except Exception:
                    write_atomic(old)
                    subprocess.run(["/usr/bin/systemctl", "reload", "solari-bind9.service"],
                                   check=False, timeout=20, capture_output=True, text=True)
                    raise
            finally:
                try:
                    candidate.unlink()
                except FileNotFoundError:
                    pass
    finally:
        if lock_fd >= 0:
            os.close(lock_fd)


def main() -> None:
    if os.geteuid() != 0:
        print("must run as root", file=sys.stderr)
        raise SystemExit(1)
    try:
        data = sys.stdin.buffer.read(65537)
        if len(data) > 65536:
            raise ValueError("zone config exceeds 64 KiB")
        text = data.decode("utf-8")
        apply(text)
    except Exception as exc:
        print(f"BIND secondary zone update failed: {exc}", file=sys.stderr)
        raise SystemExit(1)
    print("BIND secondary zone config validated and applied")


if __name__ == "__main__":
    main()
