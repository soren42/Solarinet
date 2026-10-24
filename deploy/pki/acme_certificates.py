#!/usr/bin/env python3
"""Enroll and atomically deploy web certificates through SolariNet ACME.

The service registry is root-managed and contains no private keys. Certbot
performs ACME protocol work; this module validates and installs the resulting
lineage before asking the configured web service to reload.
"""
from __future__ import annotations

import argparse
import datetime as dt
import fcntl
import hashlib
import ipaddress
import json
import os
import pathlib
import re
import shutil
import shlex
import stat
import subprocess
import sys
import tempfile
from typing import Any, Callable


class AcmeError(Exception):
    """Expected configuration, certificate, or deployment error."""


DOMAIN_RE = re.compile(r"(?=^.{1,253}$)(?:[a-z0-9](?:[a-z0-9-]{0,61}[a-z0-9])?)(?:\.(?:[a-z0-9](?:[a-z0-9-]{0,61}[a-z0-9])?))*$", re.I)
SERVICE_ID_RE = re.compile(r"^[a-z][a-z0-9_-]{0,31}$")
LINEAGE_RE = re.compile(r"^[A-Za-z0-9][A-Za-z0-9._-]{0,127}$")
MAX_CONFIG_BYTES = 256 * 1024
MIN_CERTIFICATE_LIFETIME = dt.timedelta(days=6)
MAX_APPLE_TLS_LIFETIME = dt.timedelta(days=825)
OPENSSL = "/usr/bin/openssl"
TARGET_NAME_RE = re.compile(r"^[A-Za-z0-9][A-Za-z0-9._-]{0,63}$")


def _run(args: list[str], *, timeout: int = 30, input_bytes: bytes | None = None) -> bytes:
    try:
        result = subprocess.run(args, check=False, capture_output=True,
                                input=input_bytes, stdin=subprocess.DEVNULL if input_bytes is None else None,
                                timeout=timeout)
    except (OSError, subprocess.TimeoutExpired) as exc:
        raise AcmeError(f"command could not complete: {pathlib.Path(args[0]).name}") from exc
    if result.returncode:
        message = _summarize_diagnostics(result.stderr, result.stdout)
        raise AcmeError(f"{pathlib.Path(args[0]).name} failed: {message}")
    return result.stdout


def _require_openssl3() -> None:
    version = _run([OPENSSL, "version"]).decode("ascii", "replace")
    match = re.search(r"\bOpenSSL\s+(\d+)\.(\d+)", version)
    if not match or int(match.group(1)) < 3:
        raise AcmeError("ACME certificate deployment requires OpenSSL 3.0 or later")


def _require_real_path(path: pathlib.Path, *, directory: bool, uid: int,
                       allow_missing_leaf: bool = False) -> None:
    current = pathlib.Path(path.anchor)
    parts = path.parts[1:]
    for index, part in enumerate(parts):
        current = current / part
        try:
            info = current.lstat()
        except FileNotFoundError as exc:
            if allow_missing_leaf and index == len(parts) - 1:
                return
            raise AcmeError(f"required path is missing: {current}") from exc
        except OSError as exc:
            raise AcmeError(f"cannot inspect path: {current}") from exc
        if stat.S_ISLNK(info.st_mode):
            raise AcmeError(f"path must not contain symlinks: {current}")
        final = index == len(parts) - 1
        if not final and not stat.S_ISDIR(info.st_mode):
            raise AcmeError(f"path parent is not a directory: {current}")
        if final and ((directory and not stat.S_ISDIR(info.st_mode)) or
                      (not directory and not stat.S_ISREG(info.st_mode))):
            raise AcmeError(f"path has the wrong type: {current}")
        if final and info.st_uid != uid:
            raise AcmeError(f"path must be owned by uid {uid}: {current}")
        if not final and info.st_uid not in (0, uid):
            raise AcmeError(f"path parent must be owned by root or uid {uid}: {current}")
        if info.st_mode & 0o022 and not info.st_mode & stat.S_ISVTX:
            raise AcmeError(f"path must not be group- or world-writable: {current}")


def _read_json(path: pathlib.Path, *, private: bool = False) -> dict[str, Any]:
    try:
        info = path.lstat()
        if not stat.S_ISREG(info.st_mode) or info.st_uid != os.geteuid():
            raise AcmeError(f"configuration must be a regular file owned by uid {os.geteuid()}")
        if info.st_mode & (stat.S_IWGRP | stat.S_IWOTH):
            raise AcmeError("configuration must not be group- or world-writable")
        if private and stat.S_IMODE(info.st_mode) & 0o077:
            raise AcmeError("private configuration permissions must be 0600 or stricter")
        if info.st_size > MAX_CONFIG_BYTES:
            raise AcmeError("configuration exceeds size limit")
        payload = path.read_bytes()
        value = json.loads(payload, object_pairs_hook=_unique_keys)
    except AcmeError:
        raise
    except (OSError, UnicodeError, json.JSONDecodeError) as exc:
        raise AcmeError(f"cannot read configuration: {path}") from exc
    if not isinstance(value, dict):
        raise AcmeError("configuration must be a JSON object")
    return value


def _unique_keys(pairs: list[tuple[str, Any]]) -> dict[str, Any]:
    result: dict[str, Any] = {}
    for key, value in pairs:
        if key in result:
            raise AcmeError(f"duplicate configuration key: {key}")
        result[key] = value
    return result


