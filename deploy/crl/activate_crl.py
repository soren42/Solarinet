#!/usr/bin/env python3
"""Fetch, validate, atomically install, and activate one issuer's CRL."""
from __future__ import annotations

import argparse
import datetime as dt
import fcntl
import hashlib
import os
import pathlib
import re
import signal
import ssl
import subprocess
import sys
import tempfile
import time
import urllib.parse
import urllib.request

MAX_CRL_BYTES = 1024 * 1024
MAX_BUNDLE_BYTES = 4 * 1024 * 1024
ACTIVATION_BUDGET_SECONDS = 120
OPERATION_DEADLINE: float | None = None
CRL_RE = re.compile(rb"-----BEGIN X509 CRL-----\s+.*?-----END X509 CRL-----\s*", re.S)
SINGLE_CRL_RE = re.compile(
    rb"\A[ \t\r\n]*-----BEGIN X509 CRL-----[ \t\r\n]+"
    rb"[A-Za-z0-9+/=\r\n]+-----END X509 CRL-----[ \t\r\n]*\Z"
)
CERT_RE = re.compile(rb"-----BEGIN CERTIFICATE-----\s+.*?-----END CERTIFICATE-----\s*", re.S)
ALLOWED_SERVICES = {
    "solari-client.service", "solari-monitor.service", "solari-server.service",
    "solarinet-server.service",
}


class AtomicWriteError(OSError):
    """An atomic-write failure annotated with whether os.replace completed."""

    def __init__(self, cause: OSError, replaced: bool) -> None:
        super().__init__(cause.errno, cause.strerror or str(cause), cause.filename)
        self.replaced = replaced


def command(argv: list[str], timeout: int = 10) -> bytes:
    command_timeout = float(timeout)
    if OPERATION_DEADLINE is not None:
        remaining = OPERATION_DEADLINE - time.monotonic()
        if remaining <= 0:
            raise TimeoutError("CRL validation exceeded its 120-second operation budget")
        command_timeout = min(command_timeout, remaining)
    env = dict(os.environ, LC_ALL="C")
    try:
        result = subprocess.run(argv, capture_output=True, timeout=command_timeout, env=env)
    except subprocess.TimeoutExpired as error:
        if OPERATION_DEADLINE is not None and time.monotonic() >= OPERATION_DEADLINE:
            raise TimeoutError("CRL validation exceeded its 120-second operation budget") from error
        raise
    if result.returncode:
        detail = result.stderr.decode("utf-8", "replace").strip()[:400]
        raise ValueError(f"{pathlib.Path(argv[0]).name} failed: {detail}")
    return result.stdout


def _fetch_deadline(_signum: int, _frame: object) -> None:
    raise TimeoutError("CRL fetch exceeded its total time limit")


def fetch_crl(url: str, https_ca_file: pathlib.Path | None, timeout: int) -> bytes:
    parsed = urllib.parse.urlsplit(url)
    if parsed.scheme != "https" or not parsed.hostname or parsed.username or parsed.password:
        raise ValueError("CRL URL must be an HTTPS URL without embedded credentials")
    context = ssl.create_default_context(cafile=str(https_ca_file) if https_ca_file else None)
    request = urllib.request.Request(url, headers={"Accept": "application/pkix-crl, application/x-pem-file"})
    class NoRedirect(urllib.request.HTTPRedirectHandler):
        def redirect_request(self, req, fp, code, msg, headers, newurl):
            return None

    opener = urllib.request.build_opener(urllib.request.HTTPSHandler(context=context), NoRedirect())
    previous_handler = signal.getsignal(signal.SIGALRM)
    previous_timer, previous_interval = signal.getitimer(signal.ITIMER_REAL)
    started = time.monotonic()
    signal.signal(signal.SIGALRM, _fetch_deadline)
    signal.setitimer(signal.ITIMER_REAL, timeout)
    try:
        with opener.open(request, timeout=timeout) as response:
            payload = response.read(MAX_CRL_BYTES + 1)
    finally:
        signal.setitimer(signal.ITIMER_REAL, 0)
        signal.signal(signal.SIGALRM, previous_handler)
        if previous_timer > 0:
            signal.setitimer(signal.ITIMER_REAL,
                             max(0.001, previous_timer - (time.monotonic() - started)),
                             previous_interval)
    if not payload or len(payload) > MAX_CRL_BYTES:
        raise ValueError("CRL response is empty or exceeds the 1 MiB limit")
    return payload


def subject_name(cert: pathlib.Path) -> str:
    return command(["openssl", "x509", "-in", str(cert), "-noout", "-subject",
                    "-nameopt", "RFC2253"]).decode("utf-8", "replace").strip().removeprefix("subject=")


