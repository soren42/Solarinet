#!/usr/bin/env python3
"""Install or activate an immutable SolariNet dashboard release."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import shutil
import sys
import subprocess
import tarfile
import tempfile
from pathlib import Path, PurePosixPath


VERSION_RE = re.compile(r"[A-Za-z0-9][A-Za-z0-9._+-]{0,63}\Z")
MAX_FILE_BYTES = 256 * 1024 * 1024
PYTHON_RUNTIME_FILES = {
    "apps/requirements.txt",
    "apps/sorsync/db_config.py", "apps/sorsync/delivery.py",
    "apps/sorsync/sor_emitd.py", "apps/sorsync/sor_apply_dnsd.py",
    "apps/sorsync/sor_apply_pihole.py", "apps/sorsync/sor_reconcile_discovery.py",
    "apps/notify/notifyd.py", "apps/notify/senders/__init__.py",
    "apps/alertbridge/alertbridge.py", "apps/unifi/unifipolld.py",
    "apps/discovery/avahi_import.py", "apps/discovery/mdns_inspect.py",
}


def sha256(path: Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            h.update(chunk)
    return h.hexdigest()


def validate_version(value: str) -> str:
    if not VERSION_RE.fullmatch(value) or ".." in value:
        raise ValueError("invalid release version")
    return value


def load_archive(bundle: Path, destination: Path, *, allow_dirty: bool = False) -> tuple[str, dict]:
    with tarfile.open(bundle, "r:gz") as archive:
        seen: set[str] = set()
        total = 0
        for member in archive.getmembers():
            path = PurePosixPath(member.name)
            if path.is_absolute() or not path.parts or any(p in ("", ".", "..") for p in path.parts):
                raise ValueError(f"unsafe archive path: {member.name!r}")
            name = path.as_posix()
            if any(part in {".git", ".gitignore", ".pytest_cache", "__pycache__"} for part in path.parts):
                raise ValueError(f"repository or cache metadata is not permitted: {name}")
            if ".venv" in path.parts:
                raise ValueError(f"generated Python environments are not permitted in the archive: {name}")
            if not (name in {"RELEASE.json", "bin", "bin/solariServer", "bin/solariScanWorker",
                            "bin/solariClient", "bin/solariMonitor", "bin/solariSnmpPoll",
                            "dashboard", "dashboard/api", "dashboard/public", "apps"}
                    or name.startswith("dashboard/api/") or name.startswith("dashboard/public/")
                    or name.startswith("apps/")):
                raise ValueError(f"path is outside the release allowlist: {name}")
            lower_name = path.name.lower()
            if lower_name.startswith(".env") or lower_name == "solari-auth.json" or path.suffix.lower() in {".key", ".pem"}:
                raise ValueError(f"secret-like path is not permitted: {name}")
            if name in seen:
                raise ValueError(f"duplicate archive path: {name}")
            seen.add(name)
            if member.issym() or member.islnk() or not (member.isfile() or member.isdir()):
                raise ValueError(f"unsupported archive entry: {name}")
            if member.isfile():
                total += member.size
                if member.size < 0 or member.size > MAX_FILE_BYTES or total > 1024 * 1024 * 1024:
                    raise ValueError("archive exceeds size limits")
                target = destination.joinpath(*path.parts)
                target.parent.mkdir(parents=True, exist_ok=True)
                source = archive.extractfile(member)
                if source is None:
                    raise ValueError(f"cannot read archive entry: {name}")
                with target.open("xb") as output:
                    shutil.copyfileobj(source, output)
                target.chmod(0o755 if member.mode & 0o111 else 0o644)
            else:
                destination.joinpath(*path.parts).mkdir(parents=True, exist_ok=True)

    for directory in sorted((p for p in destination.rglob("*") if p.is_dir()), reverse=True):
        directory.chmod(0o755)
    destination.chmod(0o755)

    metadata_path = destination / "RELEASE.json"
    try:
        metadata = json.loads(metadata_path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        raise ValueError("release metadata is missing or invalid") from exc
    if metadata.get("format") != 1:
        raise ValueError("unsupported release format")
    source_dirty = metadata.get("sourceTreeDirty")
    if not isinstance(source_dirty, bool):
        raise ValueError("release source-tree status is missing or invalid")
    if source_dirty and not allow_dirty:
        raise ValueError("source-dirty preflight release requires --allow-dirty")
    version = validate_version(str(metadata.get("version", "")))
    declared = metadata.get("files")
    if not isinstance(declared, dict) or not declared:
        raise ValueError("release file manifest is missing")
    actual = {
        path.relative_to(destination).as_posix()
        for path in destination.rglob("*")
        if path.is_file() and path.name != "RELEASE.json"
    }
    if actual != set(declared):
        raise ValueError("release tree does not match its file manifest")
    for name, expected in declared.items():
        path = PurePosixPath(name)
        if path.is_absolute() or any(p in ("", ".", "..") for p in path.parts):
            raise ValueError(f"unsafe manifest path: {name!r}")
        if not isinstance(expected, str) or sha256(destination.joinpath(*path.parts)) != expected:
            raise ValueError(f"release hash mismatch: {name}")
    required = {"bin/solariServer", "bin/solariScanWorker",
                "dashboard/api/index.php", "dashboard/public/index.html",
                "dashboard/public/rackwire/index.html"}
    new_runtime_present = bool(set(actual) & (set(PYTHON_RUNTIME_FILES) | {
        "bin/solariClient", "bin/solariMonitor", "bin/solariSnmpPoll",
    }))
    if new_runtime_present:
        required.update({"bin/solariClient", "bin/solariMonitor", "bin/solariSnmpPoll"})
        required.update(PYTHON_RUNTIME_FILES)
        required.update({
            "apps/notify/senders/apple.py", "apps/notify/senders/log.py",
            "apps/notify/senders/mail.py", "apps/notify/senders/mqtt.py",
            "apps/notify/senders/push.py", "apps/notify/senders/sms_tachyon.py",
            "apps/notify/senders/webpush.py",
        })
    if not required.issubset(actual):
        raise ValueError("release is missing a required runtime entry")
    return version, metadata


def ensure_python_venv(release: Path) -> None:
    """Create a host-native release-local environment from the bundled pins."""
    requirements = release / "apps" / "requirements.txt"
    if not requirements.is_file():
        return
    venv = release / ".venv"
    try:
        subprocess.run([sys.executable, "-m", "venv", str(venv)], check=True,
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        subprocess.run([str(venv / "bin/python3"), "-m", "pip", "install", "--no-input",
                        "--disable-pip-version-check", "-r", str(requirements)], check=True,
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    except (OSError, subprocess.CalledProcessError) as exc:
        shutil.rmtree(venv, ignore_errors=True)
        raise ValueError("could not create the release Python environment") from exc


def validate_installed(release: Path, *, allow_dirty: bool = False) -> str:
    try:
        metadata = json.loads((release / "RELEASE.json").read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        raise ValueError("installed release metadata is missing or invalid") from exc
    if metadata.get("format") != 1:
        raise ValueError("unsupported installed release format")
    source_dirty = metadata.get("sourceTreeDirty")
    if not isinstance(source_dirty, bool):
        raise ValueError("installed release source-tree status is missing or invalid")
    if source_dirty and not allow_dirty:
        raise ValueError("source-dirty preflight release requires --allow-dirty")
    version = validate_version(str(metadata.get("version", "")))
    declared = metadata.get("files")
    if not isinstance(declared, dict) or not declared:
        raise ValueError("installed release manifest is missing")
    actual = {
        path.relative_to(release).as_posix()
        for path in release.rglob("*")
        if path.is_file() and path.name != "RELEASE.json"
        and ".venv" not in path.relative_to(release).parts
    }
    if actual != set(declared):
        raise ValueError("installed release tree does not match its manifest")
    for name, expected in declared.items():
        path = PurePosixPath(name)
        if path.is_absolute() or any(p in ("", ".", "..") for p in path.parts):
            raise ValueError(f"unsafe installed manifest path: {name!r}")
        if not isinstance(expected, str) or sha256(release.joinpath(*path.parts)) != expected:
            raise ValueError(f"installed release hash mismatch: {name}")
    if "apps/requirements.txt" in declared and not (release / ".venv/bin/python3").is_file():
        raise ValueError("installed release Python environment is missing")
    return version


def activate(root: Path, version: str, *, allow_dirty: bool = False) -> None:
    release = root / "releases" / validate_version(version)
    if not (release / "RELEASE.json").is_file():
        raise ValueError(f"release is not installed: {version}")
    if validate_installed(release, allow_dirty=allow_dirty) != version:
        raise ValueError("installed release version does not match its directory")
    temporary = root / f".current.{os.getpid()}"
    temporary.unlink(missing_ok=True)
    temporary.symlink_to(Path("releases") / version)
    os.replace(temporary, root / "current")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, default=Path("/opt/solarinet"), help="installation root")
    choice = parser.add_mutually_exclusive_group(required=True)
    choice.add_argument("--bundle", type=Path, help="install and activate this archive")
    choice.add_argument("--activate", help="activate a previously installed version (rollback)")
    parser.add_argument("--allow-dirty", action="store_true",
                        help="allow source-dirty preflight releases; never use for production")
    args = parser.parse_args()
    root = args.root.resolve()
    try:
        (root / "releases").mkdir(parents=True, exist_ok=True)
        (root / "releases").chmod(0o755)
        if args.activate:
            activate(root, args.activate, allow_dirty=args.allow_dirty)
            print(f"active release: {args.activate}")
            return 0

        bundle = args.bundle.resolve()
        with tempfile.TemporaryDirectory(prefix=".incoming-", dir=root / "releases") as tmp:
            stage = Path(tmp)
            version, _ = load_archive(bundle, stage, allow_dirty=args.allow_dirty)
            destination = root / "releases" / version
            if destination.exists():
                raise ValueError(f"release already exists and is immutable: {version}")
            ensure_python_venv(stage)
            os.rename(stage, destination)
        activate(root, version, allow_dirty=args.allow_dirty)
        print(f"installed and activated release: {version}")
        return 0
    except (OSError, tarfile.TarError, ValueError) as exc:
        print(f"release install failed: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
