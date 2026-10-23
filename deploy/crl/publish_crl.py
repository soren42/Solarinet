#!/usr/bin/env python3
"""Validate and atomically expose one generated CRL to a static HTTPS server."""
from __future__ import annotations

import argparse
import datetime as dt
import fcntl
import hashlib
import os
import pathlib
import re
import subprocess
import sys
import tempfile

MAX_CRL_BYTES = 1024 * 1024
MAX_LIFETIME_SECONDS = 7 * 86400
MIN_REMAINING_SECONDS = 3600
CRL_RE = re.compile(
    rb"\A[ \t\r\n]*-----BEGIN X509 CRL-----[ \t\r\n]+"
    rb"[A-Za-z0-9+/=\r\n]+-----END X509 CRL-----[ \t\r\n]*\Z"
)


def command(argv: list[str]) -> bytes:
    result = subprocess.run(argv, capture_output=True, timeout=15,
                            env=dict(os.environ, LC_ALL="C"))
    if result.returncode:
        detail = result.stderr.decode("utf-8", "replace").strip()[:300]
        raise ValueError(f"{pathlib.Path(argv[0]).name} failed: {detail}")
    return result.stdout


def crl_info(data: bytes, issuer_ca: pathlib.Path, *, require_fresh: bool = True
             ) -> tuple[str, int, bytes, dt.datetime]:
    if not data or len(data) > MAX_CRL_BYTES or not CRL_RE.fullmatch(data):
        raise ValueError("source must contain exactly one PEM CRL under 1 MiB")
    with tempfile.TemporaryDirectory(prefix="solarinet-crl-publish-") as temp_name:
        candidate = pathlib.Path(temp_name) / "candidate.pem"
        candidate.write_bytes(data)
        issuer_der = command(["openssl", "x509", "-in", str(issuer_ca), "-outform", "DER"])
        issuer_fp = hashlib.sha256(issuer_der).hexdigest()
        issuer_subject = command(["openssl", "x509", "-in", str(issuer_ca), "-noout",
                                  "-subject", "-nameopt", "RFC2253"]).decode().strip()
        issuer_subject = issuer_subject.removeprefix("subject=")
        crl_issuer = command(["openssl", "crl", "-in", str(candidate), "-noout",
                              "-issuer", "-nameopt", "RFC2253"]).decode().strip()
        crl_issuer = crl_issuer.removeprefix("issuer=")
        if crl_issuer != issuer_subject:
            raise ValueError("source CRL issuer does not match the configured CA")
        command(["openssl", "crl", "-in", str(candidate), "-noout", "-verify",
                 "-CAfile", str(issuer_ca)])
        number_output = command(["openssl", "crl", "-in", str(candidate), "-noout", "-crlnumber"])
        match = re.fullmatch(rb"crlNumber=(?:0x)?([0-9a-fA-F]+)\s*", number_output)
        if not match:
            raise ValueError("source CRL has no parseable CRL number")
        def parse_time(option: str, prefix: bytes) -> dt.datetime:
            output = command(["openssl", "crl", "-in", str(candidate), "-noout", option])
            if not output.startswith(prefix):
                raise ValueError("source CRL has no " + option)
            return dt.datetime.strptime(
                output.removeprefix(prefix).decode().strip(),
                "%b %d %H:%M:%S %Y %Z").replace(tzinfo=dt.timezone.utc)

        last_update = parse_time("-lastupdate", b"lastUpdate=")
        next_update = parse_time("-nextupdate", b"nextUpdate=")
        now = dt.datetime.now(dt.timezone.utc)
        if last_update > now + dt.timedelta(minutes=2):
            raise ValueError("source CRL thisUpdate is in the future")
        if (next_update <= last_update or
                next_update - last_update > dt.timedelta(seconds=MAX_LIFETIME_SECONDS)):
            raise ValueError("source CRL validity interval is invalid or exceeds seven days")
        if require_fresh and (next_update <= now + dt.timedelta(seconds=MIN_REMAINING_SECONDS) or
                              next_update - now > dt.timedelta(seconds=MAX_LIFETIME_SECONDS)):
            raise ValueError("source CRL is expired, too close to expiry, or exceeds a seven-day lifetime")
        der = command(["openssl", "crl", "-in", str(candidate), "-outform", "DER"])
        return issuer_fp, int(match.group(1), 16), der, next_update


def atomic_write(path: pathlib.Path, payload: bytes) -> None:
    fd, temp_name = tempfile.mkstemp(prefix="." + path.name + ".", dir=path.parent)
    temp_path = pathlib.Path(temp_name)
    try:
        os.fchmod(fd, 0o644)
        with os.fdopen(fd, "wb") as stream:
            stream.write(payload)
            stream.flush()
            os.fsync(stream.fileno())
        os.replace(temp_path, path)
        dir_fd = os.open(path.parent, os.O_RDONLY | getattr(os, "O_DIRECTORY", 0))
        try:
            os.fsync(dir_fd)
        finally:
            os.close(dir_fd)
    finally:
        temp_path.unlink(missing_ok=True)


def publish(source: pathlib.Path, issuer_ca: pathlib.Path,
            public_dir: pathlib.Path) -> tuple[pathlib.Path, int, bool]:
    source = source.resolve(strict=True)
    issuer_ca = issuer_ca.resolve(strict=True)
    if not source.is_file() or not issuer_ca.is_file():
        raise ValueError("source and issuer CA must be regular files")
    public_dir.mkdir(parents=True, exist_ok=True)
    public_dir = public_dir.resolve(strict=True)
    source_stat = source.stat()
    if source_stat.st_size > MAX_CRL_BYTES:
        raise ValueError("source CRL exceeds 1 MiB")
    data = source.read_bytes()
    issuer_fp, number, der, next_update = crl_info(data, issuer_ca)
    target = public_dir / f"{issuer_fp}.crl.pem"
    lock_fd = os.open(public_dir / (target.name + ".lock"),
                      os.O_CREAT | os.O_RDWR | getattr(os, "O_NOFOLLOW", 0), 0o600)
    try:
        fcntl.flock(lock_fd, fcntl.LOCK_EX)
        if target.exists():
            old_data = target.read_bytes()
            # An expired but correctly signed old publication must not prevent
            # recovery to a newer CRL; use it only for monotonic number checks.
            _, old_number, old_der, _old_expiry = crl_info(old_data, issuer_ca,
                                                           require_fresh=False)
            if number < old_number or (number == old_number and der != old_der):
                raise ValueError("refusing CRL number rollback or conflicting same-number CRL")
            if number == old_number:
                return target, number, False
        atomic_write(target, data)
        return target, number, True
    finally:
        os.close(lock_fd)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", required=True)
    parser.add_argument("--issuer-ca", required=True)
    parser.add_argument("--public-dir", required=True)
    args = parser.parse_args()
    try:
        target, number, changed = publish(pathlib.Path(args.source),
                                          pathlib.Path(args.issuer_ca),
                                          pathlib.Path(args.public_dir))
        print(f"{'published' if changed else 'already-current'} crl_number={number} file={target.name}")
    except (OSError, ValueError, subprocess.SubprocessError) as error:
        print(f"CRL publication failed: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
