#!/usr/bin/env python3
"""Regression tests for repeatable, atomic forward/reverse zone generation."""
import contextlib
import io
import importlib.util
import os
import pathlib
import re
import shutil
import subprocess
import sys
import tempfile
import unittest
from unittest import mock

GENERATOR_PATH = pathlib.Path(__file__).with_name("gen-zones.py")
SPEC = importlib.util.spec_from_file_location("gen_zones", GENERATOR_PATH)
gen_zones = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(gen_zones)


def run_generator():
    with mock.patch.object(sys, "argv", [str(GENERATOR_PATH)]), \
         mock.patch.dict(os.environ, {"NETDB_SOURCE": "yaml"}):
        gen_zones.main()


class ZoneGenerationTest(unittest.TestCase):
    def test_unchanged_generation_keeps_serials_and_changed_data_advances_them(self):
        hosts = {
            "xenon": {"ip": "10.0.0.20"},
            "radium": {"ip": "10.1.0.10"},
            "chlorine": {"ip": "10.7.0.10"},
            "alpha": {"ip": "10.7.0.21"},
        }
        with tempfile.TemporaryDirectory() as folder:
            previous_out = gen_zones.OUT
            gen_zones.OUT = folder
            try:
                with mock.patch.object(gen_zones, "load_source",
                                       return_value=("akoria.net", hosts, {}, {})):
                    with contextlib.redirect_stdout(io.StringIO()):
                        run_generator()
                        first = self._serials(folder)
                        run_generator()
                        unchanged = self._serials(folder)
                        self.assertEqual(first, unchanged)

                        changed_hosts = dict(hosts)
                        changed_hosts["beta"] = {"ip": "10.7.0.22"}
                        with mock.patch.object(gen_zones, "load_source",
                                               return_value=("akoria.net", changed_hosts, {}, {})):
                            run_generator()
                        changed = self._serials(folder)
                        self.assertEqual(first["db.akoria.net"], changed["db.akoria.net"] - 1)
                        self.assertEqual(first["db.0.7.10.in-addr.arpa"],
                                         changed["db.0.7.10.in-addr.arpa"] - 1)
                        self.assertIn("202", pathlib.Path(folder, "db.akoria.net").read_text())
                        self.assertIn("@              IN NS    chlorine", pathlib.Path(folder, "db.akoria.net").read_text())
                        reverse = pathlib.Path(folder, "db.0.7.10.in-addr.arpa").read_text()
                        config = pathlib.Path(folder, "named.conf.akoria").read_text()
                        self.assertIn("@ IN NS chlorine.akoria.net.", reverse)
                        self.assertIn("allow-transfer { 10.7.0.10; }", config)
            finally:
                gen_zones.OUT = previous_out

    @unittest.skipUnless(shutil.which("named-checkzone") and shutil.which("named-checkconf"),
                         "BIND validation utilities are not installed")
    def test_source_tree_renders_zones_accepted_by_bind(self):
        with tempfile.TemporaryDirectory() as folder:
            previous_out, previous_zone_dir = gen_zones.OUT, gen_zones.ZONEDIR
            gen_zones.OUT = folder
            gen_zones.ZONEDIR = folder
            try:
                with contextlib.redirect_stdout(io.StringIO()):
                    run_generator()
                for zonefile in sorted(pathlib.Path(folder).glob("db.*")):
                    zone = zonefile.name[3:]
                    checked = subprocess.run(
                        ["named-checkzone", zone, str(zonefile)],
                        capture_output=True, text=True,
                    )
                    self.assertEqual(0, checked.returncode, checked.stdout + checked.stderr)
                checked_config = subprocess.run(
                    ["named-checkconf", str(pathlib.Path(folder, "named.conf.akoria"))],
                    capture_output=True, text=True,
                )
                self.assertEqual(0, checked_config.returncode,
                                 checked_config.stdout + checked_config.stderr)
            finally:
                gen_zones.OUT, gen_zones.ZONEDIR = previous_out, previous_zone_dir

    @staticmethod
    def _serials(folder):
        result = {}
        for path in pathlib.Path(folder).glob("db.*"):
            match = re.search(r"^\s*(\d+)\s*;\s*serial\s*$", path.read_text(), re.MULTILINE)
            if match:
                result[path.name] = int(match.group(1))
        return result


if __name__ == "__main__":
    unittest.main()
