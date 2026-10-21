#!/usr/bin/env python3
"""Regression coverage for safe, repeatable bare-zone secondary generation."""
import pathlib
import subprocess
import sys
import tempfile
import unittest


SCRIPT = pathlib.Path(__file__).with_name("gen-bare.py")


class BareZoneGeneratorTest(unittest.TestCase):
    def test_default_authorizes_only_the_active_khadas_secondary(self):
        with tempfile.TemporaryDirectory() as folder:
            root = pathlib.Path(folder)
            source = root / "db.akoria.net"
            output = root / "output"
            source.write_text("alpha IN A 10.0.0.21\n", encoding="utf-8")
            subprocess.run([sys.executable, str(SCRIPT), str(source), str(output)],
                           check=True, capture_output=True, text=True)
            config = (output / "named.conf.bare").read_text(encoding="utf-8")
            self.assertIn(
                "allow-transfer { 10.7.0.10; }; "
                "also-notify { 10.7.0.10; }; notify yes;",
                config,
            )
        self.assertNotIn("10.0.0.11", config)

    def test_secondary_acl_and_unchanged_serial_are_preserved(self):
        with tempfile.TemporaryDirectory() as folder:
            root = pathlib.Path(folder)
            source = root / "db.akoria.net"
            previous = root / "previous"
            output = root / "output"
            previous.mkdir()
            source.write_text(
                "alpha IN A 10.0.0.21\n"
                "alias IN CNAME alpha.akoria.net.\n",
                encoding="utf-8",
            )
            (previous / "db.bare-alpha").write_text(
                "$ORIGIN alpha.\n$TTL 300\n"
                "@ IN SOA xenon.akoria.net. hostmaster.akoria.net. (\n"
                "  2026102101 ; serial\n  3600 600 604800 300 )\n"
                "@ IN NS xenon.akoria.net.\n@ IN A 10.0.0.21\n",
                encoding="utf-8",
            )
            subprocess.run([
                sys.executable, str(SCRIPT), str(source), str(output),
                "--previous", str(previous), "--zone-dir", "/var/cache/bind/solarinet-secondary",
                "--transfer", "10.7.0.10", "--transfer", "10.0.0.11",
            ], check=True, capture_output=True, text=True)

            config = (output / "named.conf.bare").read_text(encoding="utf-8")
            expected = (
                "allow-transfer { 10.0.0.11; 10.7.0.10; }; "
                "also-notify { 10.0.0.11; 10.7.0.10; }; notify yes;"
            )
            self.assertEqual(2, config.count(expected))
            self.assertIn("2026102101 ; serial", (output / "db.bare-alpha").read_text())

    def test_changed_zone_data_advances_its_previous_serial(self):
        with tempfile.TemporaryDirectory() as folder:
            root = pathlib.Path(folder)
            source = root / "db.akoria.net"
            previous = root / "previous"
            output = root / "output"
            previous.mkdir()
            source.write_text("alpha IN A 10.0.0.22\n", encoding="utf-8")
            (previous / "db.bare-alpha").write_text(
                "$ORIGIN alpha.\n$TTL 300\n"
                "@ IN SOA xenon.akoria.net. hostmaster.akoria.net. (\n"
                "  2026102101 ; serial\n  3600 600 604800 300 )\n"
                "@ IN NS xenon.akoria.net.\n@ IN A 10.0.0.21\n",
                encoding="utf-8",
            )
            subprocess.run([
                sys.executable, str(SCRIPT), str(source), str(output),
                "--previous", str(previous), "--transfer", "10.7.0.10",
            ], check=True, capture_output=True, text=True)
            contents = (output / "db.bare-alpha").read_text(encoding="utf-8")
            self.assertIn("2026102102 ; serial", contents)
            self.assertIn("@ IN A 10.0.0.22", contents)

    def test_rejects_invalid_transfer_address(self):
        with tempfile.TemporaryDirectory() as folder:
            root = pathlib.Path(folder)
            source = root / "db.akoria.net"
            source.write_text("alpha IN A 10.0.0.21\n", encoding="utf-8")
            result = subprocess.run([
                sys.executable, str(SCRIPT), str(source), str(root / "output"),
                "--transfer", "not-an-ip",
            ], capture_output=True, text=True)
            self.assertNotEqual(0, result.returncode)


if __name__ == "__main__":
    unittest.main()
