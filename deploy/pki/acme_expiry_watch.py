#!/usr/bin/env python3
"""Check deployed HTTPS leaf certificates and publish transition alerts."""
from __future__ import annotations

import argparse
import datetime as dt
import ipaddress
import json
import os
import pathlib
import re
import subprocess
import sys
import tempfile
import stat
from typing import Any, Callable

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import acme_certificates as acme


MAX_CONFIG_BYTES = 256 * 1024
OPENSSL = "/usr/bin/openssl"


def _private_json(path: pathlib.Path, limit: int = MAX_CONFIG_BYTES) -> dict[str, Any]:
    try:
        info = path.lstat()
        if not path.is_file() or path.is_symlink() or info.st_uid != os.geteuid():
            raise ValueError("file must be a regular file owned by the service user")
        if info.st_mode & 0o077:
            raise ValueError("file permissions must exclude group and other access")
        if info.st_size > limit:
            raise ValueError("file exceeds the size limit")
        raw = path.read_bytes()
        value = json.loads(raw)
    except (OSError, ValueError, json.JSONDecodeError) as exc:
        raise ValueError(f"cannot load protected JSON file {path}: {exc}") from exc
    if not isinstance(value, dict):
        raise ValueError("JSON root must be an object")
    return value


def load_config(path: pathlib.Path) -> dict[str, Any]:
    config = _private_json(path)
    if config.get("schema_version") != 1:
        raise ValueError("unsupported expiry-watch schema_version")
    warning = config.get("warning_days")
    critical = config.get("critical_days")
    if (type(warning) is not int or type(critical) is not int
            or warning < 2 or critical < 1 or critical >= warning):
        raise ValueError("warning_days and critical_days must be integers with warning > critical >= 1")
    services = config.get("services")
    if not isinstance(services, list) or not services:
        raise ValueError("services must be a non-empty array")
    seen: set[str] = set()
    for service in services:
        if not isinstance(service, dict):
            raise ValueError("each service must be an object")
        sid = service.get("id")
        cert = service.get("certificate")
        domains = service.get("domains")
        if not isinstance(sid, str) or not re.fullmatch(r"[a-z][a-z0-9_-]{0,31}", sid):
            raise ValueError("service id is invalid")
        if sid in seen:
            raise ValueError("service ids must be unique")
        seen.add(sid)
        if not isinstance(cert, str) or not pathlib.Path(cert).is_absolute():
            raise ValueError(f"{sid} certificate must be an absolute path")
        if not isinstance(domains, list) or not domains or any(
                not isinstance(name, str) or not acme.DOMAIN_RE.fullmatch(name) for name in domains):
            raise ValueError(f"{sid} domains must contain unique DNS names")
        normalized_domains = [name.lower() for name in domains]
        if len(set(normalized_domains)) != len(normalized_domains):
            raise ValueError(f"{sid} domains must contain unique DNS names")
        service["domains"] = normalized_domains
        ip_sans = service.get("ip_sans", [])
        if not isinstance(ip_sans, list) or any(not isinstance(value, str) for value in ip_sans):
            raise ValueError(f"{sid} ip_sans must be an array of IP address strings")
        try:
            normalized_ips = [ipaddress.ip_address(value).compressed for value in ip_sans]
        except ValueError as exc:
            raise ValueError(f"{sid} ip_sans contains an invalid address") from exc
        if len(set(normalized_ips)) != len(normalized_ips):
            raise ValueError(f"{sid} ip_sans must contain unique addresses")
        service["ip_sans"] = normalized_ips
    command = config.get("notify_command")
    if (not isinstance(command, list) or not command or
            any(not isinstance(arg, str) or not arg for arg in command) or
            not pathlib.Path(command[0]).is_absolute()):
        raise ValueError("notify_command must be a non-empty argv array with an absolute executable")
    state = config.get("state_file")
    if not isinstance(state, str) or not pathlib.Path(state).is_absolute():
        raise ValueError("state_file must be an absolute path")
    return config


