#!/usr/bin/env python3
"""Tests for public-only CA trust artifact generation."""

from __future__ import annotations

import hashlib
from html.parser import HTMLParser
import json
import plistlib
import subprocess
import tempfile
import unittest
from contextlib import redirect_stderr
from datetime import datetime, timezone
from io import StringIO
from pathlib import Path
from unittest.mock import patch

from build_trust_portal import PORTAL_FILES, TrustPortalError, main, write_artifacts


class BuildTrustPortalTests(unittest.TestCase):
    def setUp(self) -> None:
        self.temp = tempfile.TemporaryDirectory(prefix="solari-pki-test-")
        self.root = Path(self.temp.name)
        self.openssl = subprocess.run(
            [
                "openssl", "req", "-utf8", "-x509", "-newkey", "rsa:2048", "-nodes",
                "-keyout", str(self.root / "private-test-key.pem"),
                "-out", str(self.root / "root.pem"), "-days", "2",
                "-subj", "/O=Test Lab/CN=Test Root CA",
                "-addext", "basicConstraints=critical,CA:TRUE",
                "-addext", "keyUsage=critical,keyCertSign,cRLSign",
            ],
            check=True,
            capture_output=True,
        )
        self.assertEqual(self.openssl.returncode, 0)

    def tearDown(self) -> None:
        self.temp.cleanup()

    def test_generates_verified_pem_profile_page_and_checksums(self) -> None:
        out = self.root / "site"
        summary = write_artifacts(self.root / "root.pem", out)

        self.assertEqual(set(p.name for p in out.iterdir()), set(PORTAL_FILES))
        self.assertEqual(out.stat().st_mode & 0o777, 0o755)
        pem = (out / "root_ca.pem").read_bytes()
        profile_data = (out / "akoria-root.mobileconfig").read_bytes()
        profile = plistlib.loads(profile_data)
        self.assertEqual(profile["PayloadType"], "Configuration")
        self.assertFalse(profile["PayloadRemovalDisallowed"])
        self.assertEqual(len(profile["PayloadContent"]), 1)
        cert_payload = profile["PayloadContent"][0]
        self.assertEqual(cert_payload["PayloadType"], "com.apple.security.root")
        self.assertEqual(cert_payload["PayloadContent"], subprocess.run(
            ["openssl", "x509", "-in", str(self.root / "root.pem"), "-outform", "DER"],
            check=True, capture_output=True,
        ).stdout)
        self.assertEqual(cert_payload["PayloadUUID"], cert_payload["PayloadUUID"].upper())
        self.assertEqual(profile["PayloadUUID"], profile["PayloadUUID"].upper())
        firefox_policy = json.loads((out / "firefox-policies.json").read_bytes())
        self.assertEqual(
            firefox_policy["policies"]["Certificates"]["Install"],
            ["/etc/ssl/solarinet/akoria-root.pem"],
        )
        self.assertIn(summary["fingerprint"].encode(), (out / "index.html").read_bytes())
        self.assertIn(b"separate trusted step", (out / "index.html").read_bytes())
        self.assertIn(b"independent trusted channel", (out / "index.html").read_bytes())
        self.assertIn(b"Certificate Trust Settings", (out / "index.html").read_bytes())
        self.assertIn(b"Firefox 120 and later", (out / "index.html").read_bytes())
        self.assertIn(b"System keychain", (out / "index.html").read_bytes())
        self.assertIn(b"-p ssl -p basic", (out / "index.html").read_bytes())
        self.assertIn(b"CA certificate", (out / "index.html").read_bytes())
        self.assertIn(b"Verify the result", (out / "index.html").read_bytes())
        self.assertIn(b"Do not bypass a certificate warning", (out / "index.html").read_bytes())
        self.assertIn(b"To remove it", (out / "index.html").read_bytes())
        self.assertTrue(pem.startswith(b"-----BEGIN CERTIFICATE-----"))
        checksums = (out / "SHA256SUMS").read_text().splitlines()
        checksum_names = set()
        for line in checksums:
            digest, name = line.split("  ", 1)
            checksum_names.add(name)
            self.assertEqual(digest, hashlib.sha256((out / name).read_bytes()).hexdigest())
        self.assertEqual(checksum_names, set(PORTAL_FILES) - {"SHA256SUMS"})
        for path in out.iterdir():
            self.assertEqual(path.stat().st_mode & 0o777, 0o644)
            self.assertNotIn(b"PRIVATE KEY", path.read_bytes())

        class LinkParser(HTMLParser):
            def __init__(self) -> None:
                super().__init__()
                self.hrefs: list[str] = []
                self.ids: set[str] = set()
                self.script_count = 0
                self.event_handlers: list[str] = []

            def handle_starttag(self, tag: str, attrs: list[tuple[str, str | None]]) -> None:
                values = dict(attrs)
                if tag.lower() == "script":
                    self.script_count += 1
                self.event_handlers.extend(name for name in values if name.lower().startswith("on"))
                if tag == "a" and values.get("href"):
                    self.hrefs.append(str(values["href"]))
                if values.get("id"):
                    self.ids.add(str(values["id"]))

        parser = LinkParser()
        parser.feed((out / "index.html").read_text())
        self.assertTrue({"akoria-root.mobileconfig", "root_ca.pem", "firefox-policies.json"}.issubset(parser.hrefs))
        self.assertTrue(any(href.startswith("https://support.apple.com/") for href in parser.hrefs))
        self.assertTrue(any(href.startswith("https://support.mozilla.org/") for href in parser.hrefs))
        self.assertTrue(any(href.startswith("https://support.google.com/android/") for href in parser.hrefs))
        self.assertTrue(any(href.startswith("https://docs.zen-browser.app/") for href in parser.hrefs))
        self.assertTrue({"first-use", "identity", "platform"}.issubset(parser.ids))
        self.assertEqual(parser.script_count, 0)
        self.assertEqual(parser.event_handlers, [])
        self.assertEqual(sum(href.startswith("https://") for href in parser.hrefs), 4)
        self.assertIn(b'download="firefox-policies.json"', (out / "index.html").read_bytes())

    def test_rejects_expired_ca_with_clear_error(self) -> None:
        class FutureDateTime:
            @staticmethod
            def now(tz=None):
                return datetime(2099, 1, 1, tzinfo=timezone.utc)

            @staticmethod
            def strptime(value, fmt):
                return datetime.strptime(value, fmt)

        with patch("build_trust_portal.datetime", FutureDateTime):
            with self.assertRaisesRegex(TrustPortalError, "CA certificate expired at .*CN=Test Root CA.*SHA-256"):
                write_artifacts(self.root / "root.pem", self.root / "expired-site")

    def test_rejects_not_yet_valid_ca(self) -> None:
        class FrozenDateTime:
            @staticmethod
            def now(tz=None):
                return datetime(2000, 1, 1, tzinfo=timezone.utc)

            @staticmethod
            def strptime(value, fmt):
                return datetime.strptime(value, fmt)

        with patch("build_trust_portal.datetime", FrozenDateTime):
            with self.assertRaisesRegex(TrustPortalError, "not valid until .*SHA-256"):
                write_artifacts(self.root / "root.pem", self.root / "future-site")

    def test_rejects_ca_without_certificate_signing_usage(self) -> None:
        ca = self.root / "wrong-usage.pem"
        subprocess.run(
            [
                "openssl", "req", "-x509", "-newkey", "rsa:2048", "-nodes",
                "-keyout", str(self.root / "wrong-usage-key.pem"), "-out", str(ca),
                "-days", "2", "-subj", "/CN=Wrong Usage Root",
                "-addext", "basicConstraints=critical,CA:TRUE",
                "-addext", "keyUsage=critical,digitalSignature",
            ],
            check=True,
            capture_output=True,
        )
        with self.assertRaisesRegex(TrustPortalError, "does not permit certificate signing"):
            write_artifacts(ca, self.root / "wrong-usage-site")

    def test_accepts_ca_without_optional_key_usage_extension(self) -> None:
        ca = self.root / "no-key-usage.pem"
        subprocess.run(
            [
                "openssl", "req", "-x509", "-newkey", "rsa:2048", "-nodes",
                "-keyout", str(self.root / "no-key-usage-key.pem"), "-out", str(ca),
                "-days", "2", "-subj", "/CN=CA Without Key Usage",
                "-addext", "basicConstraints=critical,CA:TRUE",
            ],
            check=True,
            capture_output=True,
        )
        summary = write_artifacts(ca, self.root / "no-key-usage-site")
        self.assertTrue(summary["fingerprint"])

    def test_rejects_pem_bundles_and_private_key_trailing_data(self) -> None:
        root_bytes = (self.root / "root.pem").read_bytes()
        bundle = self.root / "bundle.pem"
        bundle.write_bytes(root_bytes + root_bytes)
        with self.assertRaisesRegex(TrustPortalError, "exactly one PEM certificate"):
            write_artifacts(bundle, self.root / "bundle-site")
        with_key = self.root / "root-and-key.pem"
        with_key.write_bytes(root_bytes + (self.root / "private-test-key.pem").read_bytes())
        with self.assertRaisesRegex(TrustPortalError, "unexpected data follows"):
            write_artifacts(with_key, self.root / "key-site")

    def test_rejects_concatenated_der_certificates(self) -> None:
        der = subprocess.run(
            ["openssl", "x509", "-in", str(self.root / "root.pem"), "-outform", "DER"],
            check=True,
            capture_output=True,
        ).stdout
        bundle = self.root / "bundle.der"
        bundle.write_bytes(der + der)
        with self.assertRaisesRegex(TrustPortalError, "exactly one DER certificate"):
            write_artifacts(bundle, self.root / "der-bundle-site")

    def test_output_path_file_returns_operator_error(self) -> None:
        output_file = self.root / "not-a-directory"
        output_file.write_text("preserve")
        with self.assertRaisesRegex(TrustPortalError, "refusing to overwrite existing trust portal output path"):
            write_artifacts(self.root / "root.pem", output_file)
        self.assertEqual(output_file.read_text(), "preserve")

    def test_refuses_output_directory_symlink(self) -> None:
        target = self.root / "real-output"
        target.mkdir()
        link = self.root / "linked-output"
        link.symlink_to(target, target_is_directory=True)
        with self.assertRaisesRegex(TrustPortalError, "output directory symlink"):
            write_artifacts(self.root / "root.pem", link)
        self.assertEqual(list(target.iterdir()), [])

    def test_cli_reports_invalid_certificate_with_exit_two(self) -> None:
        err = StringIO()
        with redirect_stderr(err):
            code = main(["--root", str(self.root / "private-test-key.pem"), "--output", str(self.root / "bad-site")])
        self.assertEqual(code, 2)
        self.assertIn("error: openssl command failed", err.getvalue())

    def test_accepts_der_root_input(self) -> None:
        der_path = self.root / "root.der"
        der_path.write_bytes(subprocess.run(
            ["openssl", "x509", "-in", str(self.root / "root.pem"), "-outform", "DER"],
            check=True, capture_output=True,
        ).stdout)
        summary = write_artifacts(der_path, self.root / "der-site")
        self.assertIn(":", summary["fingerprint"])

    def test_preserves_non_ascii_subject_bytes_in_portal(self) -> None:
        root = self.root / "unicode-root.pem"
        subprocess.run(
            [
                "openssl", "req", "-utf8", "-x509", "-newkey", "rsa:2048", "-nodes",
                "-keyout", str(self.root / "unicode-root-key.pem"), "-out", str(root),
                "-days", "2", "-subj", "/CN=Zürich Root",
                "-addext", "basicConstraints=critical,CA:TRUE",
                "-addext", "keyUsage=critical,keyCertSign,cRLSign",
            ],
            check=True,
            capture_output=True,
        )
        write_artifacts(root, self.root / "unicode-site")
        page = (self.root / "unicode-site" / "index.html").read_text()
        self.assertIn("Zürich Root", page)

    def test_rejects_non_ca_before_creating_artifacts(self) -> None:
        leaf = self.root / "leaf.pem"
        subprocess.run(
            [
                "openssl", "req", "-x509", "-newkey", "rsa:2048", "-nodes",
                "-keyout", str(self.root / "leaf-key.pem"), "-out", str(leaf),
                "-days", "2", "-subj", "/CN=Not a CA",
                "-addext", "basicConstraints=critical,CA:FALSE",
            ],
            check=True,
            capture_output=True,
        )
        out = self.root / "rejected"
        with self.assertRaisesRegex(TrustPortalError, "not a CA"):
            write_artifacts(leaf, out)
        self.assertFalse(out.exists())

    def test_refuses_to_overwrite_existing_portal_artifacts(self) -> None:
        out = self.root / "site"
        out.mkdir()
        (out / "index.html").write_text("operator data")
        with self.assertRaisesRegex(TrustPortalError, "refusing to overwrite"):
            write_artifacts(self.root / "root.pem", out)
        self.assertEqual((out / "index.html").read_text(), "operator data")

    def test_rejects_intermediate_ca_as_trust_root(self) -> None:
        csr = self.root / "intermediate.csr"
        key = self.root / "intermediate-key.pem"
        intermediate = self.root / "intermediate.pem"
        extensions = self.root / "intermediate.ext"
        subprocess.run(
            [
                "openssl", "req", "-new", "-newkey", "rsa:2048", "-nodes",
                "-keyout", str(key), "-out", str(csr), "-subj", "/CN=Test Intermediate CA",
            ],
            check=True,
            capture_output=True,
        )
        extensions.write_text(
            "[v3_ca]\n"
            "basicConstraints=critical,CA:TRUE,pathlen:0\n"
            "keyUsage=critical,keyCertSign,cRLSign\n"
            "subjectKeyIdentifier=hash\n"
            "authorityKeyIdentifier=keyid,issuer\n"
        )
        subprocess.run(
            [
                "openssl", "x509", "-req", "-in", str(csr), "-CA", str(self.root / "root.pem"),
                "-CAkey", str(self.root / "private-test-key.pem"), "-CAcreateserial",
                "-out", str(intermediate), "-days", "2", "-extfile", str(extensions), "-extensions", "v3_ca",
            ],
            check=True,
            capture_output=True,
        )
        with self.assertRaisesRegex(TrustPortalError, "intermediate, not a self-issued root"):
            write_artifacts(intermediate, self.root / "intermediate-site")
        self.assertFalse((self.root / "intermediate-site").exists())

    def test_rejects_self_issued_ca_with_invalid_self_signature(self) -> None:
        key = self.root / "self-issued-key.pem"
        certificate = self.root / "self-issued.pem"
        tampered_der = self.root / "self-issued-tampered.der"
        tampered_certificate = self.root / "self-issued-tampered.pem"
        subprocess.run(
            [
                "openssl", "req", "-utf8", "-x509", "-newkey", "rsa:2048", "-nodes",
                "-keyout", str(key), "-out", str(certificate), "-days", "2",
                "-subj", "/O=Test Lab/CN=Self Issued Invalid Signature",
                "-addext", "basicConstraints=critical,CA:TRUE",
                "-addext", "keyUsage=critical,keyCertSign,cRLSign",
            ],
            check=True,
            capture_output=True,
        )
        original_der = subprocess.run(
            ["openssl", "x509", "-in", str(certificate), "-outform", "DER"],
            check=True, capture_output=True,
        ).stdout
        corrupted_der = bytearray(original_der)
        corrupted_der[-1] ^= 1  # Flip a signature bit without changing issuer/subject metadata.
        tampered_der.write_bytes(corrupted_der)
        subprocess.run(
            [
                "openssl", "x509", "-inform", "DER", "-in", str(tampered_der),
                "-out", str(tampered_certificate),
            ],
            check=True,
            capture_output=True,
        )
        subject = subprocess.run(
            ["openssl", "x509", "-in", str(tampered_certificate), "-noout", "-subject"],
            check=True, capture_output=True, text=True,
        ).stdout.strip().removeprefix("subject=")
        issuer = subprocess.run(
            ["openssl", "x509", "-in", str(tampered_certificate), "-noout", "-issuer"],
            check=True, capture_output=True, text=True,
        ).stdout.strip().removeprefix("issuer=")
        self.assertEqual(subject, issuer, "bad-signature fixture must remain self-issued")
        with self.assertRaisesRegex(TrustPortalError, "could not verify root self-signature"):
            write_artifacts(tampered_certificate, self.root / "bad-self-signature-site")
        self.assertFalse((self.root / "bad-self-signature-site").exists())

    def test_failed_atomic_publish_leaves_no_partial_output_or_stage(self) -> None:
        out = self.root / "failed-site"
        with patch("build_trust_portal.os.rename", side_effect=OSError("simulated publish failure")):
            with self.assertRaisesRegex(TrustPortalError, "cannot publish trust portal artifacts"):
                write_artifacts(self.root / "root.pem", out)
        self.assertFalse(out.exists())
        self.assertEqual(list(self.root.glob(".failed-site.pki-stage-*")), [])


if __name__ == "__main__":
    unittest.main()
