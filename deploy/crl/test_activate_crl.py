#!/usr/bin/env python3
"""Local cryptographic and failure-atomicity checks for activate_crl.py."""
from __future__ import annotations

import base64
import datetime as dt
import http.server
import pathlib
import stat
import ssl
import subprocess
import sys
import threading
import tempfile
import time
import urllib.error
import unittest
from unittest.mock import patch


def corrupt_crl_signature(crl_pem: bytes) -> bytes:
    """Flip one signature octet while preserving the DER/PEM structure."""
    begin = b"-----BEGIN X509 CRL-----"
    end = b"-----END X509 CRL-----"
    body = crl_pem.split(begin, 1)[1].split(end, 1)[0]
    der = bytearray(base64.b64decode(body))

    def tlv(offset: int) -> tuple[int, int, int]:
        tag = der[offset]
        first = der[offset + 1]
        if first < 0x80:
            length, value_start = first, offset + 2
        else:
            count = first & 0x7F
            length = int.from_bytes(der[offset + 2:offset + 2 + count], "big")
            value_start = offset + 2 + count
        return tag, value_start, value_start + length

    outer_tag, outer_start, outer_end = tlv(0)
    if outer_tag != 0x30 or outer_end != len(der):
        raise AssertionError("generated CRL has unexpected outer DER structure")
    _, _, tbs_end = tlv(outer_start)
    _, _, algorithm_end = tlv(tbs_end)
    signature_tag, signature_start, signature_end = tlv(algorithm_end)
    if signature_tag != 0x03 or signature_end != outer_end or signature_end - signature_start < 2:
        raise AssertionError("generated CRL has unexpected signature DER structure")
    der[signature_end - 1] ^= 0x01
    encoded = base64.b64encode(der).decode("ascii")
    lines = "\n".join(encoded[index:index + 64] for index in range(0, len(encoded), 64))
    return begin + b"\n" + lines.encode("ascii") + b"\n" + end + b"\n"

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import activate_crl as activator  # noqa: E402


