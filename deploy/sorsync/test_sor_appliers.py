"""Exercise target success semantics without touching DNS, AD, or Pi-hole."""
import configparser
import json
import os
import pathlib
import sys
import tempfile
import types
import unittest
from unittest import mock

try:
    import pika  # noqa: F401
except ModuleNotFoundError:
    pika_stub = types.ModuleType("pika")
    pika_stub.BasicProperties = lambda **kwargs: types.SimpleNamespace(**kwargs)
    sys.modules["pika"] = pika_stub
try:
    import pymysql  # noqa: F401
except ModuleNotFoundError:
    sys.modules["pymysql"] = types.ModuleType("pymysql")

import sor_apply_ad
import sor_apply_dnsd
import sor_apply_pihole


class AdStateTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.cfg = configparser.ConfigParser()
        self.cfg["ad"] = {"state_file": os.path.join(self.temp.name, "managed.json"),
                          "protect": "radium"}

    def test_preexisting_ad_name_is_never_claimed_for_deletion(self):
        with mock.patch.object(sor_apply_ad, "sor_hosts", return_value={"manual": "10.0.0.5"}), \
             mock.patch.object(sor_apply_ad, "ad_query_a", return_value="10.0.0.5"), \
             mock.patch.object(sor_apply_ad, "_sh") as command:
            self.assertTrue(sor_apply_ad.reconcile(self.cfg))
            command.assert_not_called()
        self.assertEqual(set(), sor_apply_ad.load_state(self.cfg))
        with mock.patch.object(sor_apply_ad, "sor_hosts", return_value={}), \
             mock.patch.object(sor_apply_ad, "_sh") as command:
            self.assertTrue(sor_apply_ad.reconcile(self.cfg))
            command.assert_not_called()

    def test_created_name_is_tracked_and_deleted_when_removed(self):
        with mock.patch.object(sor_apply_ad, "sor_hosts", return_value={"created": "10.0.0.6"}), \
             mock.patch.object(sor_apply_ad, "ad_query_a", return_value=None), \
             mock.patch.object(sor_apply_ad, "_sh", return_value=(0, "")) as command:
            self.assertTrue(sor_apply_ad.reconcile(self.cfg))
            self.assertEqual("add", command.call_args.args[1])
        self.assertEqual({"created"}, sor_apply_ad.load_state(self.cfg))
        with mock.patch.object(sor_apply_ad, "sor_hosts", return_value={}), \
             mock.patch.object(sor_apply_ad, "ad_query_a", return_value="10.0.0.6"), \
             mock.patch.object(sor_apply_ad, "_sh", return_value=(0, "")) as command:
            self.assertTrue(sor_apply_ad.reconcile(self.cfg))
            self.assertEqual("delete", command.call_args.args[1])
        self.assertEqual(set(), sor_apply_ad.load_state(self.cfg))

    def test_failed_add_does_not_advance_managed_state(self):
        with mock.patch.object(sor_apply_ad, "sor_hosts", return_value={"missing": "10.0.0.7"}), \
             mock.patch.object(sor_apply_ad, "ad_query_a", return_value=None), \
             mock.patch.object(sor_apply_ad, "_sh", return_value=(1, "denied")):
            self.assertFalse(sor_apply_ad.reconcile(self.cfg))
        self.assertEqual(set(), sor_apply_ad.load_state(self.cfg))

    def test_corrupt_state_fails_closed(self):
        with open(self.cfg["ad"]["state_file"], "w", encoding="utf-8") as state_file:
            state_file.write("not json")
        with self.assertRaises(ValueError):
            sor_apply_ad.load_state(self.cfg)

    def test_legacy_desired_name_list_does_not_grant_delete_ownership(self):
        with open(self.cfg["ad"]["state_file"], "w", encoding="utf-8") as state_file:
            state_file.write('["manual", "created"]')
        self.assertEqual(set(), sor_apply_ad.load_state(self.cfg))

    def test_ad_query_distinguishes_missing_name_from_auth_failure(self):
        with mock.patch.object(sor_apply_ad, "_sh", return_value=(255, "WERR_DNS_ERROR_NAME_DOES_NOT_EXIST")):
            self.assertIsNone(sor_apply_ad.ad_query_a(self.cfg, "missing"))
        with mock.patch.object(sor_apply_ad, "_sh", return_value=(255, "NT_STATUS_ACCESS_DENIED")):
            with self.assertRaises(RuntimeError):
                sor_apply_ad.ad_query_a(self.cfg, "unknown")


