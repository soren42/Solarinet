from __future__ import annotations

import datetime as dt
import io
import json
import os
import pathlib
import shutil
import subprocess
import sys
import tempfile
import unittest
from contextlib import redirect_stdout
from unittest.mock import patch

import acme_expiry_watch as watch


class AcmeExpiryWatchTests(unittest.TestCase):
    def setUp(self) -> None:
        self.temp = tempfile.TemporaryDirectory(prefix="solarinet-expiry-test-")
        self.root = pathlib.Path(self.temp.name)
        self.cert = self.root / "leaf.pem"
        self.cert.write_text("placeholder", encoding="ascii")
        self.config_path = self.root / "watch.json"
        self.state_path = self.root / "state.json"
        self.config = {
            "schema_version": 1,
            "warning_days": 21,
            "critical_days": 7,
            "state_file": str(self.state_path),
            "notify_command": ["/usr/bin/python3", "/usr/local/bin/notify-send"],
            "services": [{
                "id": "dashboard",
                "certificate": str(self.cert),
                "domains": ["dashboard.example.test"],
                "ip_sans": ["10.0.0.20"],
            }],
        }
        self._write_config()

    def tearDown(self) -> None:
        self.temp.cleanup()

    def _write_config(self) -> None:
        self.config_path.write_text(json.dumps(self.config), encoding="utf-8")
        self.config_path.chmod(0o600)

    def _make_one_day_certificate(self) -> pathlib.Path:
        key = self.root / "short-lived-key.pem"
        cert = self.root / "short-lived-leaf.pem"
        subprocess.run([
            "openssl", "req", "-x509", "-newkey", "rsa:2048", "-nodes", "-days", "1",
            "-keyout", str(key), "-out", str(cert), "-subj", "/CN=dashboard.example.test",
            "-addext", "subjectAltName=DNS:dashboard.example.test,IP:10.0.0.20",
        ], check=True, capture_output=True)
        return cert

    def test_config_requires_private_file_and_valid_thresholds(self) -> None:
        self.config_path.chmod(0o644)
        with self.assertRaisesRegex(ValueError, "permissions"):
            watch.load_config(self.config_path)
        self.config_path.chmod(0o600)
        self.config["critical_days"] = 21
        self._write_config()
        with self.assertRaisesRegex(ValueError, "warning_days"):
            watch.load_config(self.config_path)

    def test_xenon_inventory_example_loads_with_exact_services(self) -> None:
        source = pathlib.Path(__file__).with_name("acme_expiry_watch.xenon.example.json")
        shutil.copyfile(source, self.config_path)
        self.config_path.chmod(0o600)
        config = watch.load_config(self.config_path)
        self.assertEqual([row["id"] for row in config["services"]],
                         ["akoria_public", "dashboard", "rackwire"])

    def test_isolated_mode_starts_with_sibling_acme_module(self) -> None:
        install = self.root / "libexec" / "solarinet-pki"
        install.mkdir(parents=True)
        shutil.copy2(pathlib.Path(watch.__file__), install / "acme_expiry_watch.py")
        shutil.copy2(pathlib.Path(watch.acme.__file__), install / "acme_certificates.py")
        result = subprocess.run(
            [sys.executable, "-I", str(install / "acme_expiry_watch.py"), "--help"],
            cwd="/", stdin=subprocess.DEVNULL, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
            timeout=10, check=False,
        )
        self.assertEqual(result.returncode, 0, result.stderr.decode("utf-8", "replace"))
        self.assertIn(b"--dry-run", result.stdout)

    def test_assess_deduplicates_warning_then_escalates_and_recovers(self) -> None:
        now = dt.datetime(2026, 10, 24, tzinfo=dt.timezone.utc)
        warning = {"fingerprint": "a" * 64, "not_after": "2026-11-07T00:00:00+00:00",
                   "remaining_seconds": 14 * 86400}
        with patch.object(watch, "inspect_certificate", return_value=warning):
            state, alerts, results = watch.assess(self.config, {}, now)
            self.assertEqual(results[0]["status"], "warn")
            self.assertEqual([a["severity"] for a in alerts], ["warn"])
            state_again, duplicate_alerts, _ = watch.assess(self.config, state, now)
        self.assertEqual(state_again, state)
        self.assertEqual(duplicate_alerts, [])

        critical = dict(warning, remaining_seconds=2 * 86400)
        with patch.object(watch, "inspect_certificate", return_value=critical):
            state, alerts, _ = watch.assess(self.config, state, now)
        self.assertEqual([a["severity"] for a in alerts], ["crit"])

        healthy = dict(warning, remaining_seconds=30 * 86400)
        with patch.object(watch, "inspect_certificate", return_value=healthy):
            _, alerts, _ = watch.assess(self.config, state, now)
        self.assertEqual([a["severity"] for a in alerts], ["info"])
        self.assertIn("recovered", alerts[0]["title"])

    def test_changed_certificate_fingerprint_repeats_active_warning(self) -> None:
        now = dt.datetime(2026, 10, 24, tzinfo=dt.timezone.utc)
        previous = {"dashboard": {"status": "warn", "fingerprint": "old"}}
        fresh = {"fingerprint": "new", "not_after": "2026-11-07T00:00:00+00:00",
                 "remaining_seconds": 14 * 86400}
        with patch.object(watch, "inspect_certificate", return_value=fresh):
            _, alerts, _ = watch.assess(self.config, previous, now)
        self.assertEqual([alert["severity"] for alert in alerts], ["warn"])

    def test_certificate_errors_are_critical_and_report_without_paths(self) -> None:
        now = dt.datetime(2026, 10, 24, tzinfo=dt.timezone.utc)
        with patch.object(watch, "inspect_certificate", side_effect=ValueError("wrong SAN")):
            state, alerts, results = watch.assess(self.config, {}, now)
        self.assertEqual(results[0]["status"], "crit")
        self.assertEqual(alerts[0]["severity"], "crit")
        self.assertIn("wrong SAN", alerts[0]["body"])
        self.assertNotIn(str(self.root), alerts[0]["body"])
        self.assertEqual(state["dashboard"]["fingerprint"], "unavailable")

    def test_notification_failure_does_not_advance_state(self) -> None:
        alert = {"severity": "warn", "title": "dashboard expiring", "body": "renew it"}

        def fail(*args: object, **kwargs: object) -> object:
            raise OSError("private broker URL must not escape")

        with self.assertRaisesRegex(RuntimeError, "could not complete"):
            watch.publish(["/usr/bin/true"], alert, fail)

    def test_notification_uses_argv_without_shell_interpolation(self) -> None:
        calls: list[tuple[list[str], dict[str, object]]] = []

        def succeed(args: list[str], **kwargs: object) -> object:
            calls.append((args, kwargs))
            return type("Result", (), {"returncode": 0})()

        alert = {"severity": "warn", "title": "web cert expires", "body": "renew within 21 days"}
        command = ["/usr/bin/true", "--config", "/etc/notify.conf"]
        watch.publish(command, alert, succeed)
        self.assertEqual(calls[0][0], [str(watch._trusted_notifier(command[0])), *command[1:],
            "--severity", "warn", "--title", alert["title"], "--body", alert["body"],
            "--source", "solarinet-pki",
        ])
        self.assertNotIn("shell", calls[0][1])

    def test_notification_refuses_user_writable_code_path(self) -> None:
        untrusted = self.root / "untrusted-notifier"
        untrusted.write_text("#!/bin/sh\nexit 0\n", encoding="ascii")
        untrusted.chmod(0o755)
        with self.assertRaisesRegex(ValueError, "root-owned|writable directory"):
            watch._trusted_notifier(str(untrusted))

    def test_notification_does_not_allow_writable_symlink_path(self) -> None:
        link = self.root / "trusted-looking-link"
        link.symlink_to("/usr/bin/true")
        with self.assertRaisesRegex(ValueError, "root-owned|writable directory"):
            watch._trusted_notifier(str(link))

    def test_state_file_round_trips_with_private_permissions(self) -> None:
        state = {"dashboard": {"status": "warn", "fingerprint": "a" * 64, "not_after": None}}
        watch.save_state(self.state_path, state)
        self.assertEqual(watch.load_state(self.state_path), state)
        self.assertEqual(self.state_path.stat().st_mode & 0o777, 0o600)

    def test_inspects_real_certificate_dates_fingerprint_and_dns_sans(self) -> None:
        cert = self._make_one_day_certificate()
        result = watch.inspect_certificate(
            cert, {"dashboard.example.test"}, dt.datetime.now(dt.timezone.utc), {"10.0.0.20"})
        self.assertRegex(result["fingerprint"], r"^[0-9a-f]{64}$")
        self.assertGreater(result["remaining_seconds"], 0)
        with self.assertRaisesRegex(ValueError, "SANs do not match"):
            watch.inspect_certificate(cert, {"other.example.test"}, dt.datetime.now(dt.timezone.utc), {"10.0.0.20"})
        with self.assertRaisesRegex(ValueError, "SANs do not match"):
            watch.inspect_certificate(cert, {"dashboard.example.test"}, dt.datetime.now(dt.timezone.utc), set())

    def test_dry_run_reports_critical_without_publishing_or_advancing_state(self) -> None:
        self.config["services"][0]["certificate"] = str(self._make_one_day_certificate())
        self.config["services"][0]["domains"] = ["DASHBOARD.EXAMPLE.TEST"]
        self._write_config()
        output = io.StringIO()
        with redirect_stdout(output):
            result = watch.main(["--config", str(self.config_path), "--dry-run"])
        self.assertEqual(result, 1)
        self.assertIn("CRIT dashboard", output.getvalue())
        self.assertIn("ALERT CRIT", output.getvalue())
        self.assertFalse(self.state_path.exists())

    def test_successful_alert_delivery_persists_critical_status_and_returns_success(self) -> None:
        self.config["services"][0]["certificate"] = str(self._make_one_day_certificate())
        self.config["notify_command"] = ["/usr/bin/true"]
        self._write_config()
        with redirect_stdout(io.StringIO()):
            result = watch.main(["--config", str(self.config_path)])
        self.assertEqual(result, 0)
        self.assertEqual(watch.load_state(self.state_path)["dashboard"]["status"], "crit")


if __name__ == "__main__":
    unittest.main()
