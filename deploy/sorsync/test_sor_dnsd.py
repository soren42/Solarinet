"""Prove multi-target DNS retry/ack boundaries without touching intranet DNS."""
import configparser
import json
import pathlib
import sys
import tempfile
import types
import unittest
from unittest import mock

try:
    import pika  # noqa: F401
except ModuleNotFoundError:
    sys.modules["pika"] = types.ModuleType("pika")

import sor_apply_dnsd as dns


def result(code=0, output=""):
    return types.SimpleNamespace(returncode=code, stdout=output, stderr="")


class DnsApplyTest(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.folder = pathlib.Path(self.temporary.name)
        self.incoming = self.folder / "incoming"
        self.incoming.mkdir(mode=0o700)
        self.applied = self.folder / "applied"
        self.applied.mkdir()
        self.cfg = configparser.ConfigParser()
        self.cfg["dns"] = {"incoming_root": str(self.incoming),
            "target_state_file": str(self.folder / "targets.json"),
            "applied_zone_dir": str(self.applied),
            "release_helper": "/usr/local/libexec/solarinet-dns-release",
            "release_config": "/etc/solarinet/dns-release.json",
            "secondary_refresh_helper": "/usr/local/libexec/solari-dns-secondary-refresh",
            "render_python": "python3", "gen_zones": "/fixture/gen-zones.py",
            "gen_bare": "/fixture/gen-bare.py"}
        self.cfg["dns.secondary.chlorine"] = {"address": "10.7.0.10",
                                               "ssh_host": "jason@chlorine"}
        self.cfg["sor"] = {"host": "sor.test", "port": "3307", "user": "fixture",
                            "password": "test-only", "name": "sor_fixture"}

    def rendered(self, _cfg, output):
        (output / "db.akoria.net").write_text("forward fixture\n")
        (output / "db.bare-xenon").write_text("bare fixture\n")
        return True

    def status(self):
        return json.loads((self.folder / "targets.json").read_text())["targets"]

    def test_secondary_retries_after_unchanged_primary_and_keeps_event_pending(self):
        with mock.patch.object(dns, "render", side_effect=self.rendered), \
             mock.patch.object(dns, "local_release", return_value="a" * 64) as primary, \
             mock.patch.object(dns, "refresh_secondary",
                               side_effect=[RuntimeError("offline"), "a" * 64]) as secondary:
            self.assertFalse(dns.apply(self.cfg))
            first = self.status()
            self.assertEqual("applied", first["primary"]["phase"])
            self.assertEqual("failed", first["secondary:chlorine"]["phase"])
            self.assertNotIn("lastSuccess", first["secondary:chlorine"])
            self.assertEqual("retired", first["steel"]["phase"])
            self.assertTrue(dns.apply(self.cfg))
            second = self.status()
            self.assertEqual(2, primary.call_count)
            self.assertEqual(2, secondary.call_count)
            self.assertEqual(first["primary"]["appliedGeneration"], second["primary"]["appliedGeneration"])
            self.assertEqual("applied", second["secondary:chlorine"]["phase"])
            self.assertEqual(second["secondary:chlorine"]["desiredSource"],
                             second["secondary:chlorine"]["appliedSource"])
            self.assertTrue(second["secondary:chlorine"]["lastSuccess"])
        self.assertEqual([], list(self.incoming.iterdir()))

    def test_primary_failure_skips_secondary_and_keeps_event_pending(self):
        with mock.patch.object(dns, "render", side_effect=self.rendered), \
             mock.patch.object(dns, "local_release", side_effect=RuntimeError("reload failed")), \
             mock.patch.object(dns, "refresh_secondary") as secondary:
            self.assertFalse(dns.apply(self.cfg))
            secondary.assert_not_called()
            self.assertEqual("failed", self.status()["primary"]["phase"])
            self.assertEqual("failed", self.status()["secondary:chlorine"]["phase"])

    def test_render_failure_never_reaches_a_target(self):
        with mock.patch.object(dns, "render", return_value=False), \
             mock.patch.object(dns, "local_release") as primary:
            self.assertFalse(dns.apply(self.cfg))
            primary.assert_not_called()

    def test_corrupt_target_state_fails_closed_before_root_apply(self):
        (self.folder / "targets.json").write_text("broken json")
        with mock.patch.object(dns, "render", side_effect=self.rendered), \
             mock.patch.object(dns, "local_release") as primary:
            with self.assertRaises(ValueError):
                dns.apply(self.cfg)
            primary.assert_not_called()

    def test_symlinked_target_state_fails_closed(self):
        outside = self.folder / "outside.json"
        outside.write_text('{"version":1,"targets":{}}')
        (self.folder / "targets.json").symlink_to(outside)
        with self.assertRaises(ValueError):
            dns.target_status(self.cfg, "primary", {"phase": "pending"})

    def test_symlinked_target_lock_fails_closed(self):
        outside = self.folder / "outside.lock"
        outside.touch()
        (self.folder / "targets.json.lock").symlink_to(outside)
        with self.assertRaises(OSError):
            dns.target_status(self.cfg, "primary", {"phase": "pending"})

    def test_render_uses_private_output_applied_serials_and_sor_connection(self):
        with mock.patch.object(dns.subprocess, "run", return_value=result()) as run:
            self.assertTrue(dns.render(self.cfg, self.incoming))
            command = run.call_args_list[0].args[0]
            self.assertIn(str(self.incoming), command)
            self.assertIn(str(self.applied), command)
            environment = run.call_args_list[0].kwargs["env"]
            self.assertEqual("sor.test", environment["SOR_DB_HOST"])
            self.assertEqual("3307", environment["SOR_DB_PORT"])
            self.assertEqual(2, run.call_count)
            bare_command = run.call_args_list[1].args[0]
            self.assertEqual(["--transfer", "10.7.0.10"], bare_command[-2:])

    def test_render_authorizes_each_named_secondary_for_axfr(self):
        self.cfg["dns.secondary.future"] = {"address": "192.0.2.20",
                                             "ssh_host": "jason@192.0.2.20"}
        with mock.patch.object(dns.subprocess, "run", return_value=result()) as run:
            self.assertTrue(dns.render(self.cfg, self.incoming))
        bare_command = run.call_args_list[1].args[0]
        transfer_args = [bare_command[index:index + 2]
                         for index, value in enumerate(bare_command[:-1])
                         if value == "--transfer"]
        self.assertEqual([["--transfer", "10.7.0.10"],
                          ["--transfer", "192.0.2.20"]], transfer_args)

    def test_helper_must_prove_applied_generation(self):
        with self.assertRaises(RuntimeError):
            dns.proof(result(output=json.dumps({"phase": "pending", "desired": "a" * 64})), "test")
        with self.assertRaises(RuntimeError):
            dns.proof(result(output="[]"), "test")
        with self.assertRaises(RuntimeError):
            dns.proof(result(1), "test")

if __name__ == "__main__":
    unittest.main()
