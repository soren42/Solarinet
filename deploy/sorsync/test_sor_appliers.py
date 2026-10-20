"""Exercise target success semantics without touching DNS, AD, or Pi-hole."""
import configparser
import os
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
    def test_dns_render_failure_is_not_success(self):
        with mock.patch.object(sor_apply_dnsd, "render", return_value=False), \
             mock.patch.object(sor_apply_dnsd, "deploy_if_changed") as deploy:
            self.assertFalse(sor_apply_dnsd.apply(configparser.ConfigParser()))
            deploy.assert_not_called()

    def test_dns_retry_reloads_after_prior_reload_failure(self):
        with tempfile.TemporaryDirectory() as directory:
            src = os.path.join(directory, "src")
            dst = os.path.join(directory, "dst")
            os.mkdir(src)
            os.mkdir(dst)
            for folder in (src, dst):
                with open(os.path.join(folder, "db.akoria.net"), "w", encoding="utf-8") as zone:
                    zone.write("same rendered zone")
            cfg = configparser.ConfigParser()
            cfg["dns"] = {"zone_src_dir": src, "zone_dst_dir": dst,
                          "named_conf_akoria": os.path.join(dst, "named.conf.akoria"),
                          "reload_cmd": "sudo rndc reload"}
            with mock.patch.object(sor_apply_dnsd.subprocess, "run") as run:
                self.assertEqual([], sor_apply_dnsd.deploy_if_changed(cfg))
                run.assert_called_once_with(["sudo", "rndc", "reload"], check=True, timeout=30)

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
