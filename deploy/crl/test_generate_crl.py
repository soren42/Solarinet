#!/usr/bin/env python3
"""OpenSSL-backed unit checks for safe CRL assembly and publication."""
from __future__ import annotations

import datetime
import hashlib
import os
import pathlib
import re
import subprocess
import sys
import tempfile
import unittest
from types import SimpleNamespace
from unittest.mock import patch

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import generate_crl as generator  # noqa: E402


class CrlGenerationTests(unittest.TestCase):
    def setUp(self) -> None:
        self.temp = tempfile.TemporaryDirectory(prefix="solari-crl-test-")
        self.root = pathlib.Path(self.temp.name)
        self.caKey = self.root / "ca.key"
        self.caCert = self.root / "ca.pem"
        self.output = self.root / "revocations.pem"
        subprocess.run([
            "openssl", "req", "-x509", "-newkey", "rsa:2048", "-nodes",
            "-keyout", str(self.caKey), "-out", str(self.caCert), "-days", "2",
            "-subj", "/CN=SolariNet CRL Test CA", "-set_serial", "0x10",
            "-addext", "basicConstraints=critical,CA:TRUE",
            "-addext", "keyUsage=critical,keyCertSign,cRLSign",
            "-addext", "subjectKeyIdentifier=hash",
        ], check=True, capture_output=True)
        self.args = SimpleNamespace(
            ca_cert=str(self.caCert), ca_key=str(self.caKey), output=str(self.output),
            crl_hours=24, database="crl_test", defaults_file=None,
            host=None, port=None, user=None,
        )

    def tearDown(self) -> None:
        self.temp.cleanup()

    @staticmethod
    def _mysql_for(rows, number, updated_rows=1):
        def fake_mysql(_args, sql):
            if "SELECT complete FROM certificateInventoryState" in sql:
                return [["1"]]
            if "FROM certificateRevocation WHERE issuerFp=" in sql:
                return rows
            if sql.startswith("SELECT DISTINCT issuerFp FROM certificateRevocation"):
                return []
            if sql.startswith("INSERT IGNORE INTO certificateCrlState"):
                return [[str(number)]]
            if "UPDATE certificateCrlState SET publishedNumber" in sql:
                return [[str(updated_rows)]]
            raise AssertionError("unexpected SQL in CRL generation")
        return fake_mysql

    def test_generated_crl_has_valid_signature_number_reason_and_serials(self) -> None:
        revoked_at = int(datetime.datetime.now(datetime.timezone.utc).timestamp() * 1000)
        rows = [["A01", "keyCompromise", str(revoked_at)],
                ["A02", "superseded", str(revoked_at)],
                ["A03", "unspecified", str(revoked_at)]]
        calls = []

        def fake_mysql(_args, sql):
            calls.append(sql)
            if "SELECT complete FROM certificateInventoryState" in sql:
                return [["1"]]
            if "FROM certificateRevocation WHERE issuerFp=" in sql:
                return rows
            if sql.startswith("SELECT DISTINCT issuerFp FROM certificateRevocation"):
                return []
            if sql.startswith("INSERT IGNORE INTO certificateCrlState"):
                return [["7"]]
            if "UPDATE certificateCrlState SET publishedNumber" in sql:
                return [["1"]]
            raise AssertionError("unexpected SQL in CRL generation")

        previous_umask = os.umask(0o077)
        try:
            with patch.object(generator, "mysql", side_effect=fake_mysql):
                generator.generate(self.args)
        finally:
            os.umask(previous_umask)
        self.assertEqual(0o644, self.output.stat().st_mode & 0o777)

        number, last_update, next_update, serials, der = generator.parse_crl_metadata(self.output)
        self.assertEqual(7, number)
        self.assertEqual({"A01", "A02", "A03"}, serials)
        now = datetime.datetime.now(datetime.timezone.utc)
        self.assertGreater(next_update, now)
        self.assertLessEqual(last_update, now + datetime.timedelta(minutes=2))
        self.assertGreaterEqual(last_update, now - datetime.timedelta(minutes=5))
        self.assertEqual(64, len(hashlib.sha256(der).hexdigest()))
        crl_text = subprocess.run(["openssl", "crl", "-in", str(self.output),
                                   "-noout", "-text"], check=True,
                                  capture_output=True, text=True).stdout
        self.assertIn("X509v3 CRL Reason Code", crl_text)
        self.assertIn("X509v3 Authority Key Identifier", crl_text)
        self.assertIn("Key Compromise", crl_text)
        self.assertIn("Superseded", crl_text)
        revoked_entries = crl_text.split("Revoked Certificates:", 1)[1]
        entries = re.split(r"(?=\s*Serial Number:)", revoked_entries)
        unspecified_entry = next(entry for entry in entries
                                 if re.search(r"Serial Number: (?:0)?A0?3\b", entry))
        self.assertNotIn("X509v3 CRL Reason Code", unspecified_entry)
        self.assertTrue(any("publishedNumber=7" in sql for sql in calls))
        state_sql = next(sql for sql in calls if "UPDATE certificateCrlState SET publishedNumber" in sql)
        self.assertIn("SET time_zone='+00:00';", state_sql)
        self.assertTrue(any(sql.startswith("SET time_zone='+00:00'; SELECT complete")
                            for sql in calls))
        self.assertTrue(any(sql.startswith("SET time_zone='+00:00'; SELECT serialHex")
                            for sql in calls))

    def test_bad_ledger_row_leaves_previous_crl_unchanged(self) -> None:
        revoked_at = int(datetime.datetime.now(datetime.timezone.utc).timestamp() * 1000)
        def make_mysql(rows, number):
            def fake_mysql(_args, sql):
                if "SELECT complete FROM certificateInventoryState" in sql:
                    return [["1"]]
                if "FROM certificateRevocation WHERE issuerFp=" in sql:
                    return rows
                if sql.startswith("SELECT DISTINCT issuerFp FROM certificateRevocation"):
                    return []
                if sql.startswith("INSERT IGNORE INTO certificateCrlState"):
                    return [[str(number)]]
                if "UPDATE certificateCrlState SET publishedNumber" in sql:
                    return [["1"]]
                raise AssertionError("unexpected SQL in CRL generation")
            return fake_mysql

        with patch.object(generator, "mysql", side_effect=make_mysql(
                [["A01", "keyCompromise", str(revoked_at)]], 1)):
            generator.generate(self.args)
        before = self.output.read_bytes()
        calls = []

        def fake_mysql(_args, sql):
            calls.append(sql)
            if "SELECT complete FROM certificateInventoryState" in sql:
                return [["1"]]
            if "FROM certificateRevocation WHERE issuerFp=" in sql:
                return [["A01", "unknown-reason", str(revoked_at)]]
            if sql.startswith("SELECT DISTINCT issuerFp FROM certificateRevocation"):
                return []
            if sql.startswith("INSERT IGNORE INTO certificateCrlState"):
                return [["2"]]
            raise AssertionError("publication state must not advance after invalid input")

        with patch.object(generator, "mysql", side_effect=fake_mysql), \
                self.assertRaisesRegex(ValueError, "malformed serial, reason, or time"):
            generator.generate(self.args)
        self.assertEqual(before, self.output.read_bytes())
        self.assertFalse(any("publishedNumber=" in sql for sql in calls))

    def test_unspecified_revocation_reason_has_no_reason_code_extension(self) -> None:
        now = datetime.datetime.now(datetime.timezone.utc)
        index = generator.crl_index([("A01", "unspecified", int(now.timestamp() * 1000)),
                                     ("A02", "keyCompromise", int(now.timestamp() * 1000))],
                                    now).decode("ascii")
        self.assertNotIn(",unspecified", index)
        self.assertIn(",keyCompromise", index)

    def test_existing_crl_number_cannot_be_rolled_back(self) -> None:
        revoked_at = int(datetime.datetime.now(datetime.timezone.utc).timestamp() * 1000)
        rows = [["A01", "keyCompromise", str(revoked_at)]]
        with patch.object(generator, "mysql", side_effect=self._mysql_for(rows, 1)):
            generator.generate(self.args)
        before = self.output.read_bytes()

        with patch.object(generator, "mysql", side_effect=self._mysql_for(rows, 1)), \
                self.assertRaisesRegex(ValueError, "roll back the existing publication"):
            generator.generate(self.args)
        self.assertEqual(before, self.output.read_bytes())

    def test_ledger_change_during_generation_preserves_active_crl(self) -> None:
        revoked_at = int(datetime.datetime.now(datetime.timezone.utc).timestamp() * 1000)
        rows = [["A01", "keyCompromise", str(revoked_at)]]
        with patch.object(generator, "mysql", side_effect=self._mysql_for(rows, 1)):
            generator.generate(self.args)
        before = self.output.read_bytes()
        changed_rows = rows + [["A02", "superseded", str(revoked_at)]]
        revocation_reads = 0

        def changing_ledger(_args, sql):
            nonlocal revocation_reads
            if "SELECT complete FROM certificateInventoryState" in sql:
                return [["1"]]
            if "FROM certificateRevocation WHERE issuerFp=" in sql:
                revocation_reads += 1
                return rows if revocation_reads == 1 else changed_rows
            if sql.startswith("SELECT DISTINCT issuerFp FROM certificateRevocation"):
                return []
            if sql.startswith("INSERT IGNORE INTO certificateCrlState"):
                return [["2"]]
            raise AssertionError("publication state must not advance after ledger drift")

        with patch.object(generator, "mysql", side_effect=changing_ledger), \
                self.assertRaisesRegex(ValueError, "ledger changed during CRL generation"):
            generator.generate(self.args)
        self.assertEqual(before, self.output.read_bytes())

    def test_failed_state_commit_is_reported_after_crl_publish(self) -> None:
        revoked_at = int(datetime.datetime.now(datetime.timezone.utc).timestamp() * 1000)
        rows = [["A01", "keyCompromise", str(revoked_at)]]
        with patch.object(generator, "mysql", side_effect=self._mysql_for(rows, 1)):
            generator.generate(self.args)
        before = self.output.read_bytes()

        with patch.object(generator, "mysql", side_effect=self._mysql_for(rows, 2, updated_rows=0)), \
                self.assertRaisesRegex(ValueError, "could not be committed monotonically"):
            generator.generate(self.args)
        self.assertNotEqual(before, self.output.read_bytes())
        self.assertEqual(2, generator.parse_crl_metadata(self.output)[0])

    def test_candidate_rejects_future_this_update(self) -> None:
        ca_dir = self.root / "future-this-update"
        ca_dir.mkdir()
        (ca_dir / "newcerts").mkdir()
        (ca_dir / "index.txt").write_text("", encoding="ascii")
        (ca_dir / "serial").write_text("01\n", encoding="ascii")
        (ca_dir / "crlnumber").write_text("01\n", encoding="ascii")
        config = ca_dir / "ca.cnf"
        config.write_text(
            "[ ca ]\ndefault_ca = issuer\n[ issuer ]\n"
            f"dir = {ca_dir}\ndatabase = $dir/index.txt\nnew_certs_dir = $dir/newcerts\n"
            f"certificate = {self.caCert}\nprivate_key = {self.caKey}\n"
            "serial = $dir/serial\ncrlnumber = $dir/crlnumber\n"
            "crl_extensions = crl_ext\ndefault_md = sha256\npolicy = policy\n"
            "[ policy ]\ncommonName = supplied\n"
            "[ crl_ext ]\nauthorityKeyIdentifier = keyid:always\n",
            encoding="ascii")
        future = datetime.datetime.now(datetime.timezone.utc) + datetime.timedelta(days=1)
        later = future + datetime.timedelta(days=1)
        candidate = ca_dir / "future.crl.pem"
        subprocess.run([
            "openssl", "ca", "-batch", "-config", str(config), "-gencrl",
            "-crl_lastupdate", future.strftime("%y%m%d%H%M%SZ"),
            "-crl_nextupdate", later.strftime("%y%m%d%H%M%SZ"),
            "-out", str(candidate),
        ], check=True, capture_output=True)
        with self.assertRaisesRegex(ValueError, "thisUpdate"):
            generator.verify_candidate(candidate, self.caCert, 1, [], 24)

    def test_candidate_rejects_wrong_issuer_signature(self) -> None:
        other_key = self.root / "other-ca.key"
        other_cert = self.root / "other-ca.pem"
        subprocess.run([
            "openssl", "req", "-x509", "-newkey", "rsa:2048", "-nodes",
            "-keyout", str(other_key), "-out", str(other_cert), "-days", "2",
            "-subj", "/CN=Unrelated CRL Test CA",
            "-addext", "basicConstraints=critical,CA:TRUE",
            "-addext", "keyUsage=critical,keyCertSign,cRLSign",
        ], check=True, capture_output=True)
        ca_dir = self.root / "other-issuer"
        ca_dir.mkdir()
        (ca_dir / "newcerts").mkdir()
        (ca_dir / "index.txt").write_text("", encoding="ascii")
        (ca_dir / "serial").write_text("01\n", encoding="ascii")
        (ca_dir / "crlnumber").write_text("01\n", encoding="ascii")
        config = ca_dir / "ca.cnf"
        config.write_text(
            "[ ca ]\ndefault_ca = issuer\n[ issuer ]\n"
            f"dir = {ca_dir}\ndatabase = $dir/index.txt\nnew_certs_dir = $dir/newcerts\n"
            f"certificate = {other_cert}\nprivate_key = {other_key}\n"
            "serial = $dir/serial\ncrlnumber = $dir/crlnumber\n"
            "default_md = sha256\ndefault_crl_days = 1\n"
            "policy = policy\n[ policy ]\ncommonName = supplied\n",
            encoding="ascii")
        candidate = ca_dir / "other.crl.pem"
        subprocess.run(["openssl", "ca", "-batch", "-config", str(config),
                        "-gencrl", "-out", str(candidate)],
                       check=True, capture_output=True)
        with self.assertRaisesRegex(ValueError, "openssl failed"):
            generator.verify_candidate(candidate, self.caCert, 1, [], 24)

    def test_incomplete_inventory_refuses_before_allocation(self) -> None:
        calls = []

        def fake_mysql(_args, sql):
            calls.append(sql)
            return [["0"]]

        with patch.object(generator, "mysql", side_effect=fake_mysql), \
                self.assertRaisesRegex(ValueError, "inventory .* incomplete"):
            generator.generate(self.args)
        self.assertEqual(1, len(calls))


if __name__ == "__main__":
    unittest.main()
