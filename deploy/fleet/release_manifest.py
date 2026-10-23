#!/usr/bin/env python3
"""Create and verify read-only, CMS-signed SolariNet fleet release trees.

This tool stages artifacts locally. It does not publish, activate, or deploy a
release. The verification root is supplied by the authenticated boot/installer
path; downloading a root beside a release does not establish trust.
"""
from __future__ import annotations

import argparse
import datetime as dt
import hashlib
import json
import os
import pathlib
import re
import shutil
import stat
import subprocess
import sys
import tempfile
from typing import Any


FORMAT = "solarinet-fleet-release-v1"
RELEASE_ID_RE = re.compile(r"^[A-Za-z0-9][A-Za-z0-9._-]{0,63}$")
SHA256_RE = re.compile(r"^[0-9a-f]{64}$")
SUPPORTED_TARGETS = {
    ("debian", "x86_64"), ("debian", "arm64"),
    ("ubuntu", "x86_64"), ("ubuntu", "arm64"),
    ("opensuse", "x86_64"), ("opensuse", "arm64"),
    ("raspios", "arm64"), ("raspios", "arm32"),
}
MANIFEST_NAME = "manifest.json"
SIGNATURE_SUFFIX = ".sig"


class ReleaseError(Exception):
    """Expected invalid-input, filesystem, or signature failure."""


def canonicalJson(value: Any) -> bytes:
    return (json.dumps(value, sort_keys=True, separators=(",", ":"),
                        ensure_ascii=True, allow_nan=False) + "\n").encode("ascii")


def rejectDuplicateKeys(pairs: list[tuple[str, Any]]) -> dict[str, Any]:
    result: dict[str, Any] = {}
    for key, value in pairs:
        if key in result:
            raise ReleaseError(f"duplicate JSON key: {key}")
        result[key] = value
    return result


def parseUtc(value: Any, label: str) -> dt.datetime:
    if not isinstance(value, str) or not value.endswith("Z"):
        raise ReleaseError(f"{label} must be an RFC3339 UTC timestamp ending in Z")
    try:
        parsed = dt.datetime.fromisoformat(value[:-1] + "+00:00")
    except ValueError as error:
        raise ReleaseError(f"invalid {label}") from error
    if parsed.utcoffset() != dt.timedelta(0):
        raise ReleaseError(f"{label} must use UTC")
    return parsed


def validateTarget(value: str) -> dict[str, str]:
    if "/" not in value:
        raise ReleaseError("target must be OS/architecture")
    osName, arch = value.split("/", 1)
    if (osName, arch) not in SUPPORTED_TARGETS:
        raise ReleaseError(f"unsupported target: {value}")
    return {"os": osName, "arch": arch}


