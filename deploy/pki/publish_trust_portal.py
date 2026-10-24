#!/usr/bin/env python3
"""Publish a generated public CA trust package with an atomic release switch."""

from __future__ import annotations

import argparse
import hashlib
import os
from pathlib import Path
import re
import shutil
import stat
import sys
import tempfile
import uuid

from build_trust_portal import (
    PORTAL_FILES,
    TrustPortalError,
    artifacts_for_details,
    certificate_details_from_bytes,
)


CHECKSUM_FILES = set(PORTAL_FILES) - {"SHA256SUMS"}
FINGERPRINT_RE = re.compile(r"^(?:[0-9a-fA-F]{2}:?){32}$")


def _set_publication_owner(path: Path) -> None:
    if hasattr(os, "geteuid") and os.geteuid() == 0:
        os.chown(path, 0, 0, follow_symlinks=False)


def _require_owned_directory(path: Path, mode: int) -> None:
    info = path.stat(follow_symlinks=False)
    if not stat.S_ISDIR(info.st_mode) or info.st_uid != os.geteuid():
        raise TrustPortalError(f"publication directory must be owned by the publishing user: {path}")
    if stat.S_IMODE(info.st_mode) != mode:
        raise TrustPortalError(f"publication directory must have mode {mode:04o}: {path}")


def verify_artifacts(source: Path) -> tuple[dict[str, str | bytes], dict[str, bytes]]:
    if source.is_symlink() or not source.is_dir():
        raise TrustPortalError(f"source must be a real artifact directory: {source}")
    try:
        entries = list(source.iterdir())
    except OSError as exc:
        raise TrustPortalError(f"cannot list trust artifacts: {exc}") from exc
    if {entry.name for entry in entries} != set(PORTAL_FILES):
        raise TrustPortalError("source directory must contain exactly the generated trust artifacts")

    artifacts: dict[str, bytes] = {}
    for name in PORTAL_FILES:
        path = source / name
        if path.is_symlink() or not path.is_file():
            raise TrustPortalError(f"trust artifact must be a regular file: {name}")
        try:
            artifacts[name] = path.read_bytes()
        except OSError as exc:
            raise TrustPortalError(f"cannot read trust artifact {name}: {exc}") from exc

    lines = artifacts["SHA256SUMS"].decode("ascii", "strict").splitlines()
    checksums: dict[str, str] = {}
    for line in lines:
        match = re.fullmatch(r"([0-9a-f]{64})  ([A-Za-z0-9._-]+)", line)
        if not match or match.group(2) in checksums:
            raise TrustPortalError("SHA256SUMS contains a malformed or duplicate entry")
        checksums[match.group(2)] = match.group(1)
    if set(checksums) != CHECKSUM_FILES:
        raise TrustPortalError("SHA256SUMS does not cover every public trust artifact exactly once")
    for name in CHECKSUM_FILES:
        if hashlib.sha256(artifacts[name]).hexdigest() != checksums[name]:
            raise TrustPortalError(f"SHA256SUMS verification failed for {name}")

    details = certificate_details_from_bytes(artifacts["root_ca.pem"])
    generated = artifacts_for_details(details)
    for name, expected in generated.items():
        if artifacts[name] != expected:
            raise TrustPortalError(f"trust artifact does not match root certificate: {name}")
    return details, artifacts


def _validate_release(path: Path, expected: dict[str, bytes]) -> None:
    if path.is_symlink() or not path.is_dir():
        raise TrustPortalError(f"release path is not a real directory: {path}")
    try:
        entries = list(path.iterdir())
        if {entry.name for entry in entries} != set(PORTAL_FILES):
            raise TrustPortalError(f"existing release has an unexpected file set: {path}")
        for name, expected_bytes in expected.items():
            entry = path / name
            if entry.is_symlink() or not entry.is_file() or entry.read_bytes() != expected_bytes:
                raise TrustPortalError(f"existing immutable release differs at {name}")
    except OSError as exc:
        raise TrustPortalError(f"cannot verify existing release: {exc}") from exc


