#!/usr/bin/env python3
"""Cryptographic and serialization checks for the certificate backfill tool."""
from __future__ import annotations

import pathlib
import subprocess
import sys
import tempfile
import unittest

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import backfill_certificate_issuance as backfill  # noqa: E402


class CertificateBackfillTests(unittest.TestCase):
    def setUp(self) -> None:
        self.temp = tempfile.TemporaryDirectory(prefix="solari-cert-backfill-test-")
        self.root = pathlib.Path(self.temp.name)
        self.caKey = self.root / "ca.key"
        self.caPem = self.root / "ca.pem"
        self.leafKey = self.root / "leaf.key"
        self.leafCsr = self.root / "leaf.csr"
        self.leafPem = self.root / "leaf.pem"
        subprocess.run([
            "openssl", "req", "-x509", "-newkey", "rsa:2048", "-nodes",
            "-keyout", str(self.caKey), "-out", str(self.caPem), "-days", "2",
            "-subj", "/CN=SolariNet Test CA", "-set_serial", "0x100",
            "-addext", "basicConstraints=critical,CA:TRUE",
            "-addext", "keyUsage=critical,keyCertSign,cRLSign",
        ], check=True, capture_output=True)
        subprocess.run([
            "openssl", "req", "-new", "-newkey", "rsa:2048", "-nodes",
            "-keyout", str(self.leafKey), "-out", str(self.leafCsr),
            "-subj", "/CN=client.example.test",
        ], check=True, capture_output=True)
        subprocess.run([
            "openssl", "x509", "-req", "-in", str(self.leafCsr), "-CA", str(self.caPem),
            "-CAkey", str(self.caKey), "-set_serial", "0x0102", "-days", "1",
            "-out", str(self.leafPem),
        ], check=True, capture_output=True)

    def tearDown(self) -> None:
        self.temp.cleanup()

    def test_leaf_is_bound_to_unique_issuer_and_normalized_identity(self) -> None:
        ca = self.caPem.read_bytes()
        cas = backfill.ca_fingerprints(ca)
        issuer, serial, fp, not_before = backfill.inspect_leaf(self.leafPem.read_bytes(), cas)
        self.assertEqual(next(iter(cas)), issuer)
        self.assertEqual("102", serial)
        self.assertEqual(64, len(fp))
        self.assertGreater(not_before, 0)

    def test_ambiguous_same_key_ca_certificates_fail_closed(self) -> None:
        second_ca = self.root / "ca-second.pem"
        subprocess.run([
            "openssl", "req", "-x509", "-key", str(self.caKey), "-out", str(second_ca),
            "-days", "2", "-subj", "/CN=SolariNet Test CA", "-set_serial", "0x101",
            "-addext", "basicConstraints=critical,CA:TRUE",
            "-addext", "keyUsage=critical,keyCertSign,cRLSign",
        ], check=True, capture_output=True)
        bundle = self.caPem.read_bytes() + second_ca.read_bytes()
        with self.assertRaisesRegex(ValueError, "2 matching configured issuers"):
            backfill.inspect_leaf(self.leafPem.read_bytes(), backfill.ca_fingerprints(bundle))

    def test_wrong_issuer_fails_closed(self) -> None:
        other_key = self.root / "other.key"
        other_ca = self.root / "other.pem"
        subprocess.run([
            "openssl", "req", "-x509", "-newkey", "rsa:2048", "-nodes",
            "-keyout", str(other_key), "-out", str(other_ca), "-days", "2",
            "-subj", "/CN=Unrelated CA", "-addext", "basicConstraints=critical,CA:TRUE",
            "-addext", "keyUsage=critical,keyCertSign,cRLSign",
        ], check=True, capture_output=True)
        with self.assertRaisesRegex(ValueError, "0 matching configured issuers"):
            backfill.inspect_leaf(self.leafPem.read_bytes(), backfill.ca_fingerprints(other_ca.read_bytes()))

    def test_sql_text_is_encoded_without_interpolation(self) -> None:
        value = "operator 'quoted'\\value"
        encoded = backfill.sql_string(value)
        self.assertNotIn(value, encoded)
        self.assertIn(value.encode().hex(), encoded)


if __name__ == "__main__":
    unittest.main()