def _certificate_alt_names(certificate: pathlib.Path) -> tuple[set[str], set[str]]:
    result = subprocess.run(
        [OPENSSL, "x509", "-in", str(certificate), "-noout", "-ext", "subjectAltName"],
        stdin=subprocess.DEVNULL, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL,
        timeout=10, check=False,
    )
    text = result.stdout.decode("utf-8", "replace")
    if result.returncode or "X509v3 Subject Alternative Name" not in text:
        raise ValueError("certificate has no readable Subject Alternative Name extension")
    body = text.split("X509v3 Subject Alternative Name", 1)[1].partition("\n")[2]
    dns: set[str] = set()
    ips: set[str] = set()
    tokens = [item.strip() for item in re.split(r",|\n", body) if item.strip()]
    for token in tokens:
        if token.startswith("DNS:"):
            dns.add(token[4:].strip().lower())
        elif token.startswith("IP Address:"):
            try:
                ips.add(ipaddress.ip_address(token[len("IP Address:"):].strip()).compressed)
            except ValueError as exc:
                raise ValueError("certificate contains an invalid IP Subject Alternative Name") from exc
        else:
            raise ValueError("certificate contains an unsupported Subject Alternative Name type")
    return dns, ips


def inspect_certificate(path: pathlib.Path, expected_domains: set[str], now: dt.datetime,
                        expected_ips: set[str] | None = None) -> dict[str, Any]:
    resolved = path.resolve(strict=True)
    if not resolved.is_file():
        raise ValueError("certificate path is not a regular file")
    output = subprocess.run(
        [OPENSSL, "x509", "-in", str(resolved), "-noout", "-dates", "-fingerprint", "-sha256"],
        stdin=subprocess.DEVNULL, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL,
        timeout=10, check=False,
    )
    if output.returncode:
        raise ValueError("OpenSSL could not parse the configured certificate")
    fields = dict(line.split("=", 1) for line in output.stdout.decode("ascii", "replace").splitlines()
                  if "=" in line)
    try:
        not_after = dt.datetime.strptime(fields["notAfter"], "%b %d %H:%M:%S %Y %Z").replace(tzinfo=dt.timezone.utc)
        fingerprint = fields["sha256 Fingerprint"].replace(":", "").casefold()
    except (KeyError, ValueError) as exc:
        raise ValueError("certificate validity or fingerprint could not be parsed") from exc
    names, ips = _certificate_alt_names(resolved)
    if names != expected_domains or ips != (expected_ips or set()):
        raise ValueError("certificate DNS/IP SANs do not match the monitored service inventory")
    remaining = (not_after - now).total_seconds()
    return {"fingerprint": fingerprint, "not_after": not_after.isoformat(), "remaining_seconds": remaining}


def assess(config: dict[str, Any], previous: dict[str, Any], now: dt.datetime) -> tuple[dict[str, Any], list[dict[str, str]], list[dict[str, Any]]]:
    current: dict[str, Any] = {}
    alerts: list[dict[str, str]] = []
    results: list[dict[str, Any]] = []
    warn_seconds = config["warning_days"] * 86400
    critical_seconds = config["critical_days"] * 86400
    for service in config["services"]:
        sid = service["id"]
        try:
            cert = inspect_certificate(
                pathlib.Path(service["certificate"]), set(service["domains"]), now,
                set(service.get("ip_sans", [])))
            remaining = cert["remaining_seconds"]
            if remaining <= 0:
                status = "crit"
                detail = f"expired at {cert['not_after']}"
            elif remaining <= critical_seconds:
                status = "crit"
                detail = f"expires in {int(remaining // 86400)} days at {cert['not_after']}"
            elif remaining <= warn_seconds:
                status = "warn"
                detail = f"expires in {int(remaining // 86400)} days at {cert['not_after']}"
            else:
                status = "ok"
                detail = f"valid until {cert['not_after']}"
        except (OSError, ValueError, subprocess.TimeoutExpired, acme.AcmeError) as exc:
            cert = {"fingerprint": "unavailable", "not_after": None}
            status = "crit"
            detail = f"certificate check failed: {exc}"
        old = previous.get(sid, {})
        if not isinstance(old, dict):
            old = {}
        old_status = old.get("status") if isinstance(old, dict) else None
        fingerprint = cert["fingerprint"]
        current[sid] = {"status": status, "fingerprint": fingerprint, "not_after": cert["not_after"]}
        results.append({"id": sid, "status": status, "detail": detail})
        should_publish = status != "ok" and (old_status != status or old.get("fingerprint") != fingerprint)
        recovered = status == "ok" and old_status in {"warn", "crit"}
        if should_publish or recovered:
            severity = "info" if recovered else status
            title = f"{sid}: certificate {('recovered' if recovered else status)}"
            alerts.append({"severity": severity, "title": title, "body": f"{sid} ({', '.join(service['domains'])}): {detail}."})
    return current, alerts, results