def load_registry(path: pathlib.Path) -> dict[str, Any]:
    config = _read_json(path, private=True)
    if set(config) != {"schema_version", "acme", "services"} or config.get("schema_version") != 1:
        raise AcmeError("unsupported or malformed ACME registry")
    acme = config["acme"]
    services = config["services"]
    if not isinstance(acme, dict) or set(acme) != {"directory_url", "root_ca", "webroot", "certbot_root", "target_root"}:
        raise AcmeError("ACME settings must define directory_url, root_ca, webroot, certbot_root, and target_root")
    url = acme["directory_url"]
    if not isinstance(url, str) or not url.startswith("https://") or "/acme/" not in url:
        raise AcmeError("ACME directory_url must be a private HTTPS ACME directory")
    for field in ("root_ca", "webroot", "certbot_root", "target_root"):
        if (not isinstance(acme[field], str) or not pathlib.Path(acme[field]).is_absolute() or
                any(part in (".", "..") for part in acme[field].split("/")) or
                str(pathlib.Path(acme[field])) != acme[field]):
            raise AcmeError(f"ACME {field} must be an absolute path")
    certbot_root = pathlib.Path(acme["certbot_root"])
    public_certbot = pathlib.Path("/etc/letsencrypt")
    certbot_real = certbot_root.resolve(strict=False)
    public_real = public_certbot.resolve(strict=False)
    if (certbot_root == public_certbot or certbot_root in public_certbot.parents or
            public_certbot in certbot_root.parents or certbot_real == public_real or
            certbot_real in public_real.parents or public_real in certbot_real.parents):
        raise AcmeError("private ACME certificates require a separate Certbot store, outside /etc/letsencrypt")
    if not isinstance(services, list) or not services:
        raise AcmeError("services must be a non-empty array")
    ids: set[str] = set()
    lineages: set[str] = set()
    domains: set[str] = set()
    targets: set[str] = set()
    for service in services:
        required = {"id", "lineage", "domains", "target", "check_command", "reload_command", "group"}
        if not isinstance(service, dict) or set(service) != required:
            raise AcmeError("each service must define exactly id, lineage, domains, target, check_command, reload_command, and group")
        service_id = service["id"]
        lineage = service["lineage"]
        if not isinstance(service_id, str) or not SERVICE_ID_RE.fullmatch(service_id) or service_id in ids:
            raise AcmeError("service ids must be unique lowercase identifiers")
        if not isinstance(lineage, str) or not LINEAGE_RE.fullmatch(lineage) or lineage in lineages:
            raise AcmeError("certificate lineages must be unique safe names")
        ids.add(service_id)
        lineages.add(lineage)
        names = service["domains"]
        if not isinstance(names, list) or not names or any(
            not isinstance(name, str) or not DOMAIN_RE.fullmatch(name) or name != name.lower()
            for name in names
        ) or len(set(names)) != len(names):
            raise AcmeError(f"service {service_id} must have unique lowercase DNS names")
        if any(_is_ip_address(name) for name in names):
            raise AcmeError(f"service {service_id} names must be DNS names, not IP addresses")
        if domains.intersection(names):
            raise AcmeError("a DNS name may belong to only one service certificate")
        domains.update(names)
        target = service["target"]
        if (not isinstance(target, str) or not pathlib.Path(target).is_absolute() or
                any(part in (".", "..") for part in target.split("/")) or
                str(pathlib.Path(target)) != target or target.endswith("/")):
            raise AcmeError(f"service {service_id} target must be an absolute path")
        target_path = pathlib.Path(target)
        if (not TARGET_NAME_RE.fullmatch(target_path.name) or target_path.name in (".", "..") or
                target_path.parent != pathlib.Path(acme["target_root"])):
            raise AcmeError(f"service {service_id} target must be a direct child of target_root")
        if target in targets:
            raise AcmeError("certificate target paths must be unique")
        targets.add(target)
        if service["group"] is not None and (not isinstance(service["group"], str) or not service["group"]):
            raise AcmeError(f"service {service_id} group must be a group name or null")
        for command_name in ("check_command", "reload_command"):
            command = service[command_name]
            if (not isinstance(command, list) or not command or
                    any(not isinstance(arg, str) or "\x00" in arg for arg in command) or
                    not pathlib.Path(command[0]).is_absolute()):
                raise AcmeError(f"service {service_id} {command_name} must be an absolute argv array")
    return config


def _is_ip_address(value: str) -> bool:
    try:
        ipaddress.ip_address(value)
        return True
    except ValueError:
        return False


