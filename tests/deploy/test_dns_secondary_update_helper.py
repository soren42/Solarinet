#!/usr/bin/env python3
"""Exercise the role updater without requiring BIND or touching host paths."""
import importlib.util
import json
import pathlib
import re
import stat
import tempfile
from types import SimpleNamespace
import unittest
from unittest import mock

ROOT = pathlib.Path(__file__).resolve().parents[2]
TEMPLATE = ROOT / "deploy/fleet/ansible/roles/dns-secondary/templates/update-zones.py.j2"


def load_helper():
    source = TEMPLATE.read_text(encoding="utf-8")
    values = {
        "dns_secondary_conf_dir": "/etc/test-bind/secondary",
        "dns_secondary_data_dir": "/var/lib/named/test-secondary",
        "dns_secondary_primary": "10.0.0.20",
        "dns_secondary_main_conf": "/etc/test-bind/named.conf",
        "dns_secondary_checkconf": "/usr/sbin/named-checkconf",
        "dns_secondary_rndc": "/usr/sbin/rndc",
    }
    for key, value in values.items():
        source = source.replace("{{ " + key + " | to_json }}", json.dumps(value))
    spec = importlib.util.spec_from_loader("dns_secondary_update_helper", loader=None)
    module = importlib.util.module_from_spec(spec)
    exec(compile(source, str(TEMPLATE), "exec"), module.__dict__)
    return module


HELPER = load_helper()


class UpdateHelperTest(unittest.TestCase):
    def test_normalizes_generator_path_and_is_idempotent(self):
        with tempfile.TemporaryDirectory() as temp:
            root = pathlib.Path(temp)
            config = root / "config"
            config.mkdir()
            options, zones = config / "options.conf", config / "zones.conf"
            options.write_text("options {};\n", encoding="utf-8")
            zones.write_text("// Generated zone declarations are installed atomically by the update helper.\n",
                             encoding="utf-8")
            runtime_zone_re = re.compile(
                r'^zone "([A-Za-z0-9._-]+)" \{ type secondary; primaries '
                r'\{ 10\.0\.0\.20; \}; file "' + re.escape(str(root / "named-data"))
                + r'/db\.([A-Za-z0-9._-]+)"; \};$')
            with mock.patch.multiple(HELPER, CONFIG_DIR=config, OPTIONS=options, ZONES=zones,
                                    LOCK=config / ".zones.lock", DATA_DIR=str(root / "named-data"),
                                    MAIN_CONF=str(root / "named.conf"), CHECKCONF="named-checkconf",
                                    ZONE_RE=runtime_zone_re), \
                 mock.patch.object(HELPER.subprocess, "run") as run, \
                 mock.patch.object(HELPER.os, "fchown"), \
                 mock.patch.object(HELPER, "safe_config_dir"), \
                 mock.patch.object(HELPER.os, "fstat",
                                   return_value=SimpleNamespace(st_mode=stat.S_IFREG | 0o600,
                                                                st_uid=0)):
                payload = ('// Generated from the active SolariNet primary zone list; do not edit.\n'
                           'zone "akoria.net" { type secondary; primaries { 10.0.0.20; }; '
                           'file "/var/cache/bind/solarinet-secondary/db.akoria.net"; };\n')
                HELPER.apply(payload)
                saved = zones.read_text(encoding="utf-8")
                self.assertIn(str(root / "named-data/db.akoria.net"), saved)
                self.assertNotIn("/var/cache/bind", saved)
                run.reset_mock()
                HELPER.apply(payload)
                self.assertEqual(saved, zones.read_text(encoding="utf-8"))
                self.assertFalse(any(call.args[0][-1] == "reconfig" for call in run.call_args_list))

    def test_rejects_unsupported_zone_declaration(self):
        with self.assertRaisesRegex(ValueError, "unsupported declaration"):
            HELPER.validate('zone "akoria.net" { type forward; forwarders { 1.1.1.1; }; };\n')


class RoleTaskOrderTest(unittest.TestCase):
    """A fresh SELinux host needs the key and labels in place before named runs."""

    def setUp(self):
        self.tasks = (ROOT / "deploy/fleet/ansible/roles/dns-secondary/tasks/main.yml").read_text()

    def pos(self, name):
        idx = self.tasks.find("- name: " + name)
        self.assertGreaterEqual(idx, 0, name)
        return idx

    def test_suse_key_generated_before_validated_templates(self):
        self.assertLess(self.pos("Ensure SUSE rndc key exists"),
                        self.pos("Install authoritative-only BIND configuration"))

    def test_labels_restored_before_named_starts(self):
        restore = self.pos("Restore SELinux labels on BIND files")
        self.assertGreater(restore, self.pos("Install privileged zone update helper"))
        self.assertGreater(restore, self.pos("Configure narrowly scoped update sudo access"))
        self.assertLess(restore, self.pos("Ensure authoritative secondary is enabled and running"))
        self.assertIn("restorecon -RFv", self.tasks)
        self.assertIn("/etc/rndc.key", self.tasks)

    def test_check_pass_on_fresh_host_skips_service_start(self):
        start = self.pos("Ensure authoritative secondary is enabled and running")
        block = self.tasks[start:self.tasks.find("- name:", start + 1)]
        self.assertIn("when: not (ansible_check_mode and dns_secondary_install is changed)", block)
        install = self.tasks[self.pos("Install BIND"):self.pos("Ensure Debian rndc key exists")]
        self.assertIn("register: dns_secondary_install", install)


if __name__ == "__main__":
    unittest.main()