def crl_issuer(crl: pathlib.Path) -> str:
    return command(["openssl", "crl", "-in", str(crl), "-noout", "-issuer",
                    "-nameopt", "RFC2253"]).decode("utf-8", "replace").strip().removeprefix("issuer=")


def crl_metadata(crl: pathlib.Path) -> tuple[int, dt.datetime, dt.datetime, bytes]:
    number_out = command(["openssl", "crl", "-in", str(crl), "-noout", "-crlnumber"])
    match = re.fullmatch(rb"crlNumber=(?:0x)?([0-9a-fA-F]+)\s*", number_out)
    if not match:
        raise ValueError("CRL has no parseable CRL number")

    def parse_date(option: str) -> dt.datetime:
        value = command(["openssl", "crl", "-in", str(crl), "-noout", option])
        prefix = b"lastUpdate=" if option == "-lastupdate" else b"nextUpdate="
        if not value.startswith(prefix):
            raise ValueError("CRL is missing " + option)
        date = value[len(prefix):].decode("ascii", "strict").strip()
        return dt.datetime.strptime(date, "%b %d %H:%M:%S %Y %Z").replace(tzinfo=dt.timezone.utc)

    return (int(match.group(1), 16), parse_date("-lastupdate"),
            parse_date("-nextupdate"), command(["openssl", "crl", "-in", str(crl), "-outform", "DER"]))


def validate_crl(crl: pathlib.Path, issuer_ca: pathlib.Path, base_bundle: pathlib.Path,
                 *, min_valid_seconds: int, max_lifetime_seconds: int) -> tuple[int, str, bytes]:
    if base_bundle.stat().st_size > MAX_BUNDLE_BYTES:
        raise ValueError("immutable base CA bundle exceeds the 4 MiB limit")
    cert_data = base_bundle.read_bytes()
    if CRL_RE.search(cert_data):
        raise ValueError("immutable base CA bundle must not contain a CRL")
    certs = CERT_RE.findall(cert_data)
    if not certs:
        raise ValueError("base CA bundle contains no PEM certificates")
    issuer_der = command(["openssl", "x509", "-in", str(issuer_ca), "-outform", "DER"])
    issuer_fp = hashlib.sha256(issuer_der).hexdigest()
    bundled_fps = set()
    with tempfile.TemporaryDirectory(prefix="solarinet-crl-certs-") as temp_name:
        temp_dir = pathlib.Path(temp_name)
        for index, block in enumerate(certs):
            cert_path = temp_dir / f"cert-{index}.pem"
            cert_path.write_bytes(block)
            der = command(["openssl", "x509", "-in", str(cert_path), "-outform", "DER"])
            bundled_fps.add(hashlib.sha256(der).hexdigest())
    if issuer_fp not in bundled_fps:
        raise ValueError("issuer CA certificate is not present in the immutable base bundle")
    if crl_issuer(crl) != subject_name(issuer_ca):
        raise ValueError("CRL issuer does not match the configured issuer CA")
    command(["openssl", "crl", "-in", str(crl), "-noout", "-verify", "-CAfile", str(issuer_ca)])
    number, last_update, next_update, der = crl_metadata(crl)
    now = dt.datetime.now(dt.timezone.utc)
    if last_update > now + dt.timedelta(minutes=2):
        raise ValueError("CRL thisUpdate is in the future")
    if next_update <= now + dt.timedelta(seconds=min_valid_seconds):
        raise ValueError("CRL is expired or too close to expiry")
    if next_update <= last_update or next_update - last_update > dt.timedelta(seconds=max_lifetime_seconds):
        raise ValueError("CRL validity interval exceeds the configured maximum")
    return number, issuer_fp, der


def atomic_write(path: pathlib.Path, payload: bytes, previous_stat: os.stat_result | None,
                 new_mode: int = 0o644, preserve_mode: bool = True) -> None:
    replaced = False
    temp_path: pathlib.Path | None = None
    try:
        path.parent.mkdir(parents=True, exist_ok=True)
        fd, temp_name = tempfile.mkstemp(prefix="." + path.name + ".", dir=path.parent)
        temp_path = pathlib.Path(temp_name)
        with os.fdopen(fd, "wb") as stream:
            if previous_stat is None or not preserve_mode:
                os.fchmod(stream.fileno(), new_mode)
            else:
                os.fchmod(stream.fileno(), previous_stat.st_mode & 0o777)
            if previous_stat is not None and preserve_mode and os.geteuid() == 0:
                os.fchown(stream.fileno(), previous_stat.st_uid, previous_stat.st_gid)
            stream.write(payload)
            stream.flush()
            os.fsync(stream.fileno())
        os.replace(temp_path, path)
        replaced = True
        dir_fd = os.open(path.parent, os.O_RDONLY | getattr(os, "O_DIRECTORY", 0))
        try:
            os.fsync(dir_fd)
        finally:
            os.close(dir_fd)
    except OSError as error:
        raise AtomicWriteError(error, replaced) from error
    finally:
        if temp_path is not None:
            temp_path.unlink(missing_ok=True)


