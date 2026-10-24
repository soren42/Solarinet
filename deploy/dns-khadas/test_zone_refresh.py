#!/usr/bin/env python3
"""Unit tests for the chlorine secondary-zone refresh boundary."""
import importlib.util
import pathlib
import subprocess
import tempfile
import unittest
from unittest import mock

ROOT = pathlib.Path(__file__).resolve().parent


def load(name, filename):
    spec = importlib.util.spec_from_file_location(name, ROOT / filename)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


refresh = load("refresh_secondary", "refresh-secondary-zones.py")
update = load("update_zones", "update-zones.py")


class UpdateValidationTest(unittest.TestCase):
    def test_accepts_only_expected_secondary_declarations(self):
        update.validate('''// Generated from the active SolariNet primary zone list; do not edit.
zone "akoria.net" { type secondary; primaries { 10.0.0.20; }; file "/var/cache/bind/solarinet-secondary/db.akoria.net"; };
''')

    def test_rejects_wrong_master_path_duplicate_and_empty_configs(self):
        invalid = [
            'zone "akoria.net" { type secondary; primaries { 192.0.2.1; }; file "/tmp/db.akoria.net"; };',
            ('zone "akoria.net" { type secondary; primaries { 10.0.0.20; }; '
             'file "/var/cache/bind/solarinet-secondary/db.akoria.net"; };\n') * 2,
            "",
        ]
        for text in invalid:
            with self.subTest(text=text), self.assertRaises(ValueError):
                update.validate(text)


class RefreshDispatchTest(unittest.TestCase):
    def test_active_inventory_failure_is_generic_and_fails_closed(self):
        with mock.patch.object(refresh.subprocess, "run", return_value=mock.Mock(
                returncode=1, stdout="test-only-sensitive-output")) as run:
            with self.assertRaisesRegex(RuntimeError, "could not inspect active BIND") as error:
                refresh.active_primary_zones()
        self.assertNotIn("test-only-sensitive-output", str(error.exception))
        run.assert_called_once_with(["named-checkconf", "-p"], capture_output=True,
                                    text=True, timeout=15)

    def test_pushes_generated_zone_list_then_verifies_serials(self):
        with tempfile.TemporaryDirectory() as directory:
            primary = pathlib.Path(directory) / "named.conf.akoria"
            primary.write_text('zone "akoria.net" { type primary; file "/zones/db.akoria.net"; };\n')
            with mock.patch.object(refresh.subprocess, "run") as run, \
                 mock.patch.object(refresh, "serial", return_value="2026102101") as serial:
                count = refresh.synchronize(primary, "10.0.0.20", "10.7.0.10",
                                            "jason@chlorine", "/usr/local/sbin/helper",
                                            expanded_config=(
                                                'zone "akoria.net" { type primary; file "/zones/db.akoria.net"; };\n'))
            self.assertEqual(1, count)
            run.assert_called_once()
            args, kwargs = run.call_args
            self.assertIn("jason@chlorine", args[0])
            self.assertIn("sudo", args[0])
            self.assertIn('zone "akoria.net" { type secondary;', kwargs["input"])
            serial.assert_called_with("10.1.0.10", "akoria.net")

    def test_additional_primary_fragments_join_the_transfer_list(self):
        with tempfile.TemporaryDirectory() as directory:
            primary = pathlib.Path(directory) / "named.conf.akoria"
            bare = pathlib.Path(directory) / "named.conf.bare"
            local = pathlib.Path(directory) / "named.conf.local"
            rpz = pathlib.Path(directory) / "named.conf.rpz"
            primary.write_text('zone "akoria.net" { type primary; file "/zones/db.akoria.net"; };\n')
            bare.write_text('zone "dashboard" { type master; file "/zones/db.bare-dashboard"; };\n')
            local.write_text('include "/etc/bind/named.conf.rpz";\n')
            rpz.write_text('''zone "rpz.akoria" {
    type primary;
    file "/zones/db.rpz.akoria";
};
''')
            with mock.patch.object(refresh.subprocess, "run") as run, \
                 mock.patch.object(refresh, "serial", return_value="2026102101"):
                count = refresh.synchronize(primary, "10.0.0.20", "10.7.0.10",
                    "jason@chlorine", "/usr/local/sbin/helper", timeout_sec=1,
                    additional_configs=(bare, local, rpz),
                    expanded_config='''zone "akoria.net" { type primary; file "/zones/db.akoria.net"; };
zone "dashboard" { type master; file "/zones/db.bare-dashboard"; };
zone "rpz.akoria" {
    type primary;
    file "/zones/db.rpz.akoria";
};
''')
            self.assertEqual(3, count)
            payload = run.call_args.kwargs["input"]
            self.assertIn('zone "dashboard" { type secondary;', payload)
            self.assertIn('zone "rpz.akoria" { type secondary;', payload)

    def test_inventory_mismatch_fails_before_secondary_update(self):
        with tempfile.TemporaryDirectory() as directory:
            primary = pathlib.Path(directory) / "named.conf.akoria"
            primary.write_text('zone "akoria.net" { type primary; file "/zones/db.akoria.net"; };\n')
            active = '''zone "akoria.net" { type primary; file "/zones/db.akoria.net"; };
zone "included-but-not-passed" { type master; file "/zones/omitted"; };
'''
            with mock.patch.object(refresh.subprocess, "run") as run, \
                 self.assertRaisesRegex(RuntimeError, "differs from active BIND"):
                refresh.synchronize(primary, "10.0.0.20", "10.7.0.10",
                    "jason@chlorine", "/usr/local/sbin/helper",
                    expanded_config=active)
            run.assert_not_called()


if __name__ == "__main__":
    unittest.main()