class TargetApplyTest(unittest.TestCase):
    def test_secondary_targets_are_named_unique_and_ipv4_validated(self):
        cfg = configparser.ConfigParser()
        cfg["dns.secondary.chlorine"] = {"address": "10.7.0.10",
                                         "ssh_host": "jason@chlorine"}
        cfg["dns.secondary.iron"] = {"address": "10.0.0.11",
                                     "ssh_host": "jason@10.0.0.11"}
        self.assertEqual(["chlorine", "iron"],
                         [item["name"] for item in sor_apply_dnsd.secondary_targets(cfg)])
        cfg["dns.secondary.iron"]["address"] = "10.7.0.10"
        with self.assertRaisesRegex(ValueError, "unique"):
            sor_apply_dnsd.secondary_targets(cfg)

    def test_missing_secondary_fails_closed(self):
        with self.assertRaisesRegex(ValueError, "at least one DNS secondary"):
            sor_apply_dnsd.secondary_targets(configparser.ConfigParser())

    def test_secondary_refresh_uses_configured_host_and_propagates_failure(self):
        cfg = configparser.ConfigParser()
        cfg["dns"] = {"secondary_refresh_helper": "/usr/local/libexec/solari-dns-secondary-refresh",
                      "primary_config": "/etc/bind/named.conf.akoria",
                      "primary_additional_configs": "/etc/bind/named.conf.bare",
                      "primary_address": "10.0.0.20"}
        target = {"name": "chlorine", "address": "10.7.0.10",
                  "ssh_host": "jason@chlorine",
                  "update_helper": "/usr/local/sbin/solari-bind9-update-zones"}
        ok = types.SimpleNamespace(returncode=0, stdout="chlorine serves current SOA serials")
        with mock.patch.object(sor_apply_dnsd.subprocess, "run", return_value=ok) as run:
            self.assertEqual("a" * 64,
                             sor_apply_dnsd.refresh_secondary(cfg, target, "a" * 64))
        command = run.call_args.args[0]
        self.assertIn("10.7.0.10", command)
        self.assertIn("jason@chlorine", command)
        self.assertIn("/etc/bind/named.conf.bare", command)
        failed = types.SimpleNamespace(returncode=1, stdout="", stderr="failed")
        with mock.patch.object(sor_apply_dnsd.subprocess, "run", return_value=failed):
            with self.assertRaisesRegex(RuntimeError, "secondary refresh failed for chlorine"):
                sor_apply_dnsd.refresh_secondary(cfg, target, "a" * 64)

    def test_apply_keeps_target_pending_when_secondary_cannot_converge(self):
        with tempfile.TemporaryDirectory() as folder:
            root = pathlib.Path(folder)
            incoming = root / "incoming"
            state_dir = root / "state"
            incoming.mkdir(mode=0o700)
            state_dir.mkdir(mode=0o700)
            cfg = configparser.ConfigParser()
            cfg["dns"] = {"incoming_root": str(incoming),
                          "target_state_file": str(state_dir / "targets.json")}
            cfg["dns.secondary.chlorine"] = {"address": "10.7.0.10",
                                             "ssh_host": "jason@chlorine"}

            def render(_config, output):
                (output / "db.example.invalid").write_text("zone", encoding="utf-8")
                return True

            with mock.patch.object(sor_apply_dnsd, "render", side_effect=render), \
                 mock.patch.object(sor_apply_dnsd, "local_release", return_value="a" * 64), \
                 mock.patch.object(sor_apply_dnsd, "refresh_secondary",
                                   side_effect=RuntimeError("SOA did not converge")):
                self.assertFalse(sor_apply_dnsd.apply(cfg))

            state = json.loads((state_dir / "targets.json").read_text(encoding="utf-8"))
            self.assertEqual("applied", state["targets"]["primary"]["phase"])
            secondary = state["targets"]["secondary:chlorine"]
            self.assertEqual("failed", secondary["phase"])
            self.assertEqual("SOA did not converge", secondary["error"])

    def test_pihole_reload_failure_keeps_delivery_pending(self):
        cfg = configparser.ConfigParser()
        cfg["pihole.one"] = {"ssh": "one", "custom_list": "/tmp/custom.list",
                             "reload": "sudo pihole reloaddns"}
        read = types.SimpleNamespace(returncode=0, stdout="10.0.0.1 a.akoria.net\n")
        reload_failed = types.SimpleNamespace(returncode=1, stderr="reload failed")
        with mock.patch.object(sor_apply_pihole.subprocess, "run",
                               side_effect=[read, reload_failed]) as run:
            self.assertEqual("failed", sor_apply_pihole.apply_target(
                cfg, "one", read.stdout))
            self.assertEqual(2, run.call_count)

    def test_pihole_partial_apply_reports_failure(self):
        cfg = configparser.ConfigParser()
        cfg["pihole"] = {"targets": "one,two"}
        with mock.patch.object(sor_apply_pihole, "render", return_value="content"), \
             mock.patch.object(sor_apply_pihole, "apply_target",
                               side_effect=["changed", "failed"]):
            self.assertFalse(sor_apply_pihole.apply_all(cfg))


if __name__ == "__main__":
    unittest.main()