def validateManifest(manifest: Any) -> dict[str, Any]:
    if not isinstance(manifest, dict) or set(manifest) != {
        "format", "release_id", "not_before", "expires_at", "targets",
        "artifacts", "signer_sha256"
    }:
        raise ReleaseError("manifest has missing or unknown fields")
    if manifest["format"] != FORMAT:
        raise ReleaseError("unsupported manifest format")
    releaseId = manifest["release_id"]
    if not isinstance(releaseId, str) or not RELEASE_ID_RE.fullmatch(releaseId):
        raise ReleaseError("invalid release_id")
    if (not isinstance(manifest["signer_sha256"], str) or
            not SHA256_RE.fullmatch(manifest["signer_sha256"])):
        raise ReleaseError("invalid signer_sha256")
    notBefore = parseUtc(manifest["not_before"], "not_before")
    expiresAt = parseUtc(manifest["expires_at"], "expires_at")
    if notBefore >= expiresAt:
        raise ReleaseError("not_before must precede expires_at")

    targets = manifest["targets"]
    if not isinstance(targets, list) or not targets:
        raise ReleaseError("targets must be a non-empty array")
    targetKeys: list[tuple[str, str]] = []
    for target in targets:
        if not isinstance(target, dict) or set(target) != {"os", "arch"}:
            raise ReleaseError("target entries must contain only os and arch")
        if not isinstance(target["os"], str) or not isinstance(target["arch"], str):
            raise ReleaseError("target OS and architecture must be strings")
        key = (target["os"], target["arch"])
        if key not in SUPPORTED_TARGETS:
            raise ReleaseError(f"unsupported target: {key[0]}/{key[1]}")
        targetKeys.append(key)
    if len(set(targetKeys)) != len(targetKeys):
        raise ReleaseError("duplicate target")
    if targetKeys != sorted(targetKeys):
        raise ReleaseError("targets must be sorted canonically")

    artifacts = manifest["artifacts"]
    if not isinstance(artifacts, list) or not artifacts:
        raise ReleaseError("artifacts must be a non-empty array")
    artifactPaths: list[str] = []
    for item in artifacts:
        if not isinstance(item, dict) or set(item) != {"path", "size", "mode", "sha256"}:
            raise ReleaseError("artifact entries must contain only path, size, mode, sha256")
        path = item["path"]
        if not isinstance(path, str):
            raise ReleaseError("artifact path must be a string")
        pure = pathlib.PurePosixPath(path)
        if (not path or path.startswith("/") or "\\" in path or
                "\x00" in path or
                pure.as_posix() != path or any(part in ("", ".", "..") for part in path.split("/")) or
                path in {MANIFEST_NAME, MANIFEST_NAME + SIGNATURE_SUFFIX} or
                path.endswith(SIGNATURE_SUFFIX)):
            raise ReleaseError(f"unsafe or reserved artifact path: {path!r}")
        size = item["size"]
        if isinstance(size, bool) or not isinstance(size, int) or size < 0:
            raise ReleaseError(f"invalid artifact size: {path}")
        mode = item["mode"]
        if isinstance(mode, bool) or not isinstance(mode, int) or mode not in (0o444, 0o555):
            raise ReleaseError(f"invalid immutable artifact mode: {path}")
        if not isinstance(item["sha256"], str) or not SHA256_RE.fullmatch(item["sha256"]):
            raise ReleaseError(f"invalid artifact SHA-256: {path}")
        artifactPaths.append(path)
    if len(set(artifactPaths)) != len(artifactPaths):
        raise ReleaseError("duplicate artifact path")
    if artifactPaths != sorted(artifactPaths):
        raise ReleaseError("artifacts must be sorted canonically")
    return manifest


def runOpenSsl(args: list[str]) -> None:
    try:
        result = subprocess.run(args, check=False, capture_output=True, text=True)
    except OSError as error:
        raise ReleaseError("OpenSSL could not be executed") from error
    if result.returncode != 0:
        # OpenSSL diagnostics can contain local paths. Keep the message bounded.
        detail = (result.stderr or result.stdout).strip().splitlines()
        reason = detail[-1][:240] if detail else "verification command failed"
        raise ReleaseError("OpenSSL operation failed: " + reason)


def signCms(content: pathlib.Path, signature: pathlib.Path, signerCert: pathlib.Path,
            signingKey: pathlib.Path, caCert: pathlib.Path) -> None:
    runOpenSsl(["openssl", "cms", "-sign", "-binary", "-noattr",
                "-in", str(content), "-signer", str(signerCert),
                "-inkey", str(signingKey), "-certfile", str(caCert),
                "-outform", "DER", "-out", str(signature)])


def certificateFingerprint(certificate: pathlib.Path) -> str:
    try:
        result = subprocess.run(["openssl", "x509", "-in", str(certificate),
                                 "-noout", "-fingerprint", "-sha256"],
                                check=False, capture_output=True, text=True)
    except OSError as error:
        raise ReleaseError("OpenSSL could not read the signer certificate") from error
    match = re.search(r"=([0-9A-Fa-f:]{95})\s*$", result.stdout)
    if result.returncode != 0 or not match:
        raise ReleaseError("cannot determine signer certificate fingerprint")
    return match.group(1).replace(":", "").lower()


def verifyCms(content: pathlib.Path, signature: pathlib.Path,
              trustRoot: pathlib.Path, expectedSigner: str,
              workDir: pathlib.Path) -> None:
    try:
        if signature.stat().st_size > 2 * 1024 * 1024:
            raise ReleaseError("CMS signature exceeds the 2 MiB limit")
        if trustRoot.stat().st_size > 2 * 1024 * 1024:
            raise ReleaseError("trust root exceeds the 2 MiB limit")
    except OSError as error:
        raise ReleaseError("CMS signature or trust root is missing") from error
    signer = workDir / "cms-signer.pem"
    embedded = workDir / "cms-certs.pem"
    # First prove the detached CMS signature over these exact bytes. Chain
    # purpose is checked separately because CMS defaults to S/MIME signing.
    runOpenSsl(["openssl", "cms", "-verify", "-binary", "-inform", "DER",
                "-in", str(signature), "-content", str(content), "-noverify",
                "-signer", str(signer), "-certsout", str(embedded),
                "-out", os.devnull])
    runOpenSsl(["openssl", "verify", "-CAfile", str(trustRoot),
                "-purpose", "codesign", "-untrusted", str(embedded), str(signer)])
    if certificateFingerprint(signer) != expectedSigner:
        raise ReleaseError("CMS signer certificate does not match the pinned signer")