def _service_for_env(config: dict[str, Any], env: dict[str, str]) -> tuple[dict[str, Any], pathlib.Path, pathlib.Path]:
    raw_lineage = env.get("RENEWED_LINEAGE", "")
    renewed_domains = set(env.get("RENEWED_DOMAINS", "").split())
    acme = config["acme"]
    certbot_root = pathlib.Path(acme["certbot_root"])
    _require_real_path(certbot_root, directory=True, uid=os.geteuid())
    _require_real_path(certbot_root / "live", directory=True, uid=os.geteuid())
    for service in config["services"]:
        expected = certbot_root / "live" / service["lineage"]
        try:
            resolved_expected = expected.resolve(strict=True)
        except OSError:
            resolved_expected = expected
        if raw_lineage not in {str(expected), str(resolved_expected)}:
            continue
        if renewed_domains != set(service["domains"]):
            raise AcmeError("Certbot renewed domains do not match the service registry")
        try:
            info = expected.lstat()
            archive_root = certbot_root / "archive" / service["lineage"]
            archive_info = archive_root.lstat()
        except OSError as exc:
            raise AcmeError("Certbot lineage does not resolve") from exc
        if (not stat.S_ISDIR(info.st_mode) or info.st_uid != os.geteuid() or info.st_mode & 0o022 or
                stat.S_ISLNK(info.st_mode) or not stat.S_ISDIR(archive_info.st_mode) or
                archive_info.st_uid != os.geteuid() or archive_info.st_mode & 0o022 or archive_root.is_symlink()):
            raise AcmeError("Certbot live lineage and archive must be owned real directories")
        return service, expected, archive_root
    raise AcmeError("RENEWED_LINEAGE is not registered")


def _certificate_sans(certificate: pathlib.Path) -> set[str]:
    output = _run([OPENSSL, "x509", "-in", str(certificate), "-noout", "-ext", "subjectAltName"])
    text = output.decode("utf-8", "replace")
    if "X509v3 Subject Alternative Name" not in text:
        raise AcmeError("certificate has no Subject Alternative Name extension")
    header, _, body = text.split("X509v3 Subject Alternative Name", 1)[1].partition("\n")
    header = header.removeprefix(":").strip().casefold()
    if header not in ("", "critical"):
        raise AcmeError("certificate has an unsupported Subject Alternative Name marker")
    extension = body
    values = [part.strip() for part in re.split(r",|\n", extension) if part.strip()]
    if any(not value.startswith("DNS:") for value in values):
        raise AcmeError("certificate contains a non-DNS Subject Alternative Name")
    return {value[4:].strip().lower() for value in values}


def _require_tls_leaf(certificate: pathlib.Path) -> None:
    required = {
        "extendedKeyUsage": {"tls web server authentication", "1.3.6.1.5.5.7.3.1"},
        "keyUsage": {"digital signature"},
    }
    for extension, allowed in required.items():
        output = _run([OPENSSL, "x509", "-in", str(certificate), "-noout", "-ext", extension])
        lines = output.decode("utf-8", "replace").splitlines()
        tokens = {
            token.strip().casefold()
            for line in lines[1:]
            for token in line.split(",")
            if token.strip()
        }
        if not allowed.intersection(tokens):
            raise AcmeError(f"certificate is missing the required {extension} constraint")
    certificate_text = _run([OPENSSL, "x509", "-in", str(certificate),
                             "-noout", "-text"]).decode("utf-8", "replace")
    if "X509v3 Basic Constraints" in certificate_text:
        constraints = certificate_text.split("X509v3 Basic Constraints", 1)[1].split("X509v3", 1)[0]
        if re.search(r"\bCA\s*:\s*TRUE\b", constraints, re.I) or not re.search(r"\bCA\s*:\s*FALSE\b", constraints, re.I):
            raise AcmeError("certificate basicConstraints must not mark a TLS leaf as a CA")

    _require_apple_tls_compatibility(certificate)


def _require_apple_tls_compatibility(certificate: pathlib.Path) -> None:
    """Reject leaf algorithms and validity periods Apple clients do not accept."""
    text = _run([OPENSSL, "x509", "-in", str(certificate), "-noout", "-text"])
    certificate_text = text.decode("utf-8", "replace")
    signature = re.search(r"^\s*Signature Algorithm:\s*([^\r\n]+)", certificate_text, re.M)
    if not signature or not re.search(r"sha(?:256|384|512)", signature.group(1), re.I):
        raise AcmeError("certificate signature must use SHA-256 or a stronger SHA-2 algorithm")

    public_key = _run([OPENSSL, "x509", "-in", str(certificate), "-pubkey", "-noout"])
    details = _run([OPENSSL, "pkey", "-pubin", "-text", "-noout"], input_bytes=public_key)
    key_text = details.decode("utf-8", "replace")
    bit_count = re.search(r"Public-Key:\s*\((\d+) bit\)", key_text)
    key_algorithm = re.search(r"^\s*Public Key Algorithm:\s*([^\r\n]+)", certificate_text, re.M)
    if not bit_count:
        raise AcmeError("certificate public key type could not be validated")
    bits = int(bit_count.group(1))
    curve = re.search(r"ASN1 OID:\s*([^\r\n]+)", key_text)
    algorithm = key_algorithm.group(1).casefold() if key_algorithm else ""
    if bits >= 2048 and "rsa" in algorithm:
        pass
    elif bits >= 256 and "ec" in algorithm and curve and curve.group(1).strip().casefold() in {
        "prime256v1", "secp384r1", "secp521r1",
    }:
        pass
    else:
        raise AcmeError("certificate public key must be RSA with at least 2048 bits or NIST P-256 or stronger")

    output = _run([OPENSSL, "x509", "-in", str(certificate), "-noout",
                   "-startdate", "-enddate"]).decode("ascii", "replace")
    dates = dict(line.split("=", 1) for line in output.splitlines() if "=" in line)
    try:
        start = dt.datetime.strptime(dates["notBefore"], "%b %d %H:%M:%S %Y %Z")
        end = dt.datetime.strptime(dates["notAfter"], "%b %d %H:%M:%S %Y %Z")
    except (KeyError, ValueError) as exc:
        raise AcmeError("certificate validity dates could not be parsed") from exc
    if end - start > MAX_APPLE_TLS_LIFETIME:
        raise AcmeError("certificate validity exceeds Apple's 825-day TLS limit")


