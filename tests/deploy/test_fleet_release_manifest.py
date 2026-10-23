#!/usr/bin/env python3
"""Offline trust-chain tests for the fleet release manifest tool."""
from __future__ import annotations

import datetime as dt
import os
import pathlib
import subprocess
import sys
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT))
from deploy.fleet import release_manifest as release  # noqa: E402


def openssl(*args: str) -> None:
    subprocess.run(["openssl", *args], check=True, capture_output=True, text=True)


class FleetReleaseManifestTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.temp = tempfile.TemporaryDirectory(prefix="fleet-release-test-")
        cls.base = pathlib.Path(cls.temp.name)
        cls.ca = cls.base / "root.pem"
        cls.caKey = cls.base / "root.key"
        cls.signer = cls.base / "signer.pem"
        cls.signerKey = cls.base / "signer.key"
        cls.otherSigner = cls.base / "other-signer.pem"
        cls.otherSignerKey = cls.base / "other-signer.key"
        csr = cls.base / "signer.csr"
        ext = cls.base / "signer.ext"
        otherCsr = cls.base / "other-signer.csr"
        openssl("req", "-x509", "-newkey", "rsa:2048", "-nodes",
                "-keyout", str(cls.caKey), "-out", str(cls.ca), "-days", "3",
                "-subj", "/CN=SolariNet Test Root",
                "-addext", "basicConstraints=critical,CA:TRUE",
                "-addext", "keyUsage=critical,keyCertSign,cRLSign")
        openssl("req", "-newkey", "rsa:2048", "-nodes",
                "-keyout", str(cls.signerKey), "-out", str(csr),
                "-subj", "/CN=SolariNet Test Code Signer")
        ext.write_text("basicConstraints=critical,CA:FALSE\n"
                       "keyUsage=critical,digitalSignature\n"
                       "extendedKeyUsage=codeSigning\n", encoding="ascii")
        openssl("x509", "-req", "-in", str(csr), "-CA", str(cls.ca),
                "-CAkey", str(cls.caKey), "-CAcreateserial", "-out", str(cls.signer),
                "-days", "2", "-extfile", str(ext))
        openssl("req", "-newkey", "rsa:2048", "-nodes", "-keyout",
                str(cls.otherSignerKey), "-out", str(otherCsr),
                "-subj", "/CN=Other SolariNet Code Signer")
        openssl("x509", "-req", "-in", str(otherCsr), "-CA", str(cls.ca),
                "-CAkey", str(cls.caKey), "-CAcreateserial", "-out", str(cls.otherSigner),
                "-days", "2", "-extfile", str(ext))

    @classmethod
    def tearDownClass(cls) -> None:
        cls.temp.cleanup()

    def setUp(self) -> None:
        self.temp = tempfile.TemporaryDirectory(prefix="fleet-release-case-")
        self.base = pathlib.Path(self.temp.name)
        self.source = self.base / "source"
        self.source.mkdir()
        (self.source / "boot.ipxe").write_text("#!ipxe\nexit\n", encoding="ascii")
        (self.source / "debian" / "x86_64").mkdir(parents=True)
        (self.source / "debian" / "x86_64" / "vmlinuz").write_bytes(b"test-kernel-bytes")
        self.output = self.base / "releases" / "candidate-1"
        now = dt.datetime.now(dt.timezone.utc).replace(microsecond=0)
        self.notBefore = now.strftime("%Y-%m-%dT%H:%M:%SZ")
        self.expires = (now + dt.timedelta(days=1)).strftime("%Y-%m-%dT%H:%M:%SZ")

    def tearDown(self) -> None:
        self.temp.cleanup()

    def create(self, expires: str | None = None) -> None:
        manifest = release.buildManifest(self.source, "candidate-1", self.notBefore,
                                         expires or self.expires,
                                         ["debian/x86_64"], self.signer)
        release.createRelease(self.source, self.output, manifest, self.signer,
                              self.signerKey, self.ca)

    def verify(self, **overrides):
        options = {"expectedReleaseId": "candidate-1", "targetOs": "debian",
                   "targetArch": "x86_64", "trustRoot": self.ca,
                   "expectedSignerFingerprint": release.certificateFingerprint(self.signer)}
        options.update(overrides)
        return release.verifyRelease(self.output, **options)

    def makeWritable(self) -> None:
        for current, dirs, files in os.walk(self.output):
            pathlib.Path(current).chmod(0o755)
            for name in dirs:
                (pathlib.Path(current) / name).chmod(0o755)
            for name in files:
                (pathlib.Path(current) / name).chmod(0o644)

    def makeReadOnly(self) -> None:
        for current, dirs, files in os.walk(self.output, topdown=False):
            currentPath = pathlib.Path(current)
            for name in files:
                path = currentPath / name
                if not path.is_symlink():
                    path.chmod(0o444)
            for name in dirs:
                path = currentPath / name
                if not path.is_symlink():
                    path.chmod(0o555)
        self.output.chmod(0o555)

    def resignManifest(self, manifest: dict) -> None:
        path = self.output / release.MANIFEST_NAME
        path.write_bytes(release.canonicalJson(manifest))
        release.signCms(path, pathlib.Path(str(path) + ".sig"), self.signer,
                         self.signerKey, self.ca)

    def test_signs_and_verifies_immutable_release_for_supported_target(self) -> None:
        self.create()
        self.assertEqual("candidate-1", self.verify()["release_id"])
        self.assertEqual(0, (self.output.stat().st_mode & 0o222))

    def test_cli_create_and_verify_roundtrip(self) -> None:
        tool = ROOT / "deploy/fleet/release_manifest.py"
        signerPin = release.certificateFingerprint(self.signer)
        created = subprocess.run([
            sys.executable, str(tool), "create", "--source", str(self.source),
            "--output", str(self.output), "--release-id", "candidate-1",
            "--not-before", self.notBefore, "--expires-at", self.expires,
            "--target", "debian/x86_64", "--signer-cert", str(self.signer),
            "--signing-key", str(self.signerKey), "--ca-cert", str(self.ca),
        ], capture_output=True, text=True)
        self.assertEqual(0, created.returncode, created.stderr)
        verified = subprocess.run([
            sys.executable, str(tool), "verify", "--release-root", str(self.output),
            "--expected-release-id", "candidate-1", "--os", "debian",
            "--arch", "x86_64", "--trust-root", str(self.ca),
            "--expected-signer-sha256", signerPin,
        ], capture_output=True, text=True)
        self.assertEqual(0, verified.returncode, verified.stderr)

    def test_preserves_executable_mode_as_read_only_executable(self) -> None:
        (self.source / "boot.ipxe").chmod(0o755)
        self.create()
        self.assertEqual(0o555, (self.output / "boot.ipxe").stat().st_mode & 0o777)
        self.verify()

    def test_rejects_wrong_release_id(self) -> None:
        self.create()
        with self.assertRaisesRegex(release.ReleaseError, "does not match"):
            self.verify(expectedReleaseId="different")

    def test_rejects_unsupported_target_at_creation_and_verification(self) -> None:
        with self.assertRaisesRegex(release.ReleaseError, "unsupported target"):
            release.buildManifest(self.source, "candidate-1", self.notBefore,
                                  self.expires, ["freebsd/x86_64"], self.signer)
        self.create()
        with self.assertRaisesRegex(release.ReleaseError, "does not support"):
            self.verify(targetOs="freebsd", targetArch="x86_64")

    def test_rejects_expired_release(self) -> None:
        old = (dt.datetime.now(dt.timezone.utc) - dt.timedelta(days=2))
        notBefore = (old - dt.timedelta(days=1)).strftime("%Y-%m-%dT%H:%M:%SZ")
        expires = (old + dt.timedelta(days=1)).strftime("%Y-%m-%dT%H:%M:%SZ")
        manifest = release.buildManifest(self.source, "candidate-1", notBefore,
                                         expires, ["debian/x86_64"], self.signer)
        release.createRelease(self.source, self.output, manifest, self.signer,
                              self.signerKey, self.ca)
        with self.assertRaisesRegex(release.ReleaseError, "expired"):
            self.verify()

    def test_rejects_artifact_tampering(self) -> None:
        self.create()
        self.makeWritable()
        (self.output / "boot.ipxe").write_text("#!ipxe\nreboot\n", encoding="ascii")
        self.makeReadOnly()
        with self.assertRaisesRegex(release.ReleaseError, "length mismatch|digest mismatch"):
            self.verify()

    def test_rejects_same_length_artifact_tampering(self) -> None:
        self.create()
        self.makeWritable()
        (self.output / "boot.ipxe").write_text("#!ipxe\nnoop\n", encoding="ascii")
        self.makeReadOnly()
        with self.assertRaisesRegex(release.ReleaseError, "digest mismatch"):
            self.verify()

    def test_rejects_a_signed_manifest_path_traversal(self) -> None:
        self.create()
        self.makeWritable()
        manifest = release.loadCanonicalManifest(self.output / release.MANIFEST_NAME)
        manifest["artifacts"][0]["path"] = "../escape"
        self.resignManifest(manifest)
        self.makeReadOnly()
        with self.assertRaisesRegex(release.ReleaseError, "unsafe or reserved"):
            self.verify()

    def test_rejects_unlisted_files_and_symlink_paths(self) -> None:
        self.create()
        self.makeWritable()
        (self.output / "extra.sh").write_text("echo unexpected\n", encoding="ascii")
        self.makeReadOnly()
        with self.assertRaisesRegex(release.ReleaseError, "file set mismatch"):
            self.verify()
        self.makeWritable()
        (self.output / "extra.sh").unlink()
        (self.output / "link").symlink_to(self.output / "boot.ipxe")
        self.makeReadOnly()
        with self.assertRaisesRegex(release.ReleaseError, "symlink"):
            self.verify()

    def test_rejects_modified_manifest_signature(self) -> None:
        self.create()
        self.makeWritable()
        path = self.output / release.MANIFEST_NAME
        path.write_bytes(path.read_bytes().replace(b"candidate-1", b"candidate-2"))
        self.makeReadOnly()
        with self.assertRaisesRegex(release.ReleaseError, "OpenSSL operation failed"):
            self.verify()

    def test_rejects_malformed_detached_signature(self) -> None:
        self.create()
        self.makeWritable()
        (self.output / "manifest.json.sig").write_bytes(b"not a CMS signature")
        self.makeReadOnly()
        with self.assertRaisesRegex(release.ReleaseError, "OpenSSL operation failed"):
            self.verify()

    def test_rejects_writable_release_tree(self) -> None:
        self.create()
        self.makeWritable()
        with self.assertRaisesRegex(release.ReleaseError, "immutable mode"):
            self.verify()

    def test_rejects_changed_executable_mode(self) -> None:
        (self.source / "boot.ipxe").chmod(0o755)
        self.create()
        self.makeWritable()
        (self.output / "boot.ipxe").chmod(0o644)
        self.makeReadOnly()
        with self.assertRaisesRegex(release.ReleaseError, "mode mismatch"):
            self.verify()

    def test_rejects_signer_with_wrong_trust_root(self) -> None:
        self.create()
        with self.assertRaisesRegex(release.ReleaseError, "OpenSSL operation failed"):
            self.verify(trustRoot=self.signer)

    def test_rejects_non_codesigning_certificate(self) -> None:
        wrongKey = self.base / "wrong.key"
        wrongCsr = self.base / "wrong.csr"
        wrongCert = self.base / "wrong.pem"
        wrongExt = self.base / "wrong.ext"
        openssl("req", "-newkey", "rsa:2048", "-nodes", "-keyout", str(wrongKey),
                "-out", str(wrongCsr), "-subj", "/CN=Wrong Usage")
        wrongExt.write_text("basicConstraints=critical,CA:FALSE\n"
                            "keyUsage=critical,digitalSignature\n"
                            "extendedKeyUsage=serverAuth\n", encoding="ascii")
        openssl("x509", "-req", "-in", str(wrongCsr), "-CA", str(self.ca),
                "-CAkey", str(self.caKey), "-CAcreateserial", "-out", str(wrongCert),
                "-days", "2", "-extfile", str(wrongExt))
        manifest = release.buildManifest(self.source, "candidate-1", self.notBefore,
                                         self.expires, ["debian/x86_64"], wrongCert)
        release.createRelease(self.source, self.output, manifest, wrongCert,
                              wrongKey, self.ca)
        with self.assertRaisesRegex(release.ReleaseError, "OpenSSL operation failed"):
            self.verify(expectedSignerFingerprint=
                        release.certificateFingerprint(wrongCert))

    def test_rejects_other_code_signer_under_same_ca(self) -> None:
        manifest = release.buildManifest(self.source, "candidate-1", self.notBefore,
                                         self.expires, ["debian/x86_64"], self.otherSigner)
        release.createRelease(self.source, self.output, manifest, self.otherSigner,
                              self.otherSignerKey, self.ca)
        with self.assertRaisesRegex(release.ReleaseError, "pinned signer"):
            self.verify()


if __name__ == "__main__":
    unittest.main()