def restart_service(service: str) -> None:
    subprocess.run(["systemctl", "restart", service], check=True, timeout=60)
    subprocess.run(["systemctl", "is-active", "--quiet", service], check=True, timeout=10)


def activate(url: str, base_bundle: pathlib.Path, issuer_ca: pathlib.Path,
             active_bundle: pathlib.Path, service: str,
             https_ca_file: pathlib.Path | None = None, timeout: int = 20,
             min_valid_seconds: int = 3600, max_lifetime_seconds: int = 7 * 86400) -> bool:
    global OPERATION_DEADLINE
    previous_deadline = OPERATION_DEADLINE
    OPERATION_DEADLINE = time.monotonic() + ACTIVATION_BUDGET_SECONDS
    try:
        active_bundle.parent.mkdir(parents=True, exist_ok=True)
        lock_path = active_bundle.with_name(active_bundle.name + ".lock")
        flags = os.O_CREAT | os.O_RDWR | getattr(os, "O_NOFOLLOW", 0)
        lock_fd = os.open(lock_path, flags, 0o600)
        try:
            fcntl.flock(lock_fd, fcntl.LOCK_EX)
            return _activate_locked(url, base_bundle, issuer_ca, active_bundle, service,
                                    https_ca_file, timeout, min_valid_seconds,
                                    max_lifetime_seconds)
        finally:
            os.close(lock_fd)
    finally:
        OPERATION_DEADLINE = previous_deadline