def _require_minimum_lifetime(certificate: pathlib.Path) -> None:
    output = _run([OPENSSL, "x509", "-in", str(certificate), "-noout",
                   "-startdate", "-enddate"]).decode("ascii", "replace")
    dates = dict(line.split("=", 1) for line in output.splitlines() if "=" in line)
    try:
        start = dt.datetime.strptime(dates["notBefore"], "%b %d %H:%M:%S %Y %Z")
        end = dt.datetime.strptime(dates["notAfter"], "%b %d %H:%M:%S %Y %Z")
    except (KeyError, ValueError) as exc:
        raise AcmeError("certificate validity dates could not be parsed") from exc
    if end - start < MIN_CERTIFICATE_LIFETIME:
        raise AcmeError("certificate lifetime is below the six-day minimum required for safe renewal margin")


def _check_lineage(config: dict[str, Any], service: dict[str, Any], lineage: pathlib.Path,
                   archive: pathlib.Path) -> tuple[bytes, bytes, str]:
    _require_openssl3()
    names = {"cert.pem": "cert", "chain.pem": "chain", "fullchain.pem": "fullchain", "privkey.pem": "privkey"}
    resolved: dict[str, pathlib.Path] = {}
    generations: set[str] = set()
    try:
        if {item.name for item in lineage.iterdir()} != {*names, "README"}:
            raise AcmeError("Certbot live lineage contains an unexpected file set")
        readme_info = (lineage / "README").lstat()
        if not stat.S_ISREG(readme_info.st_mode) or readme_info.st_uid != os.geteuid():
            raise AcmeError("Certbot live lineage README has an unsafe type or owner")
        archive_resolved = archive.resolve(strict=True)
        for live_name, prefix in names.items():
            link = lineage / live_name
            link_info = link.lstat()
            if not stat.S_ISLNK(link_info.st_mode) or link_info.st_uid != os.geteuid():
                raise AcmeError("Certbot live artifacts must be symlinks into the archive")
            source = link.resolve(strict=True)
            if source.parent != archive_resolved:
                raise AcmeError("Certbot live artifact resolves outside its matching archive lineage")
            match = re.fullmatch(prefix + r"([0-9]+)\.pem", source.name)
            if not match:
                raise AcmeError("Certbot live artifact has an unexpected archive filename")
            generations.add(match.group(1))
            source_info = source.lstat()
            if (not stat.S_ISREG(source_info.st_mode) or source_info.st_uid != os.geteuid() or
                    source_info.st_mode & 0o022):
                raise AcmeError("Certbot archive artifacts must be regular files owned by the effective uid")
            if prefix == "privkey" and source_info.st_mode & 0o077:
                raise AcmeError("Certbot private key must not be accessible to group or other users")
            resolved[live_name] = source
        if len(generations) != 1:
            raise AcmeError("Certbot live artifacts point to mixed archive generations")
        cert_data = resolved["cert.pem"].read_bytes()
        chain_data = resolved["chain.pem"].read_bytes()
        fullchain_data = resolved["fullchain.pem"].read_bytes()
        key_data = resolved["privkey.pem"].read_bytes()
    except AcmeError:
        raise
    except OSError as exc:
        raise AcmeError("Certbot lineage is incomplete or unreadable") from exc
    if not cert_data or not fullchain_data or not key_data:
        raise AcmeError("Certbot lineage contains an empty certificate or key")
    if fullchain_data != cert_data + chain_data:
        raise AcmeError("Certbot fullchain does not match its leaf and issuer-chain files")
    cert = resolved["cert.pem"]
    chain = resolved["chain.pem"]
    if _certificate_sans(cert) != set(service["domains"]):
        raise AcmeError("certificate SANs do not exactly match the service registry")
    _require_tls_leaf(cert)
    _require_minimum_lifetime(cert)
    _run([OPENSSL, "x509", "-in", str(cert), "-noout", "-checkend", "300"])
    ca = pathlib.Path(config["acme"]["root_ca"])
    _require_real_path(ca, directory=False, uid=os.geteuid())
    _run([OPENSSL, "verify", "-purpose", "sslserver", "-CAfile", str(ca),
          "-no-CApath", "-no-CAstore",
          "-untrusted", str(chain), str(cert)])
    cert_public = _run([OPENSSL, "x509", "-in", str(cert), "-pubkey", "-noout"])
    key_public = _run([OPENSSL, "pkey", "-in", str(resolved['privkey.pem']), "-pubout"])
    if cert_public.strip() != key_public.strip():
        raise AcmeError("certificate and private key do not match")
    fingerprint = _run([OPENSSL, "x509", "-in", str(cert), "-outform", "DER"])
    release_id = hashlib.sha256(fingerprint).hexdigest()
    return fullchain_data, key_data, release_id