def _verified_retained_release(releases: Path, fingerprint: str, expected: str) -> tuple[Path, dict[str, str]]:
    if not re.fullmatch(r"[0-9a-f]{64}", fingerprint):
        raise TrustPortalError("retained release name must be a lowercase SHA-256 fingerprint")
    release = releases / fingerprint
    if release.is_symlink() or not release.is_dir():
        raise TrustPortalError(f"retained release is not a real directory: {release}")
    try:
        _require_owned_directory(release, 0o755)
        entries = list(release.iterdir())
        if {entry.name for entry in entries} != set(PORTAL_FILES):
            raise TrustPortalError("retained release has an unexpected file set")
        artifacts: dict[str, bytes] = {}
        for name in PORTAL_FILES:
            entry = release / name
            info = entry.stat(follow_symlinks=False)
            if not stat.S_ISREG(info.st_mode) or info.st_uid != os.geteuid() or stat.S_IMODE(info.st_mode) != 0o644:
                raise TrustPortalError(f"retained release artifact has unexpected owner or mode: {entry}")
            artifacts[name] = entry.read_bytes()
        lines = artifacts["SHA256SUMS"].decode("ascii", "strict").splitlines()
        checksums: dict[str, str] = {}
        for line in lines:
            match = re.fullmatch(r"([0-9a-f]{64})  ([A-Za-z0-9._-]+)", line)
            if not match or match.group(2) in checksums:
                raise TrustPortalError("retained release has malformed checksums")
            checksums[match.group(2)] = match.group(1)
        if set(checksums) != CHECKSUM_FILES:
            raise TrustPortalError("retained release checksums do not cover the exact artifact set")
        for name in CHECKSUM_FILES:
            if hashlib.sha256(artifacts[name]).hexdigest() != checksums[name]:
                raise TrustPortalError(f"retained release checksum verification failed for {name}")
        details = certificate_details_from_bytes(artifacts["root_ca.pem"])
        actual = str(details["fingerprint_hex"]).lower()
        if actual != fingerprint or actual != expected:
            raise TrustPortalError("retained release certificate does not match its name and independent pin")
        return release, details
    except OSError as exc:
        raise TrustPortalError(f"cannot verify retained release: {exc}") from exc


def _current_release(root: Path, releases: Path) -> Path | None:
    current = root / "current"
    if not current.exists() and not current.is_symlink():
        return None
    if not current.is_symlink():
        raise TrustPortalError(f"refusing to replace non-symlink current path: {current}")
    if not current.exists():
        raise TrustPortalError(
            f"current symlink target is missing; inspect and repair or remove {current} before publishing"
        )
    try:
        target = Path(os.readlink(current))
        if target.is_absolute() or len(target.parts) != 2 or target.parts[0] != "releases":
            raise TrustPortalError("current must point to a release directory below releases/")
        resolved_releases = releases.resolve(strict=True)
        resolved_target = (root / target).resolve(strict=True)
        if resolved_target.parent != resolved_releases or not resolved_target.is_dir():
            raise TrustPortalError("current target is outside the immutable releases directory")
        return resolved_target
    except OSError as exc:
        raise TrustPortalError(
            f"cannot validate current release; inspect and repair or remove {current}: {exc}"
        ) from exc


