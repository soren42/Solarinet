#!/usr/bin/env python3
"""Validate the Khadas secondary configuration generator."""
import importlib.util
import pathlib
import shutil
import subprocess
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parent
SPEC = importlib.util.spec_from_file_location("gen_secondary", ROOT / "gen-secondary.py")
gen = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(gen)


class SecondaryConfigTest(unittest.TestCase):
    def test_generates_every_primary_zone_and_ignores_secondary_and_forward_zones(self):
        source = '''zone "akoria.net" { type primary; file "/zones/db.akoria.net"; };
zone "0.7.10.in-addr.arpa" { type primary; file "/zones/rev"; };
zone "akoria.org" { type forward; forwarders { 10.1.0.10; }; };
zone "unrelated" { type secondary; primaries { 192.0.2.1; }; };
'''
        result = gen.generate(source, "10.0.0.20", "/var/cache/bind/solarinet-secondary")
        self.assertEqual(2, result.count("type secondary"))
        self.assertIn('zone "akoria.net" { type secondary; primaries { 10.0.0.20; }', result)
        self.assertIn('zone "0.7.10.in-addr.arpa" { type secondary;', result)
        self.assertNotIn("akoria.org", result)
        self.assertNotIn("unrelated", result)

    def test_merges_bare_master_and_rpz_primary_fragments(self):
        akoria = 'zone "akoria.net" { type primary; file "/zones/db.akoria.net"; };'
        bare = 'zone "dashboard" { type master; file "/zones/db.bare-dashboard"; };'
        local = '''zone "rpz.akoria" { type primary; file "/zones/db.rpz.akoria"; };
zone "akoria.org" { type forward; forwarders { 10.1.0.10; }; };'''
        result = gen.generate([akoria, bare, local], "10.0.0.20",
                              "/var/cache/bind/solarinet-secondary")
        self.assertEqual(3, result.count("type secondary"))
        self.assertIn('zone "dashboard" { type secondary;', result)
        self.assertIn('zone "rpz.akoria" { type secondary;', result)
        self.assertNotIn("akoria.org", result)

    def test_parses_multiline_rpz_declaration_from_dedicated_fragment(self):
        rpz = '''zone "rpz.akoria" {
    type primary;
    file "/zones/db.rpz.akoria";
};
'''
        result = gen.generate([rpz], "10.0.0.20", "/var/cache/bind/solarinet-secondary")
        self.assertIn('zone "rpz.akoria" { type secondary;', result)

    def test_rejects_invalid_primary_or_path(self):
        for primary, path in [("not-an-ip", "/var/cache/bind"),
                              ("10.0.0.20", "/var/cache/bind/../tmp")]:
            with self.assertRaises(ValueError):
                gen.generate('zone "akoria.net" { type primary; };', primary, path)

    @unittest.skipUnless(shutil.which("named-checkconf"), "named-checkconf is required")
    def test_complete_config_passes_named_checkconf(self):
        source = ('zone "akoria.net" { type primary; file "/zones/db.akoria.net"; };\n'
                  'zone "rpz.akoria" { type primary; file "/zones/db.rpz.akoria"; };\n')
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            (root / "options.conf").write_text((ROOT / "options.conf").read_text()
                .replace("10.7.0.10", "127.0.0.1")
                .replace("/var/cache/bind/solarinet-secondary", directory))
            (root / "zones.conf").write_text(gen.generate(source, "10.0.0.20", "/var/cache/bind/slaves"))
            conf = f'include "{root}/options.conf";\ninclude "{root}/zones.conf";\n'
            (root / "named.conf").write_text(conf)
            run = subprocess.run(["named-checkconf", str(root / "named.conf")],
                                 capture_output=True, text=True, timeout=10)
            self.assertEqual(0, run.returncode, run.stdout + run.stderr)


if __name__ == "__main__":
    unittest.main()