class ActivateCrlTests(unittest.TestCase):
    def setUp(self) -> None:
        self.temp = tempfile.TemporaryDirectory(prefix="solari-crl-activate-")
        self.root = pathlib.Path(self.temp.name)
        self.ca_dir = self.root / "ca"
        self.ca_dir.mkdir()
        self.ca_key = self.ca_dir / "ca.key"
        self.ca_cert = self.ca_dir / "ca.pem"
        self.base_bundle = self.root / "base.pem"
        self.active = self.root / "active.pem"
        subprocess.run([
            "openssl", "req", "-x509", "-newkey", "rsa:2048", "-nodes",
            "-keyout", str(self.ca_key), "-out", str(self.ca_cert), "-days", "5",
            "-subj", "/CN=SolariNet activation test CA", "-set_serial", "0x50",
            "-addext", "basicConstraints=critical,CA:TRUE",
            "-addext", "keyUsage=critical,keyCertSign,cRLSign",
        ], check=True, capture_output=True)
        self.base_bundle.write_bytes(self.ca_cert.read_bytes())
        (self.ca_dir / "newcerts").mkdir()
        (self.ca_dir / "index.txt").write_text("", encoding="ascii")
        (self.ca_dir / "serial").write_text("01\n", encoding="ascii")
        self.config = self.ca_dir / "ca.cnf"
        self.config.write_text(
            "[ ca ]\ndefault_ca = issuer\n[ issuer ]\n"
            f"dir = {self.ca_dir}\ndatabase = $dir/index.txt\n"
            "new_certs_dir = $dir/newcerts\n"
            f"certificate = {self.ca_cert}\nprivate_key = {self.ca_key}\n"
            "serial = $dir/serial\ncrlnumber = $dir/crlnumber\n"
            "default_md = sha256\ndefault_crl_hours = 24\n"
            "policy = policy\nunique_subject = no\n"
            "[ policy ]\ncommonName = supplied\n", encoding="ascii")

    def tearDown(self) -> None:
        self.temp.cleanup()

    def make_crl(self, number: int, *, expired: bool = False,
                 last_update: dt.datetime | None = None,
                 ca_cert: pathlib.Path | None = None,
                 ca_key: pathlib.Path | None = None,
                 config: pathlib.Path | None = None) -> bytes:
        number_file = self.ca_dir / f"crlnumber-{number}"
        number_hex = format(number, "X"); number_hex = ("0" + number_hex) if len(number_hex) % 2 else number_hex
        number_file.write_text(number_hex + "\n", encoding="ascii")
        selected_config = config or self.config
        text = selected_config.read_text(encoding="ascii")
        if "crlnumber = $dir/crlnumber" in text:
            text = text.replace("crlnumber = $dir/crlnumber", f"crlnumber = {number_file}")
        else:
            text = text.replace("crlnumber = $dir/other-crlnumber",
                                f"crlnumber = {number_file}")
        temp_config = self.ca_dir / f"ca-{number}-{id(self)}.cnf"
        temp_config.write_text(text, encoding="ascii")
        output = self.ca_dir / f"number-{number}-{id(self)}.crl.pem"
        args = ["openssl", "ca", "-batch", "-config", str(temp_config),
                "-gencrl", "-out", str(output)]
        if expired:
            last = dt.datetime.now(dt.timezone.utc) - dt.timedelta(days=3)
            next_ = last + dt.timedelta(days=1)
            args += ["-crl_lastupdate", last.strftime("%y%m%d%H%M%SZ"),
                     "-crl_nextupdate", next_.strftime("%y%m%d%H%M%SZ")]
        elif last_update is not None:
            next_ = last_update + dt.timedelta(days=1)
            args += ["-crl_lastupdate", last_update.strftime("%y%m%d%H%M%SZ"),
                     "-crl_nextupdate", next_.strftime("%y%m%d%H%M%SZ")]
        subprocess.run(args, check=True, capture_output=True)
        return output.read_bytes()

    def activate_bytes(self, crl_bytes: bytes) -> bool:
        with patch.object(activator, "fetch_crl", return_value=crl_bytes), \
                patch.object(activator, "restart_service") as restart:
            changed = activator.activate(
                "https://crl.example.test/current.pem", self.base_bundle,
                self.ca_cert, self.active, "solari-client.service")
            self.restart_mock = restart
        return changed

    def test_valid_newer_crl_is_atomically_installed_then_service_restarted(self) -> None:
        crl = self.make_crl(7)
        self.assertTrue(self.activate_bytes(crl))
        self.restart_mock.assert_called_once_with("solari-client.service")
        installed = self.active.read_bytes()
        self.assertIn(self.ca_cert.read_bytes().strip(), installed)
        self.assertIn(b"-----BEGIN X509 CRL-----", installed)
        self.assertEqual(7, activator.crl_metadata(self.active)[0])
        receipt = pathlib.Path(str(self.active) + ".activated")
        self.assertEqual(0o644, stat.S_IMODE(receipt.stat().st_mode))
        self.assertRegex(receipt.read_text(encoding="ascii"),
                         r"^[0-9a-f]{64}:7:[0-9a-f]{64}\n$")

    def test_seeded_base_only_bundle_activates_without_prior_receipt(self) -> None:
        self.active.write_bytes(self.base_bundle.read_bytes())
        marker = pathlib.Path(str(self.active) + ".activated")
        self.assertEqual([], activator.CRL_RE.findall(self.active.read_bytes()))
        self.assertFalse(marker.exists())

        self.assertTrue(self.activate_bytes(self.make_crl(8)))
        self.restart_mock.assert_called_once_with("solari-client.service")
        self.assertEqual(8, activator.crl_metadata(self.active)[0])
        self.assertTrue(marker.is_file())

    def test_receipt_write_failure_reports_active_state_and_reconciles_on_retry(self) -> None:
        crl = self.make_crl(9)
        marker = pathlib.Path(str(self.active) + ".activated")
        original_write = activator.atomic_write
        calls = 0

        def fail_receipt(path: pathlib.Path, *args: object, **kwargs: object) -> None:
            nonlocal calls
            calls += 1
            if calls == 2:
                raise OSError("injected receipt write failure")
            original_write(path, *args, **kwargs)  # type: ignore[arg-type]

        with patch.object(activator, "fetch_crl", return_value=crl), \
                patch.object(activator, "atomic_write", side_effect=fail_receipt), \
                patch.object(activator, "restart_service") as restart, \
                self.assertRaisesRegex(RuntimeError, "is active and solari-client.service restarted"):
            activator.activate("https://crl.example.test/current.pem", self.base_bundle,
                               self.ca_cert, self.active, "solari-client.service")
        self.assertEqual(9, activator.crl_metadata(self.active)[0])
        self.assertFalse(marker.exists())
        restart.assert_called_once_with("solari-client.service")

        with patch.object(activator, "fetch_crl", return_value=crl), \
                patch.object(activator, "restart_service") as retry_restart:
            self.assertTrue(activator.activate("https://crl.example.test/current.pem",
                self.base_bundle, self.ca_cert, self.active, "solari-client.service"))
        retry_restart.assert_called_once_with("solari-client.service")
        self.assertTrue(marker.is_file())

    def test_bad_signature_and_expired_crl_leave_active_bundle_unchanged(self) -> None:
        good = self.make_crl(3)
        self.activate_bytes(good)
        before = self.active.read_bytes()
        other_dir = self.root / "other"
        other_dir.mkdir()
        other_key, other_cert = other_dir / "ca.key", other_dir / "ca.pem"
        subprocess.run([
            "openssl", "req", "-x509", "-newkey", "rsa:2048", "-nodes",
            "-keyout", str(other_key), "-out", str(other_cert), "-days", "5",
            "-subj", "/CN=SolariNet activation test CA",
            "-addext", "basicConstraints=critical,CA:TRUE",
            "-addext", "keyUsage=critical,keyCertSign,cRLSign",
        ], check=True, capture_output=True)
        other_public_key = subprocess.run(
            ["openssl", "x509", "-in", str(other_cert), "-pubkey", "-noout"],
            check=True, capture_output=True).stdout
        configured_public_key = subprocess.run(
            ["openssl", "x509", "-in", str(self.ca_cert), "-pubkey", "-noout"],
            check=True, capture_output=True).stdout
        self.assertNotEqual(other_public_key, configured_public_key)
        other = other_dir / "other.cnf"
        other.write_text(
            "[ ca ]\ndefault_ca = issuer\n[ issuer ]\n"
            f"dir = {other_dir}\ndatabase = {other_dir / 'index.txt'}\n"
            f"new_certs_dir = {other_dir / 'newcerts'}\n"
            f"certificate = {other_cert}\nprivate_key = {other_key}\n"
            f"serial = {other_dir / 'serial'}\ncrlnumber = {other_dir / 'other-crlnumber'}\n"
            "default_md = sha256\ndefault_crl_hours = 24\n"
            "policy = policy\nunique_subject = no\n"
            "[ policy ]\ncommonName = supplied\n", encoding="ascii")
        (other_dir / "newcerts").mkdir()
        (other_dir / "index.txt").write_text("", encoding="ascii")
        (other_dir / "serial").write_text("01\n", encoding="ascii")
        (other_dir / "other-crlnumber").write_text("09\n", encoding="ascii")
        wrong_signed = self.make_crl(9, config=other)
        wrong_path = other_dir / "wrong-signer.crl.pem"
        wrong_path.write_bytes(wrong_signed)
        verifies_with_other = subprocess.run(
            ["openssl", "crl", "-in", str(wrong_path), "-noout", "-verify",
             "-CAfile", str(other_cert)], capture_output=True)
        rejects_with_configured = subprocess.run(
            ["openssl", "crl", "-in", str(wrong_path), "-noout", "-verify",
             "-CAfile", str(self.ca_cert)], capture_output=True)
        self.assertEqual(verifies_with_other.returncode, 0, "fixture CRL must verify with its signer")
        self.assertNotEqual(rejects_with_configured.returncode, 0,
                            "fixture CRL must not verify with the configured issuer")
        with patch.object(activator, "fetch_crl", return_value=wrong_signed), \
                patch.object(activator, "restart_service") as restart, \
                self.assertRaisesRegex(ValueError, "openssl failed"):
            activator.activate("https://crl.example.test/current.pem", self.base_bundle,
                               self.ca_cert, self.active, "solari-client.service")
        restart.assert_not_called()
        wrong = corrupt_crl_signature(wrong_signed)
        with patch.object(activator, "fetch_crl", return_value=wrong), \
                patch.object(activator, "restart_service") as restart, \
                self.assertRaisesRegex(ValueError, "openssl failed"):
            activator.activate("https://crl.example.test/current.pem", self.base_bundle,
                               self.ca_cert, self.active, "solari-client.service")
        restart.assert_not_called()
        expired = self.make_crl(8, expired=True)
        with patch.object(activator, "fetch_crl", return_value=expired), \
                patch.object(activator, "restart_service") as restart, \
                self.assertRaisesRegex(ValueError, "expired or too close"):
            activator.activate("https://crl.example.test/current.pem", self.base_bundle,
                               self.ca_cert, self.active, "solari-client.service")
        restart.assert_not_called()
        self.assertEqual(before, self.active.read_bytes())

    def test_crl_number_rollback_and_unknown_restart_unit_are_rejected(self) -> None:
        self.activate_bytes(self.make_crl(8))
        before = self.active.read_bytes()
        with patch.object(activator, "fetch_crl", return_value=self.make_crl(7)), \
                patch.object(activator, "restart_service") as restart, \
                self.assertRaisesRegex(ValueError, "rollback"):
            activator.activate("https://crl.example.test/current.pem", self.base_bundle,
                               self.ca_cert, self.active, "solari-client.service")
        restart.assert_not_called()
        with patch.object(activator, "fetch_crl") as fetch, \
                self.assertRaisesRegex(ValueError, "allowlist"):
            activator.activate("https://crl.example.test/current.pem", self.base_bundle,
                               self.ca_cert, self.active, "sshd.service")
        fetch.assert_not_called()
        self.assertEqual(before, self.active.read_bytes())

    def test_conflicting_same_number_crl_is_rejected(self) -> None:
        first = self.make_crl(37)
        self.activate_bytes(first)
        before = self.active.read_bytes()
        conflicting = self.make_crl(
            37, last_update=dt.datetime.now(dt.timezone.utc) - dt.timedelta(minutes=2))
        self.assertNotEqual(first, conflicting)
        with patch.object(activator, "fetch_crl", return_value=conflicting), \
                patch.object(activator, "restart_service") as restart, \
                self.assertRaisesRegex(ValueError, "conflicting same-number"):
            activator.activate("https://crl.example.test/current.pem", self.base_bundle,
                               self.ca_cert, self.active, "solari-client.service")
        restart.assert_not_called()
        self.assertEqual(before, self.active.read_bytes())

    def test_restart_failure_restores_prior_bundle_and_restarts_old_state(self) -> None:
        self.activate_bytes(self.make_crl(2))
        before = self.active.read_bytes()
        before_marker = pathlib.Path(str(self.active) + ".activated").read_bytes()
        with patch.object(activator, "fetch_crl", return_value=self.make_crl(3)), \
                patch.object(activator, "restart_service",
                             side_effect=[subprocess.CalledProcessError(1, "systemctl"), None]) as restart, \
                self.assertRaisesRegex(RuntimeError, "previous bundle restored"):
            activator.activate("https://crl.example.test/current.pem", self.base_bundle,
                               self.ca_cert, self.active, "solari-client.service")
        self.assertEqual(before, self.active.read_bytes())
        self.assertEqual(before_marker, pathlib.Path(str(self.active) + ".activated").read_bytes())
        self.assertEqual(2, restart.call_count)

    def test_restart_timeout_restores_prior_bundle_and_restarts_old_state(self) -> None:
        self.activate_bytes(self.make_crl(26))
        before = self.active.read_bytes()
        with patch.object(activator, "fetch_crl", return_value=self.make_crl(27)), \
                patch.object(activator, "restart_service",
                             side_effect=[TimeoutError("restart deadline"), None]) as restart, \
                self.assertRaisesRegex(RuntimeError, "previous bundle restored"):
            activator.activate("https://crl.example.test/current.pem", self.base_bundle,
                               self.ca_cert, self.active, "solari-client.service")
        self.assertEqual(before, self.active.read_bytes())
        self.assertEqual(2, restart.call_count)

    def test_timeout_after_bundle_replace_restores_before_service_restart(self) -> None:
        self.activate_bytes(self.make_crl(28))
        before = self.active.read_bytes()
        original_write = activator.atomic_write
        calls = 0

        def replace_then_timeout(*args: object, **kwargs: object) -> None:
            nonlocal calls
            calls += 1
            original_write(*args, **kwargs)  # type: ignore[arg-type]
            if calls == 1:
                raise TimeoutError("injected deadline after atomic replacement")

        with patch.object(activator, "fetch_crl", return_value=self.make_crl(29)), \
                patch.object(activator, "atomic_write", side_effect=replace_then_timeout), \
                patch.object(activator, "restart_service") as restart, \
                self.assertRaisesRegex(RuntimeError, "previous bundle restored"):
            activator.activate("https://crl.example.test/current.pem", self.base_bundle,
                               self.ca_cert, self.active, "solari-client.service")
        self.assertEqual(before, self.active.read_bytes())
        restart.assert_called_once_with("solari-client.service")

    def test_response_with_trailing_certificate_is_rejected_before_activation(self) -> None:
        self.activate_bytes(self.make_crl(21))
        before = self.active.read_bytes()
        payload = self.make_crl(22) + self.ca_cert.read_bytes()
        with patch.object(activator, "fetch_crl", return_value=payload), \
                patch.object(activator, "restart_service") as restart, \
                self.assertRaisesRegex(ValueError, "exactly one PEM CRL"):
            activator.activate("https://crl.example.test/current.pem", self.base_bundle,
                               self.ca_cert, self.active, "solari-client.service")
        restart.assert_not_called()
        self.assertEqual(before, self.active.read_bytes())

    def test_initial_atomic_write_failure_leaves_bundle_and_service_untouched(self) -> None:
        marker = pathlib.Path(str(self.active) + ".activated")
        self.assertFalse(self.active.exists())
        with patch.object(activator, "fetch_crl", return_value=self.make_crl(23)), \
                patch.object(activator.os, "fchmod", side_effect=OSError("injected disk failure")), \
                patch.object(activator, "restart_service") as restart, \
                self.assertRaisesRegex(RuntimeError, "before active bundle replacement"):
            activator.activate("https://crl.example.test/current.pem", self.base_bundle,
                               self.ca_cert, self.active, "solari-client.service")
        self.assertFalse(self.active.exists())
        self.assertFalse(marker.exists())
        restart.assert_not_called()

    def test_failed_bundle_rollback_is_reported_as_manual_recovery(self) -> None:
        self.activate_bytes(self.make_crl(23))
        original_write = activator.atomic_write
        calls = 0

        def fail_rollback(*args: object, **kwargs: object) -> None:
            nonlocal calls
            calls += 1
            if calls == 2:
                raise OSError("simulated full filesystem")
            original_write(*args, **kwargs)  # type: ignore[arg-type]

        with patch.object(activator, "fetch_crl", return_value=self.make_crl(24)), \
                patch.object(activator, "atomic_write", side_effect=fail_rollback), \
                patch.object(activator, "restart_service",
                             side_effect=subprocess.CalledProcessError(1, "systemctl")), \
                self.assertRaisesRegex(RuntimeError, "manual recovery"):
            activator.activate("https://crl.example.test/current.pem", self.base_bundle,
                               self.ca_cert, self.active, "solari-client.service")
        self.assertEqual(2, calls)

    def test_total_operation_deadline_aborts_before_bundle_change(self) -> None:
        before = self.active.read_bytes() if self.active.exists() else None

        def slow_fetch(*_args: object, **_kwargs: object) -> bytes:
            time.sleep(0.2)
            return self.make_crl(25)

        with patch.object(activator, "ACTIVATION_BUDGET_SECONDS", 0.05), \
                patch.object(activator, "fetch_crl", side_effect=slow_fetch), \
                patch.object(activator, "restart_service") as restart, \
                self.assertRaisesRegex(TimeoutError, "120-second operation budget"):
            activator.activate("https://crl.example.test/current.pem", self.base_bundle,
                               self.ca_cert, self.active, "solari-client.service")
        restart.assert_not_called()
        self.assertEqual(before, self.active.read_bytes() if self.active.exists() else None)

    def test_exact_same_generation_is_idempotent(self) -> None:
        crl = self.make_crl(12)
        self.activate_bytes(crl)
        with patch.object(activator, "fetch_crl", return_value=crl), \
                patch.object(activator, "restart_service") as restart:
            self.assertFalse(activator.activate(
                "https://crl.example.test/current.pem", self.base_bundle,
                self.ca_cert, self.active, "solari-client.service"))
        restart.assert_not_called()

    def test_same_crl_without_activation_marker_reconciles_service_after_interruption(self) -> None:
        crl = self.make_crl(16)
        self.activate_bytes(crl)
        pathlib.Path(str(self.active) + ".activated").unlink()
        with patch.object(activator, "fetch_crl", return_value=crl), \
                patch.object(activator, "restart_service") as restart:
            self.assertTrue(activator.activate(
                "https://crl.example.test/current.pem", self.base_bundle,
                self.ca_cert, self.active, "solari-client.service"))
        restart.assert_called_once_with("solari-client.service")
        self.assertTrue(pathlib.Path(str(self.active) + ".activated").is_file())

    def test_reconciliation_repairs_private_receipt_without_restarting_service(self) -> None:
        crl = self.make_crl(28)
        self.activate_bytes(crl)
        receipt = pathlib.Path(str(self.active) + ".activated")
        receipt.chmod(0o600)
        with patch.object(activator, "fetch_crl", return_value=crl), \
                patch.object(activator, "restart_service") as restart:
            changed = activator.activate(
                "https://crl.example.test/current.pem", self.base_bundle,
                self.ca_cert, self.active, "solari-client.service")
        self.assertTrue(changed)
        restart.assert_not_called()
        self.assertEqual(0o644, stat.S_IMODE(receipt.stat().st_mode))

    def test_same_crl_reconciles_changed_base_bundle(self) -> None:
        crl = self.make_crl(31)
        self.activate_bytes(crl)
        extra_key, extra_cert = self.root / "extra.key", self.root / "extra.pem"
        subprocess.run([
            "openssl", "req", "-x509", "-newkey", "rsa:2048", "-nodes",
            "-keyout", str(extra_key), "-out", str(extra_cert), "-days", "5",
            "-subj", "/CN=Additional activation test root",
            "-addext", "basicConstraints=critical,CA:TRUE",
            "-addext", "keyUsage=critical,keyCertSign,cRLSign",
        ], check=True, capture_output=True)
        self.base_bundle.write_bytes(self.ca_cert.read_bytes() + extra_cert.read_bytes())
        with patch.object(activator, "fetch_crl", return_value=crl), \
                patch.object(activator, "restart_service") as restart:
            self.assertTrue(activator.activate(
                "https://crl.example.test/current.pem", self.base_bundle,
                self.ca_cert, self.active, "solari-client.service"))
        restart.assert_called_once_with("solari-client.service")
        self.assertIn(extra_cert.read_bytes().strip(), self.active.read_bytes())

    def test_fetch_uses_verified_https_and_rejects_an_untrusted_server(self) -> None:
        crl = self.make_crl(14)
        tls_key, tls_cert = self.root / "https.key", self.root / "https.pem"
        subprocess.run([
            "openssl", "req", "-x509", "-newkey", "rsa:2048", "-nodes",
            "-keyout", str(tls_key), "-out", str(tls_cert), "-days", "2",
            "-subj", "/CN=localhost",
            "-addext", "subjectAltName=IP:127.0.0.1",
            "-addext", "extendedKeyUsage=serverAuth",
        ], check=True, capture_output=True)

        redirect_targets: list[str] = []

        class Handler(http.server.BaseHTTPRequestHandler):
            def do_GET(self) -> None:
                if self.path == "/redirect.crl":
                    self.send_response(302)
                    self.send_header("Location", "https://example.invalid/current.crl")
                    self.end_headers()
                    return
                if self.path in ("/redirect-301.crl", "/redirect-307.crl"):
                    self.send_response(301 if self.path.startswith("/redirect-301") else 307)
                    self.send_header("Location", f"https://127.0.0.1:{self.server.server_port}/redirect-target.crl")
                    self.end_headers()
                    return
                if self.path == "/redirect-target.crl":
                    redirect_targets.append(self.path)
                self.send_response(200)
                self.send_header("Content-Type", "application/pkix-crl")
                self.send_header("Content-Length", str(len(crl)))
                self.end_headers()
                self.wfile.write(crl)

            def log_message(self, _format: str, *_args: object) -> None:
                return

        server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Handler)
        context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        context.load_cert_chain(tls_cert, tls_key)
        server.socket = context.wrap_socket(server.socket, server_side=True)
        thread = threading.Thread(target=server.serve_forever, daemon=True)
        thread.start()
        url = f"https://127.0.0.1:{server.server_port}/current.crl"
        try:
            self.assertEqual(crl, activator.fetch_crl(url, tls_cert, 5))
            with self.assertRaises(urllib.error.HTTPError) as redirect:
                activator.fetch_crl(url.rsplit("/", 1)[0] + "/redirect.crl", tls_cert, 5)
            self.assertEqual(302, redirect.exception.code, "cross-host redirects must be refused before a second request")
            redirect.exception.close()
            for code in (301, 307):
                with self.assertRaises(urllib.error.HTTPError) as same_host_redirect:
                    activator.fetch_crl(url.rsplit("/", 1)[0] + f"/redirect-{code}.crl", tls_cert, 5)
                self.assertEqual(code, same_host_redirect.exception.code,
                                 "same-host redirects must be refused without following them")
                same_host_redirect.exception.close()
            self.assertEqual([], redirect_targets, "no redirect target may be fetched")
            with self.assertRaises(urllib.error.URLError):
                activator.fetch_crl(url, None, 5)
        finally:
            server.shutdown()
            server.server_close()
            thread.join(timeout=5)


if __name__ == "__main__":
    unittest.main()