def _owned_directory(path: pathlib.Path, mode: int, uid: int, gid: int) -> None:
    info = path.lstat()
    if not stat.S_ISDIR(info.st_mode) or info.st_uid != uid:
        raise AcmeError(f"certificate target directory has unexpected owner or type: {path}")
    if info.st_gid != gid:
        os.chown(path, uid, gid)
    if stat.S_IMODE(info.st_mode) != mode:
        path.chmod(mode)


def _verify_existing_release(release: pathlib.Path, fullchain: bytes, private_key: bytes,
                             uid: int, gid: int, key_mode: int) -> None:
    info = release.lstat()
    if (not stat.S_ISDIR(info.st_mode) or info.st_uid != uid or info.st_gid != gid or
            stat.S_IMODE(info.st_mode) != 0o750):
        raise AcmeError("existing certificate release has unexpected ownership or mode")
    expected = {"fullchain.pem": (fullchain, 0o644), "privkey.pem": (private_key, key_mode)}
    try:
        if {item.name for item in release.iterdir()} != set(expected):
            raise AcmeError("existing certificate release contains unexpected files")
        for name, (payload, mode) in expected.items():
            path = release / name
            file_info = path.lstat()
            if (not stat.S_ISREG(file_info.st_mode) or file_info.st_uid != uid or
                    file_info.st_gid != gid or stat.S_IMODE(file_info.st_mode) != mode or
                    path.read_bytes() != payload):
                raise AcmeError("existing certificate release does not match its fingerprint and permissions")
    except OSError as exc:
        raise AcmeError("could not verify existing certificate release") from exc


def _write_payload(path: pathlib.Path, payload: bytes, mode: int, uid: int, gid: int) -> None:
    descriptor = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_NOFOLLOW,
                         mode)
    try:
        os.fchown(descriptor, uid, gid)
        os.fchmod(descriptor, mode)
        with os.fdopen(descriptor, "wb", closefd=False) as output:
            output.write(payload)
            output.flush()
            os.fsync(output.fileno())
    finally:
        os.close(descriptor)


def _reconcile_release_tree(releases: pathlib.Path, uid: int, gid: int, key_mode: int) -> None:
    for item in releases.iterdir():
        if item.name.startswith(".stage-"):
            info = item.lstat()
            if not stat.S_ISDIR(info.st_mode) or info.st_uid != uid:
                raise AcmeError("stale certificate staging path has an unsafe type or owner")
            shutil.rmtree(item)
            continue
        if not re.fullmatch(r"[0-9a-f]{64}", item.name):
            raise AcmeError("certificate release directory has an unexpected name")
        info = item.lstat()
        if not stat.S_ISDIR(info.st_mode) or info.st_uid != uid:
            raise AcmeError("certificate release has an unsafe type or owner")
        _owned_directory(item, 0o750, uid, gid)
        for name, mode in (("fullchain.pem", 0o644), ("privkey.pem", key_mode)):
            path = item / name
            file_info = path.lstat()
            if not stat.S_ISREG(file_info.st_mode) or file_info.st_uid != uid:
                raise AcmeError("certificate release file has an unsafe type or owner")
            if file_info.st_gid != gid:
                os.chown(path, uid, gid)
            if stat.S_IMODE(file_info.st_mode) != mode:
                path.chmod(mode)
        if {path.name for path in item.iterdir()} != {"fullchain.pem", "privkey.pem"}:
            raise AcmeError("certificate release contains an unexpected file set")
    _fsync_directory(releases)


def _atomic_link(parent: pathlib.Path, name: str, target: pathlib.Path, uid: int, gid: int) -> None:
    temp = parent / f".{name}-{os.getpid()}-{os.urandom(6).hex()}"
    try:
        temp.symlink_to(target)
        os.chown(temp, uid, gid, follow_symlinks=False)
        os.replace(temp, parent / name)
        _fsync_directory(parent)
    finally:
        temp.unlink(missing_ok=True)


def _fsync_directory(path: pathlib.Path) -> None:
    descriptor = os.open(path, os.O_RDONLY | getattr(os, "O_DIRECTORY", 0))
    try:
        os.fsync(descriptor)
    finally:
        os.close(descriptor)


def _result_diagnostic(result: Any) -> str:
    stderr = getattr(result, "stderr", "") or ""
    stdout = getattr(result, "stdout", "") or ""
    return _summarize_diagnostics(stderr, stdout)


def _summarize_diagnostics(stderr: str | bytes, stdout: str | bytes = b"") -> str:
    pieces: list[str] = []
    for value in (stderr, stdout):
        if isinstance(value, bytes):
            value = value.decode("utf-8", "replace")
        pieces.extend(line.strip() for line in str(value).splitlines() if line.strip())
    if not pieces:
        return "command returned a non-zero status"
    return " | ".join(line[:240] for line in pieces[-5:])[-1000:]


def _current_target(target: pathlib.Path, releases: pathlib.Path) -> pathlib.Path | None:
    current = target / "current"
    if not current.exists():
        if current.is_symlink():
            return None
        return None
    if not current.is_symlink():
        raise AcmeError("current certificate pointer must be a symlink")
    try:
        resolved = current.resolve(strict=True)
    except OSError as exc:
        if current.is_symlink() and not current.exists():
            return None
        raise AcmeError("current certificate pointer cannot be resolved") from exc
    if resolved.parent != releases.resolve(strict=True) or not resolved.is_dir():
        raise AcmeError("current certificate pointer escapes the release directory")
    return resolved


