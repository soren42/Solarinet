"""Tests for immutable trust-portal publication and atomic activation."""

from __future__ import annotations

import hashlib
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parent))
from build_trust_portal import (
    PORTAL_FILES,
    TrustPortalError,
    certificate_details_from_bytes,
    write_artifacts,
)
from publish_trust_portal import activate_retained_release, publish_trust_portal


class PublishTrustPortalTests(unittest.TestCase):
    def setUp(self) -> None:
        self.temp = tempfile.TemporaryDirectory(prefix="solari-pki-publish-test-")
        self.base = Path(self.temp.name)
        self.source_a = self._make_source("root-a", "Akoria Test Root A")
        self.root = self.base / "trust"

    def tearDown(self) -> None:
        self.temp.cleanup()

    def _make_source(self, name: str, common_name: str) -> Path:
        cert = self.base / f"{name}.pem"
        key = self.base / f"{name}-key.pem"
        subprocess.run(
            [
                "openssl", "req", "-utf8", "-x509", "-newkey", "rsa:2048", "-nodes",
                "-keyout", str(key), "-out", str(cert), "-days", "2",
                "-subj", f"/O=Akoria Test/CN={common_name}",
                "-addext", "basicConstraints=critical,CA:TRUE",
                "-addext", "keyUsage=critical,keyCertSign,cRLSign",
            ],
            check=True,
            capture_output=True,
        )
        source = self.base / f"{name}-artifacts"
        write_artifacts(cert, source)
        return source

    def _refresh_checksums(self, source: Path) -> None:
        checksums = "".join(
            f"{hashlib.sha256((source / name).read_bytes()).hexdigest()}  {name}\n"
            for name in PORTAL_FILES if name != "SHA256SUMS"
        )
        (source / "SHA256SUMS").write_text(checksums)

    def publish(self, source: Path, root: Path, expected_sha256: str | None = None):
        expected = expected_sha256 or str(
            certificate_details_from_bytes((source / "root_ca.pem").read_bytes())["fingerprint_hex"]
        )
        return publish_trust_portal(source, root, expected)

    def test_publishes_readable_release_and_current_link(self) -> None:
        result = self.publish(self.source_a, self.root)
        current = self.root / "current"
        self.assertTrue(current.is_symlink())
        self.assertEqual(os.readlink(current), f"releases/{Path(result['release']).name}")
        self.assertEqual(current.resolve(), Path(result["release"]).resolve())
        self.assertEqual(self.root.stat().st_mode & 0o777, 0o755)
        self.assertEqual((self.root / "releases").stat().st_mode & 0o777, 0o755)
        self.assertEqual(Path(result["release"]).stat().st_mode & 0o777, 0o755)
        for path in Path(result["release"]).iterdir():
            self.assertEqual(path.stat().st_mode & 0o777, 0o644)

    def test_repeat_publish_is_idempotent(self) -> None:
        first = self.publish(self.source_a, self.root)
        second = self.publish(self.source_a, self.root)
        self.assertEqual(first["release"], second["release"])
        self.assertEqual((self.root / "current").resolve(), Path(first["release"]).resolve())
        self.assertEqual(len(list((self.root / "releases").iterdir())), 1)

    def test_rollback_from_retained_release_directory(self) -> None:
        first = self.publish(self.source_a, self.root)
        source_b = self._make_source("root-b", "Akoria Test Root B")
        second = self.publish(source_b, self.root)
        rollback = self.publish(Path(first["release"]), self.root)
        self.assertEqual(first["release"], rollback["release"])
        self.assertEqual(Path(first["release"]).resolve(), (self.root / "current").resolve())
        self.assertTrue(Path(second["release"]).is_dir())

    def test_retained_activation_survives_builder_template_change(self) -> None:
        first = self.publish(self.source_a, self.root)
        source_b = self._make_source("root-b", "Akoria Test Root B")
        self.publish(source_b, self.root)
        with patch("publish_trust_portal.artifacts_for_details", side_effect=AssertionError("builder invoked")):
            rollback = activate_retained_release(Path(first["release"]).name, self.root,
                                                 str(certificate_details_from_bytes(
                                                     (self.source_a / "root_ca.pem").read_bytes()
                                                 )["fingerprint_hex"]))
        self.assertEqual(Path(first["release"]).resolve(), (self.root / "current").resolve())
        self.assertEqual(first["release"], rollback["release"])

    def test_retained_activation_checks_release_integrity_and_mode(self) -> None:
        first = self.publish(self.source_a, self.root)
        fingerprint = Path(first["release"]).name
        release = Path(first["release"])
        (release / "index.html").write_text("tampered")
        with self.assertRaisesRegex(TrustPortalError, "checksum verification failed"):
            activate_retained_release(fingerprint, self.root, self.publish_pin())
        (release / "index.html").write_bytes((self.source_a / "index.html").read_bytes())
        release.chmod(0o700)
        with self.assertRaisesRegex(TrustPortalError, "publication directory must have mode 0755"):
            activate_retained_release(fingerprint, self.root, self.publish_pin())
        self.assertEqual((self.root / "current").resolve(), release)

    def publish_pin(self) -> str:
        return str(certificate_details_from_bytes((self.source_a / "root_ca.pem").read_bytes())["fingerprint_hex"])

    def test_refuses_existing_publication_directories_with_wrong_modes(self) -> None:
        self.root.mkdir(mode=0o700)
        with self.assertRaisesRegex(TrustPortalError, "publication directory must have mode 0755"):
            self.publish(self.source_a, self.root)
        self.assertFalse((self.root / "releases").exists())

        self.root.chmod(0o755)
        releases = self.root / "releases"
        releases.mkdir(mode=0o700)
        with self.assertRaisesRegex(TrustPortalError, "publication directory must have mode 0755"):
            self.publish(self.source_a, self.root)
        self.assertFalse((self.root / "current").exists())

    def test_rotation_switches_atomically_and_retains_previous_release(self) -> None:
        first = self.publish(self.source_a, self.root)
        source_b = self._make_source("root-b", "Akoria Test Root B")
        second = self.publish(source_b, self.root)
        self.assertNotEqual(first["release"], second["release"])
        self.assertTrue(Path(first["release"]).is_dir())
        self.assertEqual((self.root / "current").resolve(), Path(second["release"]).resolve())

    def test_checksum_failure_preserves_active_release(self) -> None:
        first = self.publish(self.source_a, self.root)
        (self.source_a / "index.html").write_text("tampered")
        with self.assertRaisesRegex(TrustPortalError, "SHA256SUMS verification failed"):
            self.publish(self.source_a, self.root)
        self.assertEqual((self.root / "current").resolve(), Path(first["release"]).resolve())

    def test_rejects_checksumming_a_profile_from_a_different_root(self) -> None:
        other_source = self._make_source("root-other", "Akoria Other Test Root")
        (self.source_a / "akoria-root.mobileconfig").write_bytes(
            (other_source / "akoria-root.mobileconfig").read_bytes()
        )
        self._refresh_checksums(self.source_a)
        with self.assertRaisesRegex(
            TrustPortalError, "trust artifact does not match root certificate: akoria-root.mobileconfig"
        ):
            self.publish(self.source_a, self.root)
        self.assertFalse(self.root.exists())

    def test_requires_independently_matching_root_fingerprint_before_filesystem_change(self) -> None:
        with self.assertRaisesRegex(TrustPortalError, "independently verified --expect-sha256"):
            self.publish(self.source_a, self.root, "00" * 32)
        self.assertFalse(self.root.exists())

    def test_accepts_colon_delimited_root_fingerprint(self) -> None:
        fingerprint = str(
            certificate_details_from_bytes((self.source_a / "root_ca.pem").read_bytes())["fingerprint_hex"]
        )
        colon_delimited = ":".join(fingerprint[index:index + 2]
                                    for index in range(0, len(fingerprint), 2))
        result = self.publish(self.source_a, self.root, colon_delimited)
        self.assertEqual((self.root / "current").resolve(), Path(result["release"]).resolve())

    def test_rejects_malformed_expected_fingerprint_before_filesystem_change(self) -> None:
        with self.assertRaisesRegex(TrustPortalError, "independently verified --expect-sha256"):
            self.publish(self.source_a, self.root, "not-a-fingerprint")
        self.assertFalse(self.root.exists())

    def test_rejects_symlinked_source_directory_and_artifact(self) -> None:
        source_alias = self.base / "source-alias"
        source_alias.symlink_to(self.source_a, target_is_directory=True)
        with self.assertRaisesRegex(TrustPortalError, "source must be a real artifact directory"):
            self.publish(source_alias, self.root)

        symlinked_file_source = self.base / "symlinked-file-source"
        symlinked_file_source.mkdir()
        for name in PORTAL_FILES:
            target = self.source_a / name
            destination = symlinked_file_source / name
            if name == "root_ca.pem":
                destination.symlink_to(target)
            else:
                destination.write_bytes(target.read_bytes())
        with self.assertRaisesRegex(TrustPortalError, "regular file: root_ca.pem"):
            self.publish(symlinked_file_source, self.root)

    def test_refuses_existing_release_with_different_content(self) -> None:
        first = self.publish(self.source_a, self.root)
        release = Path(first["release"])
        (release / "index.html").write_text("altered release")
        with self.assertRaisesRegex(TrustPortalError, "existing immutable release differs at index.html"):
            self.publish(self.source_a, self.root)
        self.assertEqual((self.root / "current").resolve(), release)

    def test_rejects_unsafe_current_symlink_targets(self) -> None:
        outside = self.base / "outside-release"
        outside.mkdir()
        cases = (
            ("absolute", str(outside)),
            ("parent-traversal", "releases/.."),
            ("symlinked-out", "releases/escape"),
        )
        for label, target in cases:
            with self.subTest(target=label):
                root = self.base / f"unsafe-{label}" / "trust"
                releases = root / "releases"
                releases.mkdir(parents=True)
                root.chmod(0o755)
                releases.chmod(0o755)
                if label == "symlinked-out":
                    (releases / "escape").symlink_to(outside, target_is_directory=True)
                (root / "current").symlink_to(target)
                with self.assertRaises(TrustPortalError):
                    self.publish(self.source_a, root)

    def test_dangling_current_symlink_has_recovery_guidance(self) -> None:
        self.root.mkdir()
        self.root.chmod(0o755)
        (self.root / "releases").mkdir()
        (self.root / "releases").chmod(0o755)
        (self.root / "current").symlink_to("releases/missing")
        with self.assertRaisesRegex(TrustPortalError, "inspect and repair or remove"):
            self.publish(self.source_a, self.root)

    def test_non_symlink_current_path_is_preserved_and_refused(self) -> None:
        self.root.mkdir()
        self.root.chmod(0o755)
        (self.root / "current").mkdir()
        (self.root / "current" / "operator-data").write_text("preserve")
        with self.assertRaisesRegex(TrustPortalError, "refusing to replace non-symlink current path"):
            self.publish(self.source_a, self.root)
        self.assertEqual((self.root / "current" / "operator-data").read_text(), "preserve")

    def test_failed_link_swap_keeps_previous_release_active(self) -> None:
        first = self.publish(self.source_a, self.root)
        source_b = self._make_source("root-b", "Akoria Test Root B")
        with patch("publish_trust_portal.os.replace", side_effect=OSError("simulated link switch failure")):
            with self.assertRaisesRegex(TrustPortalError, "cannot atomically activate trust release"):
                self.publish(source_b, self.root)
        self.assertEqual((self.root / "current").resolve(), Path(first["release"]).resolve())
        self.assertEqual(list(self.root.glob(".current-*")), [])


if __name__ == "__main__":
    unittest.main()
