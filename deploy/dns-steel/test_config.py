#!/usr/bin/env python3
"""Validate the checked-in steel BIND include chain with a local fixture."""
import pathlib
import shutil
import subprocess
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parent


class SteelBindConfigTest(unittest.TestCase):
    @unittest.skipUnless(shutil.which("named-checkconf"), "named-checkconf is required")
    def test_parent_includes_the_legacy_file_expected_by_release_bootstrap(self):
        local = (ROOT / "named.conf.local").read_text()
        self.assertEqual(1, local.count('include "/etc/bind/named.conf.bare";'))

        with tempfile.TemporaryDirectory(prefix="steel-bind-config-") as directory:
            root = pathlib.Path(directory)
            for name in ("named.conf", "named.conf.local", "named.conf.options"):
                source = (ROOT / name).read_text()
                (root / name).write_text(source.replace("/etc/bind/", str(root) + "/"))
            # Bootstrap requires one legacy bare-zone include. The fixture only
            # provides a declaration, so this checks BIND syntax without DNS.
            (root / "named.conf.bare").write_text(
                'zone "fixture" { type primary; file "/var/cache/bind/db.fixture"; };\n'
            )
            result = subprocess.run(
                ["named-checkconf", "-p", str(root / "named.conf")],
                capture_output=True, text=True, timeout=10,
            )
            self.assertEqual(0, result.returncode, result.stderr)


if __name__ == "__main__":
    unittest.main()