def checkedTreeFiles(root: pathlib.Path, requireImmutable: bool = False) -> set[str]:
    if root.is_symlink() or not root.is_dir():
        raise ReleaseError("release root must be a real directory")
    if requireImmutable and stat.S_IMODE(root.stat().st_mode) != 0o555:
        raise ReleaseError("release root must have immutable mode 0555")
    found: set[str] = set()
    for current, dirs, files in os.walk(root, followlinks=False):
        currentPath = pathlib.Path(current)
        for name in list(dirs):
            path = currentPath / name
            if path.is_symlink():
                raise ReleaseError(f"symlink directory is forbidden: {path.relative_to(root)}")
            if requireImmutable and stat.S_IMODE(path.stat().st_mode) != 0o555:
                raise ReleaseError(f"release directory mode mismatch: {path.relative_to(root)}")
        for name in files:
            path = currentPath / name
            if path.is_symlink():
                raise ReleaseError(f"symlink file is forbidden: {path.relative_to(root)}")
            try:
                mode = path.lstat().st_mode
            except OSError as error:
                raise ReleaseError("cannot inspect release tree") from error
            if not stat.S_ISREG(mode):
                raise ReleaseError(f"non-regular release file is forbidden: {path.relative_to(root)}")
            if requireImmutable and (mode & 0o222):
                raise ReleaseError(f"writable release file: {path.relative_to(root)}")
            found.add(path.relative_to(root).as_posix())
    return found


def safeArtifact(root: pathlib.Path, relative: str) -> pathlib.Path:
    path = root.joinpath(*pathlib.PurePosixPath(relative).parts)
    try:
        mode = path.lstat().st_mode
    except OSError as error:
        raise ReleaseError(f"artifact is missing: {relative}") from error
    if not stat.S_ISREG(mode):
        raise ReleaseError(f"artifact is not a regular file: {relative}")
    # Check every path component so an intermediate symlink cannot escape root.
    cursor = root
    for part in pathlib.PurePosixPath(relative).parts:
        cursor = cursor / part
        if cursor.is_symlink():
            raise ReleaseError(f"symlink path is forbidden: {relative}")
    return path


def digestFile(path: pathlib.Path) -> tuple[int, str]:
    digest = hashlib.sha256()
    size = 0
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            size += len(block)
            digest.update(block)
    return size, digest.hexdigest()


def buildManifest(source: pathlib.Path, releaseId: str, notBefore: str,
                  expiresAt: str, targets: list[str],
                  signerCert: pathlib.Path) -> dict[str, Any]:
    if not RELEASE_ID_RE.fullmatch(releaseId):
        raise ReleaseError("invalid release_id")
    targetItems = [validateTarget(target) for target in targets]
    targetItems.sort(key=lambda item: (item["os"], item["arch"]))
    artifactPaths = sorted(checkedTreeFiles(source))
    if not artifactPaths:
        raise ReleaseError("source tree contains no artifacts")
    artifacts = []
    for relative in artifactPaths:
        if relative in {MANIFEST_NAME, MANIFEST_NAME + SIGNATURE_SUFFIX} or relative.endswith(SIGNATURE_SUFFIX):
            raise ReleaseError(f"reserved signature/manifest file in source tree: {relative}")
        sourceFile = safeArtifact(source, relative)
        size, digest = digestFile(sourceFile)
        mode = 0o555 if sourceFile.stat().st_mode & 0o111 else 0o444
        artifacts.append({"path": relative, "size": size, "mode": mode,
                          "sha256": digest})
    manifest = {"format": FORMAT, "release_id": releaseId,
                "not_before": notBefore, "expires_at": expiresAt,
                "targets": targetItems, "artifacts": artifacts,
                "signer_sha256": certificateFingerprint(signerCert)}
    return validateManifest(manifest)