def publish_trust_portal(source: Path, root: Path, expected_sha256: str) -> dict[str, str]:
    details, artifacts = verify_artifacts(source)
    fingerprint = str(details["fingerprint_hex"]).lower()
    expected = expected_sha256.replace(":", "").lower()
    if not FINGERPRINT_RE.fullmatch(expected_sha256) or fingerprint != expected:
        raise TrustPortalError(
            "root certificate SHA-256 does not match the independently verified --expect-sha256 value"
        )
    root = root.absolute()
    if root.name != "trust" or root == Path("/"):
        raise TrustPortalError("publication root must be a dedicated directory named trust")
    if root.parent.is_symlink() or not root.parent.is_dir():
        raise TrustPortalError(f"publication parent must be an existing real directory: {root.parent}")
    if root.is_symlink() or (root.exists() and not root.is_dir()):
        raise TrustPortalError(f"publication root must be a real directory: {root}")
    try:
        if not root.exists():
            root.mkdir(mode=0o755)
            root.chmod(0o755)
        _require_owned_directory(root, 0o755)
        releases = root / "releases"
        if releases.is_symlink() or (releases.exists() and not releases.is_dir()):
            raise TrustPortalError(f"releases path must be a real directory: {releases}")
        if not releases.exists():
            releases.mkdir(mode=0o755)
            releases.chmod(0o755)
        _require_owned_directory(releases, 0o755)
    except OSError as exc:
        raise TrustPortalError(f"cannot prepare publication root: {exc}") from exc

    _current_release(root, releases)
    release = releases / fingerprint
    if release.exists() or release.is_symlink():
        _validate_release(release, artifacts)
        try:
            _require_owned_directory(release, 0o755)
            for name in PORTAL_FILES:
                artifact = release / name
                info = artifact.stat(follow_symlinks=False)
                if info.st_uid != os.geteuid() or stat.S_IMODE(info.st_mode) != 0o644:
                    raise TrustPortalError(f"existing release artifact has unexpected owner or mode: {artifact}")
        except OSError as exc:
            raise TrustPortalError(f"cannot secure existing trust release ownership: {exc}") from exc
    else:
        try:
            staging = Path(tempfile.mkdtemp(prefix=".stage-", dir=releases))
        except OSError as exc:
            raise TrustPortalError(f"cannot prepare release staging directory: {exc}") from exc
        try:
            for name, payload in artifacts.items():
                artifact = staging / name
                artifact.write_bytes(payload)
                _set_publication_owner(artifact)
                artifact.chmod(0o644)
            _set_publication_owner(staging)
            staging.chmod(0o755)
            os.rename(staging, release)
        except OSError as exc:
            raise TrustPortalError(f"cannot publish immutable trust release: {exc}") from exc
        finally:
            if staging.exists():
                shutil.rmtree(staging, ignore_errors=True)

    link = root / f".current-{uuid.uuid4().hex}"
    current = root / "current"
    try:
        link.symlink_to(Path("releases") / fingerprint)
        _set_publication_owner(link)
        os.replace(link, current)
    except OSError as exc:
        raise TrustPortalError(f"cannot atomically activate trust release: {exc}") from exc
    finally:
        link.unlink(missing_ok=True)

    return {
        "fingerprint": str(details["fingerprint_colon"]),
        "release": str(release),
        "current": str(current),
    }


def activate_retained_release(fingerprint: str, root: Path, expected_sha256: str) -> dict[str, str]:
    expected = expected_sha256.replace(":", "").lower()
    if not FINGERPRINT_RE.fullmatch(expected_sha256):
        raise TrustPortalError("invalid independently verified --expect-sha256 fingerprint")
    root = root.absolute()
    if root.name != "trust" or root == Path("/") or root.is_symlink() or not root.is_dir():
        raise TrustPortalError("publication root must be an existing real directory named trust")
    _require_owned_directory(root, 0o755)
    releases = root / "releases"
    if releases.is_symlink() or not releases.is_dir():
        raise TrustPortalError("releases path must be a real directory")
    _require_owned_directory(releases, 0o755)
    release, details = _verified_retained_release(releases, fingerprint, expected)
    _current_release(root, releases)
    link = root / f".current-{uuid.uuid4().hex}"
    current = root / "current"
    try:
        link.symlink_to(Path("releases") / fingerprint)
        _set_publication_owner(link)
        os.replace(link, current)
    except OSError as exc:
        raise TrustPortalError(f"cannot atomically activate retained trust release: {exc}") from exc
    finally:
        link.unlink(missing_ok=True)
    return {"fingerprint": str(details["fingerprint_colon"]), "release": str(release), "current": str(current)}


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    mode = parser.add_mutually_exclusive_group(required=True)
    mode.add_argument("--source", type=Path, help="verified output directory from build_trust_portal.py")
    mode.add_argument("--activate-release", help="activate a retained immutable release by lowercase SHA-256 fingerprint")
    parser.add_argument("--root", required=True, type=Path, help="root-owned publication directory containing releases/ and current")
    parser.add_argument("--expect-sha256", required=True,
                        help="independently verified root certificate SHA-256 fingerprint (with or without colons)")
    args = parser.parse_args(argv)
    try:
        if args.source is not None:
            result = publish_trust_portal(args.source, args.root, args.expect_sha256)
        else:
            result = activate_retained_release(args.activate_release, args.root, args.expect_sha256)
    except (TrustPortalError, UnicodeError) as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 2
    print(f"Published trust release: {result['release']}")
    print(f"Current: {result['current']}")
    print(f"SHA-256: {result['fingerprint']}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
