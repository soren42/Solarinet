#!/usr/bin/env python3
"""Temporary-root tests for the dashboard release packaging and activation flow."""

from __future__ import annotations

import io
import importlib.util
import json
import os
import subprocess
import sys
import tarfile
import tempfile
import unittest
from unittest.mock import patch
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
PACKAGER = ROOT / "deploy/dashboard/package-release.py"
INSTALLER = ROOT / "deploy/dashboard/install-release.py"
BIN_DIR = Path(os.environ.get("SOLARI_RELEASE_BIN_DIR", ROOT / "build-server/src/server"))
BINARY = BIN_DIR / "solariServer"
SCAN_WORKER = BIN_DIR / "solariScanWorker"
PACKAGER_SPEC = importlib.util.spec_from_file_location("dashboard_package_release", PACKAGER)
assert PACKAGER_SPEC and PACKAGER_SPEC.loader
PACKAGER_MODULE = importlib.util.module_from_spec(PACKAGER_SPEC)
PACKAGER_SPEC.loader.exec_module(PACKAGER_MODULE)
SOURCE_DIRTY = bool(subprocess.check_output(
    ["git", "-C", str(ROOT), "status", "--porcelain", "--untracked-files=all"], text=True
).strip())


class ReleaseFlowTests(unittest.TestCase):
    def test_submodule_provenance_rejects_checkout_off_gitlink(self) -> None:
        with patch.object(subprocess, "check_output", side_effect=["a" * 40, "b" * 40]):
            with self.assertRaisesRegex(ValueError, "does not match parent gitlink"):
                PACKAGER_MODULE.source_submodules()

    def test_submodule_provenance_rejects_uninitialized_checkout(self) -> None:
        failure = subprocess.CalledProcessError(128, ["git"])
        with patch.object(subprocess, "check_output", side_effect=failure):
            with self.assertRaisesRegex(ValueError, "required submodule is not initialized"):
                PACKAGER_MODULE.source_submodules()

    def run_packager(self, version: str, output: Path) -> None:
        subprocess.run(
            [sys.executable, str(PACKAGER), "--version", version, "--binary", str(BINARY),
             "--scan-worker-binary", str(SCAN_WORKER), "--output", str(output),
             *( ["--allow-dirty"] if SOURCE_DIRTY else [] )],
            check=True,
            cwd=ROOT,
            stdout=subprocess.DEVNULL,
        )

    def run_installer(self, root: Path, *arguments: str, success: bool = True,
                      allow_dirty: bool | None = None) -> subprocess.CompletedProcess[str]:
        if allow_dirty is None:
            allow_dirty = SOURCE_DIRTY
        dirty_option = ["--allow-dirty"] if allow_dirty else []
        result = subprocess.run(
            [sys.executable, str(INSTALLER), "--root", str(root), *arguments, *dirty_option],
            cwd=ROOT,
            text=True,
            capture_output=True,
        )
        self.assertEqual(result.returncode == 0, success, result.stderr)
        return result

    def test_dirty_source_is_rejected_by_default_packager(self) -> None:
        if not SOURCE_DIRTY:
            self.skipTest("source tree is clean; no dirty source to reject")
        with tempfile.TemporaryDirectory(prefix="solarinet-release-dirty-package-") as temp:
            output = Path(temp) / "must-not-exist.tar.gz"
            result = subprocess.run(
                [sys.executable, str(PACKAGER), "--version", "dirty-rejected",
                 "--binary", str(BINARY), "--scan-worker-binary", str(SCAN_WORKER),
                 "--output", str(output)],
                cwd=ROOT,
                text=True,
                capture_output=True,
            )
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("source worktree is dirty", result.stderr)
            self.assertFalse(output.exists())

    def test_install_and_rollback_use_one_active_release(self) -> None:
        with tempfile.TemporaryDirectory(prefix="solarinet-release-test-") as temp:
            base = Path(temp)
            first, second = base / "one.tar.gz", base / "two.tar.gz"
            root = base / "install"
            self.run_packager("test-1", first)
            self.run_packager("test-2", second)
            with tarfile.open(first, "r:gz") as archive:
                names = set(archive.getnames())
                metadata = json.load(archive.extractfile("RELEASE.json"))
            self.assertIn("dashboard/api/index.php", names)
            self.assertIn("dashboard/public/index.html", names)
            self.assertIn("dashboard/public/rackwire/index.html", names)
            self.assertIn("bin/solariServer", names)
            self.assertIn("bin/solariScanWorker", names)
            self.assertFalse(any(
                name == ".git" or "/.git/" in name or name.startswith(".git/")
                or "/.github/" in name or name.startswith(".github/")
                or name.endswith((".key", ".pem"))
                for name in names
            ))
            self.assertEqual(metadata["version"], "test-1")
            self.assertEqual(metadata["sourceTreeDirty"], SOURCE_DIRTY)
            rackwire = subprocess.check_output(
                ["git", "-C", str(ROOT / "dashboard/public/rackwire"), "rev-parse", "HEAD"],
                text=True,
            ).strip()
            self.assertEqual(metadata["sourceSubmodules"], {
                "dashboard/public/rackwire": rackwire,
            })
            parent_pin = subprocess.check_output(
                ["git", "-C", str(ROOT), "rev-parse", "HEAD:dashboard/public/rackwire"],
                text=True,
            ).strip()
            self.assertEqual(rackwire, parent_pin)

            self.run_installer(root, "--bundle", str(first))
            self.assertEqual((root / "current").resolve().name, "test-1")
            self.run_installer(root, "--bundle", str(first), success=False)
            self.assertEqual((root / "current").resolve().name, "test-1")
            self.run_installer(root, "--bundle", str(second))
            self.assertEqual((root / "current").resolve().name, "test-2")
            self.assertTrue((root / "releases/test-1/dashboard/public/index.html").is_file())
            self.run_installer(root, "--activate", "test-1")
            self.assertEqual((root / "current").resolve().name, "test-1")

    def test_dirty_preflight_bundle_requires_explicit_installer_opt_in(self) -> None:
        if not SOURCE_DIRTY:
            self.skipTest("source tree is clean; no dirty preflight bundle to reject")
        with tempfile.TemporaryDirectory(prefix="solarinet-release-dirty-") as temp:
            base = Path(temp)
            bundle, root = base / "dirty.tar.gz", base / "install"
            self.run_packager("dirty-preflight", bundle)
            self.run_installer(root, "--bundle", str(bundle), success=False, allow_dirty=False)
            self.assertFalse((root / "current").exists())
            self.run_installer(root, "--bundle", str(bundle), allow_dirty=True)
            self.assertEqual((root / "current").resolve().name, "dirty-preflight")
            self.run_installer(root, "--activate", "dirty-preflight", success=False, allow_dirty=False)
            self.run_installer(root, "--activate", "dirty-preflight", allow_dirty=True)

    def test_rejects_modified_payload_without_switching_current(self) -> None:
        with tempfile.TemporaryDirectory(prefix="solarinet-release-corrupt-") as temp:
            base = Path(temp)
            good, altered = base / "good.tar.gz", base / "altered.tar.gz"
            root = base / "install"
            self.run_packager("good", good)
            self.run_installer(root, "--bundle", str(good))
            with tarfile.open(good, "r:gz") as source, tarfile.open(altered, "w:gz") as dest:
                for member in source.getmembers():
                    stream = source.extractfile(member) if member.isfile() else None
                    if member.name == "dashboard/public/index.html":
                        member.size = len(b"tampered")
                        dest.addfile(member, io.BytesIO(b"tampered"))
                    else:
                        dest.addfile(member, stream)
            self.run_installer(root, "--bundle", str(altered), success=False)
            self.assertEqual((root / "current").resolve().name, "good")

    def test_rejects_bundle_missing_embedded_rackwire_application(self) -> None:
        with tempfile.TemporaryDirectory(prefix="solarinet-release-no-rackwire-") as temp:
            base = Path(temp)
            good, incomplete, root = base / "good.tar.gz", base / "incomplete.tar.gz", base / "install"
            self.run_packager("missing-rackwire", good)
            with tarfile.open(good, "r:gz") as source, tarfile.open(incomplete, "w:gz") as dest:
                for member in source.getmembers():
                    if member.name == "dashboard/public/rackwire/index.html":
                        continue
                    stream = source.extractfile(member) if member.isfile() else None
                    if member.name == "RELEASE.json":
                        metadata = json.load(stream)
                        del metadata["files"]["dashboard/public/rackwire/index.html"]
                        payload = (json.dumps(metadata, sort_keys=True, indent=2) + "\n").encode()
                        member.size = len(payload)
                        stream = io.BytesIO(payload)
                    dest.addfile(member, stream)
            self.run_installer(root, "--bundle", str(incomplete), success=False)
            self.assertFalse((root / "current").exists())

    def test_rejects_path_traversal_without_writing_outside_root(self) -> None:
        with tempfile.TemporaryDirectory(prefix="solarinet-release-path-") as temp:
            base = Path(temp)
            malicious = base / "malicious.tar.gz"
            root = base / "install"
            with tarfile.open(malicious, "w:gz") as archive:
                info = tarfile.TarInfo("../escape")
                payload = b"escape"
                info.size = len(payload)
                archive.addfile(info, io.BytesIO(payload))
            self.run_installer(root, "--bundle", str(malicious), success=False)
            self.assertFalse((base / "escape").exists())
            self.assertFalse((root / "current").exists())

    def test_rejects_symlink_archive_members(self) -> None:
        with tempfile.TemporaryDirectory(prefix="solarinet-release-symlink-") as temp:
            base = Path(temp)
            malicious = base / "symlink.tar.gz"
            root = base / "install"
            with tarfile.open(malicious, "w:gz") as archive:
                info = tarfile.TarInfo("dashboard/public/escape")
                info.type = tarfile.SYMTYPE
                info.linkname = "../../../../outside"
                archive.addfile(info)
            result = self.run_installer(root, "--bundle", str(malicious), success=False)
            self.assertIn("unsupported archive entry", result.stderr)
            self.assertFalse((base / "outside").exists())
            self.assertFalse((root / "current").exists())


if __name__ == "__main__":
    unittest.main(verbosity=2)