def load_state(path: pathlib.Path) -> dict[str, Any]:
    if not path.exists():
        return {}
    state = _private_json(path)
    services = state.get("services", {})
    if not isinstance(services, dict):
        raise ValueError("state file services must be an object")
    return services


def save_state(path: pathlib.Path, services: dict[str, Any]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    fd, name = tempfile.mkstemp(prefix=f".{path.name}.", dir=path.parent)
    try:
        os.fchmod(fd, 0o600)
        with os.fdopen(fd, "w", encoding="utf-8") as stream:
            json.dump({"schema_version": 1, "services": services}, stream, sort_keys=True)
            stream.write("\n")
            stream.flush()
            os.fsync(stream.fileno())
        os.replace(name, path)
    finally:
        try:
            os.unlink(name)
        except FileNotFoundError:
            pass


def publish(command: list[str], alert: dict[str, str], runner: Callable[..., Any] = subprocess.run) -> None:
    try:
        trusted_executable = _trusted_notifier(command[0])
        safe_command = [str(trusted_executable), *command[1:]]
        result = runner(safe_command + ["--severity", alert["severity"], "--title", alert["title"],
                                   "--body", alert["body"], "--source", "solarinet-pki"],
                        stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL,
                        stderr=subprocess.DEVNULL, timeout=20, check=False)
    except (OSError, subprocess.TimeoutExpired) as exc:
        raise RuntimeError("notification publisher could not complete") from exc
    if result.returncode:
        raise RuntimeError("notification publisher failed")


def _trusted_notifier(executable: str) -> pathlib.Path:
    """Require root-owned, non-writable code and path before root executes it."""
    candidate = pathlib.Path(executable)
    if not candidate.is_absolute():
        raise ValueError("notification executable must be an absolute path")
    try:
        absolute = pathlib.Path(os.path.abspath(candidate))
        for entry in [absolute, *absolute.parents]:
            info = entry.lstat()
            if info.st_uid != 0:
                raise ValueError("notification executable path must be root-owned")
            if stat.S_ISDIR(info.st_mode) and info.st_mode & (stat.S_IWGRP | stat.S_IWOTH):
                raise ValueError("notification executable path contains a writable directory")
        canonical = absolute.resolve(strict=True)
        for entry in [canonical, *canonical.parents]:
            info = entry.lstat()
            if info.st_uid != 0:
                raise ValueError("notification executable path must be root-owned")
            if stat.S_ISDIR(info.st_mode) and info.st_mode & (stat.S_IWGRP | stat.S_IWOTH):
                raise ValueError("notification executable path contains a writable directory")
            if entry == canonical and (not stat.S_ISREG(info.st_mode)
                                       or info.st_mode & (stat.S_IWGRP | stat.S_IWOTH)):
                raise ValueError("notification executable must be a protected regular file")
        if not os.access(canonical, os.X_OK):
            raise ValueError("notification executable is missing or not executable")
    except OSError as exc:
        raise ValueError("notification executable path cannot be verified") from exc
    return canonical


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--config", required=True, type=pathlib.Path)
    parser.add_argument("--dry-run", action="store_true", help="evaluate and print alerts without publishing or saving state")
    args = parser.parse_args(argv)
    try:
        config = load_config(args.config)
        state_path = pathlib.Path(config["state_file"])
        previous = load_state(state_path)
        now = dt.datetime.now(dt.timezone.utc)
        current, alerts, results = assess(config, previous, now)
        for result in results:
            print(f"{result['status'].upper()} {result['id']}: {result['detail']}")
        if args.dry_run:
            for alert in alerts:
                print(f"ALERT {alert['severity'].upper()} {alert['title']}: {alert['body']}")
            return 1 if any(row["status"] == "crit" for row in results) else 0
        for alert in alerts:
            publish(config["notify_command"], alert)
        save_state(state_path, current)
        return 0
    except (OSError, ValueError, RuntimeError) as exc:
        print(f"acme-expiry-watch: {exc}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