def deploy(config: dict[str, Any], env: dict[str, str], runner: Callable[..., Any] = subprocess.run) -> str:
    service, lineage, archive = _service_for_env(config, env)
    fullchain, private_key, release_id = _check_lineage(config, service, lineage, archive)
    uid = os.geteuid()
    group_name = service["group"]
    if group_name is None:
        gid = os.getegid()
    else:
        import grp
        try:
            gid = grp.getgrnam(group_name).gr_gid
        except KeyError as exc:
            raise AcmeError(f"configured certificate group does not exist: {group_name}") from exc
    target = pathlib.Path(service["target"])
    parent = target.parent
    if parent != pathlib.Path(config["acme"]["target_root"]):
        raise AcmeError("certificate target is outside the configured target_root")
    _require_real_path(parent, directory=True, uid=uid)
    if target.is_symlink() or (target.exists() and not target.is_dir()):
        raise AcmeError("certificate target must be a real directory")
    if not target.exists():
        target.mkdir(mode=0o750, exist_ok=True)
        os.chown(target, uid, gid)
    _owned_directory(target, 0o750, uid, gid)
    releases = target / "releases"
    if releases.is_symlink() or (releases.exists() and not releases.is_dir()):
        raise AcmeError("certificate releases path must be a real directory")
    if not releases.exists():
        releases.mkdir(mode=0o750, exist_ok=True)
        os.chown(releases, uid, gid)
    _owned_directory(releases, 0o750, uid, gid)

    lock_path = target / ".deploy.lock"
    fd = os.open(lock_path, os.O_CREAT | os.O_RDWR | os.O_NOFOLLOW | os.O_NONBLOCK, 0o600)
    try:
        lock_info = os.fstat(fd)
        if not stat.S_ISREG(lock_info.st_mode) or lock_info.st_uid != uid:
            raise AcmeError("certificate deploy lock must be a regular file owned by the effective uid")
        os.fchown(fd, uid, gid)
        os.fchmod(fd, 0o600)
        fcntl.flock(fd, fcntl.LOCK_EX)
        _reconcile_release_tree(releases, uid, gid,
                                0o640 if group_name is not None else 0o600)
        previous = _current_target(target, releases)
        release = releases / release_id
        if release.exists():
            _verify_existing_release(release, fullchain, private_key, uid, gid,
                                     0o640 if group_name is not None else 0o600)
        else:
            stage = pathlib.Path(tempfile.mkdtemp(prefix=".stage-", dir=releases))
            try:
                cert_file = stage / "fullchain.pem"
                key_file = stage / "privkey.pem"
                _write_payload(cert_file, fullchain, 0o644, uid, gid)
                _write_payload(key_file, private_key,
                               0o640 if group_name is not None else 0o600, uid, gid)
                os.chown(stage, uid, gid)
                stage.chmod(0o750)
                _fsync_directory(stage)
                os.rename(stage, release)
                _fsync_directory(releases)
            except OSError as exc:
                raise AcmeError("could not stage verified certificate release") from exc
            finally:
                if stage.exists():
                    shutil.rmtree(stage, ignore_errors=True)

        relative = pathlib.Path("releases") / release_id
        if previous == release:
            return f"{service['id']} already uses certificate {release_id}"
        _atomic_link(target, "current", relative, uid, gid)
        reload_attempted = False
        try:
            for command_name in ("check_command", "reload_command"):
                if command_name == "reload_command":
                    reload_attempted = True
                completed = runner(service[command_name], check=False, capture_output=True,
                                   text=True, timeout=60)
                if completed.returncode != 0:
                    action = command_name.removesuffix("_command")
                    raise AcmeError(f"{action} failed for {service['id']}: {_result_diagnostic(completed)}")
        except (OSError, subprocess.TimeoutExpired, AcmeError) as exc:
            pointer_restored = False
            service_restored = not reload_attempted
            restore_diagnostic = ""
            try:
                if previous is None:
                    (target / "current").unlink(missing_ok=True)
                    _fsync_directory(target)
                    pointer_restored = True
                else:
                    _atomic_link(target, "current", pathlib.Path("releases") / previous.name, uid, gid)
                    pointer_restored = True
                if previous is not None and reload_attempted:
                    restored = runner(service["reload_command"], check=False, capture_output=True,
                                      text=True, timeout=60)
                    service_restored = restored.returncode == 0
                    if not service_restored:
                        restore_diagnostic = _result_diagnostic(restored)
            except (OSError, subprocess.TimeoutExpired):
                pass
            if not pointer_restored:
                raise AcmeError(f"deployment failed for {service['id']}; the previous certificate pointer could not be restored") from exc
            if not service_restored:
                if previous is None:
                    raise AcmeError(
                        f"deployment failed for {service['id']}; no previous release existed and "
                        f"service state may be unknown after the failed reload: {exc}") from exc
                raise AcmeError(f"deployment failed for {service['id']}; the previous pointer is active but the service reload failed: {restore_diagnostic or 'command could not complete'}") from exc
            if isinstance(exc, AcmeError):
                raise
            raise AcmeError(f"certificate deployment failed for {service['id']}; previous release restored") from exc
        return f"{service['id']} activated certificate {release_id}"
    finally:
        os.close(fd)