def createRelease(source: pathlib.Path, output: pathlib.Path, manifest: dict[str, Any],
                  signerCert: pathlib.Path, signingKey: pathlib.Path,
                  caCert: pathlib.Path) -> None:
    if source.is_symlink() or not source.is_dir():
        raise ReleaseError("source must be a real directory")
    source = source.resolve(strict=True)
    output = output.resolve(strict=False)
    if output == source or source in output.parents:
        raise ReleaseError("output release directory must be outside the source tree")
    if certificateFingerprint(signerCert) != manifest["signer_sha256"]:
        raise ReleaseError("manifest signer fingerprint does not match signing certificate")
    if output.exists() or output.is_symlink():
        raise ReleaseError("output release directory already exists")
    output.parent.mkdir(parents=True, exist_ok=True)
    tempRoot = pathlib.Path(tempfile.mkdtemp(prefix=".release-stage-", dir=output.parent))
    try:
        for item in manifest["artifacts"]:
            relative = item["path"]
            sourceFile = safeArtifact(source, relative)
            dest = tempRoot.joinpath(*pathlib.PurePosixPath(relative).parts)
            dest.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(sourceFile, dest, follow_symlinks=False)
            size, digest = digestFile(dest)
            if size != item["size"] or digest != item["sha256"]:
                raise ReleaseError(f"source changed while staging: {relative}")
            dest.chmod(item["mode"])
            signCms(dest, pathlib.Path(str(dest) + SIGNATURE_SUFFIX),
                    signerCert, signingKey, caCert)
        manifestPath = tempRoot / MANIFEST_NAME
        manifestPath.write_bytes(canonicalJson(manifest))
        signCms(manifestPath, pathlib.Path(str(manifestPath) + SIGNATURE_SUFFIX),
                signerCert, signingKey, caCert)
        artifactModes = {item["path"]: item["mode"] for item in manifest["artifacts"]}
        for current, dirs, files in os.walk(tempRoot, topdown=False):
            currentPath = pathlib.Path(current)
            for name in files:
                path = currentPath / name
                relative = path.relative_to(tempRoot).as_posix()
                path.chmod(artifactModes.get(relative, 0o444))
                with path.open("rb") as stream:
                    os.fsync(stream.fileno())
            for name in dirs:
                path = currentPath / name
                path.chmod(0o555)
                dirFd = os.open(path, os.O_RDONLY | getattr(os, "O_DIRECTORY", 0))
                try:
                    os.fsync(dirFd)
                finally:
                    os.close(dirFd)
        tempRoot.chmod(0o555)
        dirFd = os.open(tempRoot, os.O_RDONLY | getattr(os, "O_DIRECTORY", 0))
        try:
            os.fsync(dirFd)
        finally:
            os.close(dirFd)
        os.rename(tempRoot, output)
        parentFd = os.open(output.parent, os.O_RDONLY | getattr(os, "O_DIRECTORY", 0))
        try:
            os.fsync(parentFd)
        finally:
            os.close(parentFd)
    except Exception:
        # Restore directory permissions so temporary output can be removed.
        if tempRoot.exists():
            for current, dirs, _files in os.walk(tempRoot):
                pathlib.Path(current).chmod(0o755)
                for name in dirs:
                    (pathlib.Path(current) / name).chmod(0o755)
            shutil.rmtree(tempRoot, ignore_errors=True)
        raise


def loadCanonicalManifest(path: pathlib.Path) -> dict[str, Any]:
    try:
        if path.stat().st_size > 8 * 1024 * 1024:
            raise ReleaseError("manifest exceeds the 8 MiB limit")
        data = path.read_bytes()
        value = json.loads(data, object_pairs_hook=rejectDuplicateKeys,
                           parse_constant=lambda _v: (_ for _ in ()).throw(ReleaseError("invalid JSON number")))
    except (OSError, UnicodeError, json.JSONDecodeError) as error:
        raise ReleaseError("manifest is unreadable or invalid JSON") from error
    manifest = validateManifest(value)
    if canonicalJson(manifest) != data:
        raise ReleaseError("manifest is not canonical JSON")
    return manifest