def _activate_locked(url: str, base_bundle: pathlib.Path, issuer_ca: pathlib.Path,
                     active_bundle: pathlib.Path, service: str,
                     https_ca_file: pathlib.Path | None, timeout: int,
                     min_valid_seconds: int, max_lifetime_seconds: int) -> bool:
    if service not in ALLOWED_SERVICES:
        raise ValueError("service is not in the SolariNet activation allowlist")
    if timeout <= 0 or min_valid_seconds < 0 or max_lifetime_seconds <= 0:
        raise ValueError("timeout and CRL lifetime bounds must be positive")
    base_bundle = base_bundle.resolve(strict=True)
    issuer_ca = issuer_ca.resolve(strict=True)
    payload = fetch_crl(url, https_ca_file, timeout)
    state_path = active_bundle.with_name(active_bundle.name + ".activated")
    with tempfile.TemporaryDirectory(prefix="solarinet-crl-candidate-") as temp_name:
        crl_path = pathlib.Path(temp_name) / "candidate.crl.pem"
        base_bundle_snapshot = pathlib.Path(temp_name) / "base-ca.pem"
        with base_bundle.open("rb") as stream:
            base_bundle_bytes = stream.read(MAX_BUNDLE_BYTES + 1)
        if len(base_bundle_bytes) > MAX_BUNDLE_BYTES:
            raise ValueError("immutable base CA bundle exceeds the 4 MiB limit")
        base_bundle_snapshot.write_bytes(base_bundle_bytes)
        if not SINGLE_CRL_RE.fullmatch(payload):
            raise ValueError("CRL response must contain exactly one PEM CRL and whitespace")
        crl_path.write_bytes(payload)
        number, _issuer_fp, der = validate_crl(
            crl_path, issuer_ca, base_bundle_snapshot,
            min_valid_seconds=min_valid_seconds,
            max_lifetime_seconds=max_lifetime_seconds,
        )
        canonical_crl = command(["openssl", "crl", "-in", str(crl_path), "-outform", "PEM"])
        combined = base_bundle_bytes.rstrip() + b"\n" + canonical_crl.rstrip() + b"\n"
        if len(combined) > MAX_BUNDLE_BYTES:
            raise ValueError("combined active CA bundle exceeds the 4 MiB limit")

        old_bytes: bytes | None = None
        old_stat: os.stat_result | None = None
        old_number = 0
        old_der: bytes | None = None
        if active_bundle.exists():
            old_stat = active_bundle.stat(follow_symlinks=False)
            if not active_bundle.is_file() or active_bundle.is_symlink():
                raise ValueError("active bundle path must be a regular non-symlink file")
            if old_stat.st_size > MAX_BUNDLE_BYTES:
                raise ValueError("active CA bundle exceeds the 4 MiB limit")
            old_bytes = active_bundle.read_bytes()
            old_crls = CRL_RE.findall(old_bytes)
            if len(old_crls) > 1:
                raise ValueError("active bundle contains multiple CRLs; use one updater per issuer bundle")
            if old_crls:
                old_crl_path = pathlib.Path(temp_name) / "previous.crl.pem"
                old_crl_path.write_bytes(old_crls[0])
                if crl_issuer(old_crl_path) != subject_name(issuer_ca):
                    raise ValueError("active bundle contains a CRL for a different issuer")
                command(["openssl", "crl", "-in", str(old_crl_path), "-noout", "-verify",
                         "-CAfile", str(issuer_ca)])
                old_number, _old_last, _old_next, old_der = crl_metadata(old_crl_path)
                if number < old_number or (number == old_number and der != old_der):
                    raise ValueError("CRL number rollback or conflicting same-number CRL refused")
                if number == old_number and der == old_der:
                    expected_state = f"{_issuer_fp}:{number}:{hashlib.sha256(der).hexdigest()}\n".encode("ascii")
                    bundle_matches = old_bytes == combined
                    receipt_matches = (state_path.is_file() and not state_path.is_symlink() and
                                       state_path.read_bytes() == expected_state)
                    if bundle_matches:
                        if receipt_matches and state_path.stat(follow_symlinks=False).st_mode & 0o777 == 0o644:
                            return False
                        if not receipt_matches:
                            restart_service(service)
                            print(f"reconciled CRL number={number} issuer_fp={_issuer_fp} service={service}")
                        atomic_write(state_path, expected_state,
                                     state_path.stat(follow_symlinks=False) if state_path.exists() else None,
                                     new_mode=0o644, preserve_mode=False)
                        return True
                    # Base roots changed while the CRL generation stayed the
                    # same. Fall through to atomically replace and restart.
        if OPERATION_DEADLINE is not None and time.monotonic() >= OPERATION_DEADLINE:
            raise TimeoutError("CRL validation exceeded its 120-second operation budget")
        try:
            atomic_write(active_bundle, combined, old_stat)
            restart_service(service)
        except (OSError, subprocess.SubprocessError) as error:
            if isinstance(error, AtomicWriteError) and not error.replaced:
                raise RuntimeError(
                    f"activation failed before active bundle replacement; "
                    f"active trust state is unchanged: {error}; check bundle ownership, "
                    "filesystem space, and write permissions"
                ) from error
            try:
                if old_bytes is None:
                    active_bundle.unlink(missing_ok=True)
                else:
                    atomic_write(active_bundle, old_bytes, old_stat)
            except OSError as rollback_error:
                raise RuntimeError(
                    f"activation failed ({error}); bundle rollback failed ({rollback_error}); "
                    "active trust state requires manual recovery"
                ) from error
            try:
                restart_service(service)
            except (OSError, subprocess.SubprocessError) as rollback_error:
                raise RuntimeError(f"activation failed ({error}); rollback restart also failed ({rollback_error})") from error
            raise RuntimeError(f"activation failed; previous bundle restored: {error}") from error
        expected_state = f"{_issuer_fp}:{number}:{hashlib.sha256(der).hexdigest()}\n".encode("ascii")
        try:
            atomic_write(state_path, expected_state,
                         state_path.stat(follow_symlinks=False) if state_path.exists() else None,
                         new_mode=0o644, preserve_mode=False)
        except OSError as error:
            raise RuntimeError(
                f"CRL number {number} is active and {service} restarted, but the "
                f"activation receipt write failed; the next run will reconcile it: {error}"
            ) from error
        print(f"activated CRL number={number} issuer_fp={_issuer_fp} service={service}")
        return True


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--url", required=True)
    parser.add_argument("--base-ca-bundle", required=True, type=pathlib.Path)
    parser.add_argument("--issuer-ca", required=True, type=pathlib.Path)
    parser.add_argument("--active-bundle", required=True, type=pathlib.Path)
    parser.add_argument("--service", required=True, choices=sorted(ALLOWED_SERVICES))
    parser.add_argument("--https-ca-file", type=pathlib.Path,
                        help="exclusive CA trust file for the HTTPS CRL endpoint (default: system trust)")
    parser.add_argument("--timeout", type=int, default=20)
    parser.add_argument("--min-valid-minutes", type=int, default=5)
    parser.add_argument("--max-crl-hours", type=int, default=168)
    args = parser.parse_args()
    try:
        activate(args.url, args.base_ca_bundle, args.issuer_ca, args.active_bundle,
                 args.service, args.https_ca_file, args.timeout,
                 args.min_valid_minutes * 60, args.max_crl_hours * 3600)
    except (OSError, ValueError, subprocess.SubprocessError, RuntimeError) as error:
        print(f"CRL activation failed: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