def _certbot_hook(registry: pathlib.Path, *, require_safe: bool) -> str:
    script = pathlib.Path(__file__).absolute()
    if require_safe:
        _require_safe_hook(script)
    return shlex.join([sys.executable, str(script),
                       "--registry", str(registry.resolve()), "--deploy-renewed"])


def _require_certbot4(certbot: str) -> None:
    if not pathlib.Path(certbot).is_absolute():
        raise AcmeError("Certbot path must be absolute")
    version = _run([certbot, "--version"]).decode("ascii", "replace")
    match = re.search(r"\bcertbot\s+(\d+)\.(\d+)\.(\d+)", version, re.I)
    if not match or tuple(map(int, match.groups())) < (4, 0, 0):
        raise AcmeError("private ACME renewal requires Certbot 4.0.0 or later")


def _require_safe_hook(script: pathlib.Path) -> None:
    uid = os.geteuid()
    try:
        current = pathlib.Path(script.anchor)
        for component in script.parts[1:]:
            current = current / component
            info = current.lstat()
            if stat.S_ISLNK(info.st_mode):
                raise AcmeError("Certbot hook path must not contain symlinks")
            if info.st_mode & 0o022:
                raise AcmeError("Certbot hook path must not be group- or world-writable")
            if not stat.S_ISDIR(info.st_mode) and current != script:
                raise AcmeError("Certbot hook path has a non-directory parent")
            if info.st_uid not in (0, uid):
                raise AcmeError("Certbot hook path has an unexpected owner")
        if not stat.S_ISREG(script.lstat().st_mode) or not os.access(script, os.X_OK):
            raise AcmeError("Certbot hook must be an executable regular file")
    except OSError as exc:
        raise AcmeError("Certbot hook path is missing or unreadable") from exc


def _certbot_paths(config: dict[str, Any], *, create: bool) -> tuple[pathlib.Path, pathlib.Path, pathlib.Path]:
    root = pathlib.Path(config["acme"]["certbot_root"])
    if create:
        _require_real_path(pathlib.Path(config["acme"]["root_ca"]), directory=False,
                           uid=os.geteuid())
        _require_real_path(pathlib.Path(config["acme"]["webroot"]), directory=True,
                           uid=os.geteuid())
        _require_real_path(pathlib.Path(config["acme"]["target_root"]), directory=True,
                           uid=os.geteuid())
        _require_real_path(root, directory=True, uid=os.geteuid(), allow_missing_leaf=True)
    if root.is_symlink() or (root.exists() and not root.is_dir()):
        raise AcmeError("dedicated Certbot root must be a real directory")
    if not root.exists():
        if not create:
            return root, root / "work", root / "logs"
        root.mkdir(mode=0o700, parents=True)
    uid = os.geteuid()
    if root.stat().st_uid != uid:
        raise AcmeError("dedicated Certbot root must be owned by the effective uid")
    if create:
        root.chmod(0o700)
        _require_real_path(root, directory=True, uid=uid)
    result = (root, root / "work", root / "logs")
    for directory in result[1:]:
        if directory.is_symlink() or (directory.exists() and not directory.is_dir()):
            raise AcmeError("Certbot work and log paths must be real directories")
        if not directory.exists() and not create:
            continue
        directory.mkdir(mode=0o700, exist_ok=True)
        if directory.stat().st_uid != uid:
            raise AcmeError("Certbot work and log directories must be owned by the effective uid")
        if create:
            directory.chmod(0o700)
    return result


def enrollment_command(config: dict[str, Any], service_id: str, certbot: str,
                       registry: pathlib.Path, *, create_paths: bool = False) -> tuple[list[str], dict[str, str]]:
    acme = config["acme"]
    service = next((item for item in config["services"] if item["id"] == service_id), None)
    if service is None:
        raise AcmeError(f"unknown service id: {service_id}")
    root, work, logs = _certbot_paths(config, create=create_paths)
    argv = [certbot, "certonly", "--non-interactive", "--pre-hook", "",
            "--post-hook", "", "--no-directory-hooks", "--agree-tos",
            "--register-unsafely-without-email", "--no-eff-email",
            "--config-dir", str(root), "--work-dir", str(work), "--logs-dir", str(logs),
            "--server", acme["directory_url"], "--authenticator", "webroot",
            "--preferred-challenges", "http-01", "--webroot-path", acme["webroot"],
            "--cert-name", service["lineage"]]
    for domain in service["domains"]:
        argv.extend(("--domain", domain))
    argv.extend(("--deploy-hook", _certbot_hook(registry, require_safe=create_paths)))
    env = {"REQUESTS_CA_BUNDLE": acme["root_ca"]}
    return argv, env


def renewal_command(config: dict[str, Any], certbot: str, registry: pathlib.Path,
                    *, create_paths: bool = False, force: bool = False) -> tuple[list[str], dict[str, str]]:
    root, work, logs = _certbot_paths(config, create=create_paths)
    argv = [certbot, "renew", "--non-interactive", "--no-random-sleep-on-renew",
            "--pre-hook", "",
            "--post-hook", "", "--no-directory-hooks", "--config-dir", str(root),
            "--work-dir", str(work), "--logs-dir", str(logs),
            "--deploy-hook", _certbot_hook(registry, require_safe=create_paths)]
    if force:
        argv.append("--force-renewal")
    return argv, {"REQUESTS_CA_BUNDLE": config["acme"]["root_ca"]}


