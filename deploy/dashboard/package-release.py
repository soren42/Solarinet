#!/usr/bin/env python3
"""Build a secret-free, allowlisted SolariNet dashboard release archive."""

from __future__ import annotations

import argparse
import gzip
import hashlib
import json
import os
import re
import shutil
import subprocess
import tarfile
import tempfile
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
VERSION_RE = re.compile(r"[A-Za-z0-9][A-Za-z0-9._+-]{0,63}\Z")
REQUIRED_SUBMODULES = ("dashboard/public/rackwire",)


def digest(path: Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            h.update(chunk)
    return h.hexdigest()


def source_submodules() -> dict[str, str]:
    """Return pinned dependency revisions and reject a checkout off its gitlink."""
    revisions: dict[str, str] = {}
    for relative in REQUIRED_SUBMODULES:
        path = ROOT / relative
        try:
            expected = subprocess.check_output(
                ["git", "-C", str(ROOT), "rev-parse", f"HEAD:{relative}"],
                text=True, stderr=subprocess.DEVNULL,
            ).strip()
            actual = subprocess.check_output(
                ["git", "-C", str(path), "rev-parse", "HEAD"],
                text=True, stderr=subprocess.DEVNULL,
            ).strip()
        except (OSError, subprocess.CalledProcessError) as exc:
            raise ValueError(f"required submodule is not initialized: {relative}") from exc
        if not re.fullmatch(r"[0-9a-f]{40}", expected) or actual != expected:
            raise ValueError(
                f"submodule checkout does not match parent gitlink: {relative} "
                f"(expected {expected}, got {actual})"
            )
        revisions[relative] = actual
    return revisions


def copy_tree(source: Path, destination: Path, files: dict[str, str]) -> None:
    if not source.is_dir():
        raise ValueError(f"required directory is missing: {source}")
    for path in sorted(source.rglob("*")):
        if path.is_symlink():
            raise ValueError(f"symlink is not permitted in release input: {path}")
        if not path.is_file():
            continue
        relative = path.relative_to(ROOT).as_posix()
        if any(part in {".git", ".gitignore", ".github", ".pytest_cache", "__pycache__"} for part in path.parts):
            continue
        lower_name = path.name.lower()
        if lower_name.startswith(".env") or lower_name in {"solari-auth.json"} or path.suffix.lower() in {".key", ".pem"}:
            raise ValueError(f"secret-like file is not permitted in release input: {path}")
        target = destination / relative
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(path, target)
        target.chmod(0o755 if os.access(path, os.X_OK) else 0o644)
        files[relative] = digest(target)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--version", required=True, help="immutable release directory name")
    parser.add_argument("--binary", type=Path, default=ROOT / "build-server/src/server/solariServer")
    parser.add_argument("--scan-worker-binary", type=Path,
                        default=ROOT / "build-server/src/server/solariScanWorker")
    parser.add_argument("--output", type=Path)
    parser.add_argument("--allow-dirty", action="store_true",
                        help="allow a source-dirty preflight bundle; never use for production")
    args = parser.parse_args()
    if not VERSION_RE.fullmatch(args.version) or ".." in args.version:
        parser.error("version must be a simple path-safe identifier")
    binary = args.binary.resolve()
    if not binary.is_file() or not os.access(binary, os.X_OK):
        parser.error(f"server binary is missing or not executable: {binary}")
    scan_worker = args.scan_worker_binary.resolve()
    if not scan_worker.is_file() or not os.access(scan_worker, os.X_OK):
        parser.error(f"scan worker binary is missing or not executable: {scan_worker}")

    output = (args.output or ROOT / "deploy/dist" / f"solarinet-dashboard-{args.version}.tar.gz").resolve()
    try:
        revision = subprocess.check_output(
            ["git", "-C", str(ROOT), "rev-parse", "HEAD"], text=True, stderr=subprocess.DEVNULL
        ).strip()
        dirty = bool(subprocess.check_output(
            ["git", "-C", str(ROOT), "status", "--porcelain", "--untracked-files=all"],
            text=True, stderr=subprocess.DEVNULL,
        ).strip())
    except (OSError, subprocess.CalledProcessError) as exc:
        parser.error(f"cannot establish Git source provenance: {exc}")
    if dirty and not args.allow_dirty:
        parser.error("source worktree is dirty; commit or clean it, or use --allow-dirty for preflight only")
    try:
        submodules = source_submodules()
    except ValueError as exc:
        parser.error(str(exc))
    output.parent.mkdir(parents=True, exist_ok=True)

    with tempfile.TemporaryDirectory(prefix="solarinet-release-") as tmp:
        stage = Path(tmp) / "release"
        stage.mkdir()
        files: dict[str, str] = {}
        bin_dir = stage / "bin"
        bin_dir.mkdir()
        server = bin_dir / "solariServer"
        shutil.copyfile(binary, server)
        server.chmod(0o755)
        files["bin/solariServer"] = digest(server)
        worker = bin_dir / "solariScanWorker"
        shutil.copyfile(scan_worker, worker)
        worker.chmod(0o755)
        files["bin/solariScanWorker"] = digest(worker)
        copy_tree(ROOT / "dashboard/api", stage, files)
        copy_tree(ROOT / "dashboard/public", stage, files)
        required = {
            "dashboard/api/index.php",
            "dashboard/public/index.html",
            "dashboard/public/rackwire/index.html",
        }
        missing = sorted(required - files.keys())
        if missing:
            raise ValueError(f"required dashboard runtime entries are missing: {missing}")
        unexpected = [
            name for name in files
            if not (name in {"bin/solariServer", "bin/solariScanWorker"}
                    or name.startswith("dashboard/api/") or name.startswith("dashboard/public/"))
        ]
        if unexpected:
            raise ValueError(f"unexpected release paths: {unexpected[:3]}")
        metadata = {
            "format": 1,
            "version": args.version,
            "sourceRevision": revision,
            "sourceSubmodules": submodules,
            "sourceTreeDirty": dirty,
            "files": files,
        }
        (stage / "RELEASE.json").write_text(
            json.dumps(metadata, sort_keys=True, indent=2) + "\n", encoding="utf-8"
        )
        temp_output = output.with_name(output.name + ".tmp")
        try:
            with temp_output.open("wb") as raw:
                with gzip.GzipFile(filename="", mode="wb", fileobj=raw, mtime=0) as compressed:
                    with tarfile.open(fileobj=compressed, mode="w|", format=tarfile.PAX_FORMAT) as archive:
                        for path in sorted(stage.rglob("*")):
                            archive.add(path, arcname=path.relative_to(stage).as_posix(), recursive=False)
            os.replace(temp_output, output)
        finally:
            temp_output.unlink(missing_ok=True)

    print(f"created {output}")
    print(f"version={args.version} files={len(files)} sourceRevision={revision} sourceTreeDirty={str(dirty).lower()}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