def verifyRelease(root: pathlib.Path, expectedReleaseId: str, targetOs: str,
                  targetArch: str, trustRoot: pathlib.Path,
                  expectedSignerFingerprint: str) -> dict[str, Any]:
    root = root.absolute()
    if (not isinstance(expectedSignerFingerprint, str) or
            not SHA256_RE.fullmatch(expectedSignerFingerprint)):
        raise ReleaseError("expected signer fingerprint must be 64 lowercase hex characters")
    files = checkedTreeFiles(root, requireImmutable=True)
    manifestPath = safeArtifact(root, MANIFEST_NAME)
    signaturePath = safeArtifact(root, MANIFEST_NAME + SIGNATURE_SUFFIX)
    if manifestPath.stat().st_size > 8 * 1024 * 1024:
        raise ReleaseError("manifest exceeds the 8 MiB limit")
    with tempfile.TemporaryDirectory(prefix="solarinet-cms-verify-") as temp:
        verifyCms(manifestPath, signaturePath, trustRoot,
                  expectedSignerFingerprint, pathlib.Path(temp))
        manifest = loadCanonicalManifest(manifestPath)
        if manifest["signer_sha256"] != expectedSignerFingerprint:
            raise ReleaseError("manifest signer does not match the pinned signer")
        if manifest["release_id"] != expectedReleaseId:
            raise ReleaseError("release_id does not match the requested release")
        now = dt.datetime.now(dt.timezone.utc)
        if now < parseUtc(manifest["not_before"], "not_before"):
            raise ReleaseError("release is not yet valid")
        if now >= parseUtc(manifest["expires_at"], "expires_at"):
            raise ReleaseError("release has expired")
        target = (targetOs, targetArch)
        if target not in {(item["os"], item["arch"]) for item in manifest["targets"]}:
            raise ReleaseError("release does not support the requested OS/architecture")

        expectedFiles = {MANIFEST_NAME, MANIFEST_NAME + SIGNATURE_SUFFIX}
        for item in manifest["artifacts"]:
            relative = item["path"]
            path = safeArtifact(root, relative)
            signature = safeArtifact(root, relative + SIGNATURE_SUFFIX)
            expectedFiles.update((relative, relative + SIGNATURE_SUFFIX))
            size, digest = digestFile(path)
            if size != item["size"]:
                raise ReleaseError(f"artifact length mismatch: {relative}")
            if digest != item["sha256"]:
                raise ReleaseError(f"artifact digest mismatch: {relative}")
            if stat.S_IMODE(path.stat().st_mode) != item["mode"]:
                raise ReleaseError(f"artifact mode mismatch: {relative}")
            verifyCms(path, signature, trustRoot,
                      expectedSignerFingerprint, pathlib.Path(temp))
        if files != expectedFiles:
            extras = sorted(files - expectedFiles)
            missing = sorted(expectedFiles - files)
            raise ReleaseError(f"release file set mismatch (extra={extras}, missing={missing})")
    return manifest


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest="command", required=True)
    create = sub.add_parser("create", help="stage and sign a new read-only release tree")
    create.add_argument("--source", type=pathlib.Path, required=True)
    create.add_argument("--output", type=pathlib.Path, required=True)
    create.add_argument("--release-id", required=True)
    create.add_argument("--not-before", required=True)
    create.add_argument("--expires-at", required=True)
    create.add_argument("--target", action="append", required=True, help="OS/architecture")
    create.add_argument("--signer-cert", type=pathlib.Path, required=True)
    create.add_argument("--signing-key", type=pathlib.Path, required=True)
    create.add_argument("--ca-cert", type=pathlib.Path, required=True)
    verify = sub.add_parser("verify", help="verify signature, target, time, path and digest")
    verify.add_argument("--release-root", type=pathlib.Path, required=True)
    verify.add_argument("--expected-release-id", required=True)
    verify.add_argument("--os", dest="target_os", required=True)
    verify.add_argument("--arch", dest="target_arch", required=True)
    verify.add_argument("--trust-root", type=pathlib.Path, required=True)
    verify.add_argument("--expected-signer-sha256", required=True,
                        help="pinned signer certificate fingerprint (lowercase hex)")
    args = parser.parse_args()
    try:
        if args.command == "create":
            manifest = buildManifest(args.source, args.release_id,
                                     args.not_before, args.expires_at, args.target,
                                     args.signer_cert)
            createRelease(args.source, args.output, manifest, args.signer_cert,
                          args.signing_key, args.ca_cert)
            print(f"staged signed release {args.release_id} at {args.output}")
        else:
            if (args.target_os, args.target_arch) not in SUPPORTED_TARGETS:
                raise ReleaseError("unsupported requested OS/architecture")
            manifest = verifyRelease(args.release_root, args.expected_release_id,
                                     args.target_os, args.target_arch, args.trust_root,
                                     args.expected_signer_sha256)
            print(f"verified {manifest['release_id']} for {args.target_os}/{args.target_arch}")
    except (OSError, ReleaseError) as error:
        print(f"fleet release rejected: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