def _validate_private_renewal_store(config: dict[str, Any]) -> None:
    root = pathlib.Path(config["acme"]["certbot_root"])
    renewal = root / "renewal"
    if renewal.is_symlink() or not renewal.is_dir():
        raise AcmeError("private Certbot renewal store is missing; enroll all registered services before enabling its timer")
    expected = {service["lineage"] + ".conf" for service in config["services"]}
    found: set[str] = set()
    for item in renewal.iterdir():
        info = item.lstat()
        if not stat.S_ISREG(info.st_mode) or info.st_uid != os.geteuid() or item.suffix != ".conf":
            raise AcmeError("private Certbot renewal directory contains an unsafe or unexpected entry")
        found.add(item.name)
    if found != expected:
        raise AcmeError("private Certbot renewal lineages do not exactly match the service registry")


def _service_environment(config: dict[str, Any], service: dict[str, Any]) -> dict[str, str]:
    root = pathlib.Path(config["acme"]["certbot_root"])
    return {
        "RENEWED_LINEAGE": str(root / "live" / service["lineage"]),
        "RENEWED_DOMAINS": " ".join(service["domains"]),
    }


def _service_group(config: dict[str, Any], service: dict[str, Any]) -> tuple[int, int]:
    uid = os.geteuid()
    if service["group"] is None:
        return uid, os.getegid()
    import grp
    try:
        return uid, grp.getgrnam(service["group"]).gr_gid
    except KeyError as exc:
        raise AcmeError(f"configured certificate group does not exist: {service['group']}") from exc


def _assert_service_deployed(config: dict[str, Any], service: dict[str, Any]) -> None:
    _, lineage, archive = _service_for_env(config, _service_environment(config, service))
    fullchain, private_key, release_id = _check_lineage(config, service, lineage, archive)
    target = pathlib.Path(service["target"])
    releases = target / "releases"
    current = _current_target(target, releases)
    if current is None or current.name != release_id:
        raise AcmeError(f"{service['id']} renewal hook did not activate the current Certbot certificate")
    uid, gid = _service_group(config, service)
    _verify_existing_release(current, fullchain, private_key, uid, gid,
                             0o640 if service["group"] is not None else 0o600)


def _run_and_verify_certbot(command: list[str], environment: dict[str, str], runner: Callable[..., Any],
                            config: dict[str, Any], services: list[dict[str, Any]]) -> int:
    _run_certbot(command, environment, runner)
    for service in services:
        _assert_service_deployed(config, service)
    return 0


def _run_certbot(command: list[str], environment: dict[str, str], runner: Callable[..., Any]) -> int:
    env = dict(os.environ)
    env.update(environment)
    try:
        result = runner(command, check=False, env=env, timeout=1800)
    except (OSError, subprocess.TimeoutExpired) as exc:
        raise AcmeError("Certbot could not complete") from exc
    if result.returncode != 0:
        raise AcmeError(f"Certbot exited with status {result.returncode}")
    return 0


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--registry", type=pathlib.Path, required=True)
    mode = parser.add_mutually_exclusive_group(required=True)
    mode.add_argument("--plan-service", help="print the certbot enrollment command without issuing")
    mode.add_argument("--issue-service", help="request a new/updated certificate for one registered service")
    mode.add_argument("--deploy-service", help="retry deployment from an already-issued registered Certbot lineage")
    mode.add_argument("--renew", action="store_true", help="renew only certificates in the dedicated private-CA Certbot store")
    mode.add_argument("--deploy-renewed", action="store_true", help="validate and deploy Certbot's RENEWED_* lineage")
    parser.add_argument("--force-renewal", action="store_true",
                        help="force renewal of every registered private-CA certificate; valid only with --renew")
    parser.add_argument("--certbot", default="/usr/bin/certbot")
    args = parser.parse_args(argv)
    try:
        config = load_registry(args.registry)
        if args.force_renewal and not args.renew:
            raise AcmeError("--force-renewal is only valid with --renew")
        if args.plan_service:
            command, env = enrollment_command(config, args.plan_service, args.certbot, args.registry)
            print("REQUESTS_CA_BUNDLE=" + env["REQUESTS_CA_BUNDLE"])
            print(json.dumps(command))
            return 0
        if args.issue_service:
            service = next((item for item in config["services"] if item["id"] == args.issue_service), None)
            if service is None:
                raise AcmeError(f"unknown service id: {args.issue_service}")
            _require_certbot4(args.certbot)
            command, env = enrollment_command(config, args.issue_service, args.certbot,
                                              args.registry, create_paths=True)
            return _run_and_verify_certbot(command, env, subprocess.run, config, [service])
        if args.deploy_service:
            service = next((item for item in config["services"] if item["id"] == args.deploy_service), None)
            if service is None:
                raise AcmeError(f"unknown service id: {args.deploy_service}")
            print(deploy(config, _service_environment(config, service)))
            _assert_service_deployed(config, service)
            return 0
        if args.renew:
            _validate_private_renewal_store(config)
            _require_certbot4(args.certbot)
            command, env = renewal_command(config, args.certbot, args.registry,
                                          create_paths=True, force=args.force_renewal)
            return _run_and_verify_certbot(command, env, subprocess.run, config, config["services"])
        print(deploy(config, dict(os.environ)))
        return 0
    except (AcmeError, OSError, ValueError) as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
