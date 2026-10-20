"""Prove stable DNS generations without reading or writing live BIND paths."""
import datetime
import pathlib
import re
import shutil
import subprocess
import sys
import tempfile
import unittest

from zone_serial import SERIAL_TOKEN, SOA_RE, stampZone

ROOT = pathlib.Path(__file__).resolve().parents[1]
TEMPLATE = ("$ORIGIN example.test.\n@ IN SOA ns.example.test. hostmaster.example.test. (\n"
            f" {SERIAL_TOKEN} ; serial\n 3600 600 604800 300 )\n"
            "@ IN NS ns.example.test.\nns IN A 192.0.2.1\n")


class SerialTest(unittest.TestCase):
    def test_unchanged_generation_never_consumes_serial(self):
        with tempfile.TemporaryDirectory() as folder:
            previous = pathlib.Path(folder) / "db.example.test"
            first = stampZone(TEMPLATE, previous, datetime.date(2026, 10, 20))
            previous.write_text(first, encoding="utf-8")
            self.assertEqual(first, stampZone(TEMPLATE, previous, datetime.date(2026, 10, 21)))
            changed = TEMPLATE.replace("192.0.2.1", "192.0.2.2")
            draft = stampZone(changed, previous, datetime.date(2026, 10, 20))
            self.assertEqual("2026102002", SOA_RE.search(draft).group(2))
            self.assertEqual(draft, stampZone(changed, previous, datetime.date(2026, 10, 20)))
            self.assertEqual(first, previous.read_text(encoding="utf-8"))

    def test_corrupt_applied_serial_is_not_silently_reset(self):
        with tempfile.TemporaryDirectory() as folder:
            previous = pathlib.Path(folder) / "db.example.test"
            previous.write_text("truncated zone", encoding="utf-8")
            with self.assertRaises(ValueError):
                stampZone(TEMPLATE, previous)

    def test_serial_wrap_uses_rfc1982_order(self):
        with tempfile.TemporaryDirectory() as folder:
            previous = pathlib.Path(folder) / "db.example.test"
            previous.write_text(TEMPLATE.replace(SERIAL_TOKEN, "4294967295"), encoding="utf-8")
            result = stampZone(TEMPLATE.replace("192.0.2.1", "192.0.2.2"), previous)
            newSerial = int(SOA_RE.search(result).group(2))
            self.assertTrue(0 < ((newSerial - 4294967295) & 0xffffffff) < 0x80000000)


class GenerationTest(unittest.TestCase):
    def render(self, output, previous):
        subprocess.run([sys.executable, str(ROOT / "netdb/gen-zones.py"),
                        "--source", "yaml", "--output", str(output),
                        "--previous", str(previous)], check=True, capture_output=True, text=True)
        subprocess.run([sys.executable, str(ROOT / "deploy/dns-bare/gen-bare.py"),
                        str(output / "db.akoria.net"), str(output),
                        "--previous", str(previous)], check=True, capture_output=True, text=True)

    def test_isolated_generations_match_and_do_not_copy_removed_files(self):
        with tempfile.TemporaryDirectory() as folder:
            folder = pathlib.Path(folder)
            first, second = folder / "first", folder / "second"
            self.render(first, folder / "absent")
            original = {path.name: path.read_bytes() for path in first.iterdir()}
            (first / "db.bare-obsolete").write_text("old retired zone", encoding="utf-8")
            self.render(second, first)
            actual = {path.name: path.read_bytes() for path in second.iterdir()}
            self.assertEqual(original, actual)
            self.assertFalse((second / "db.bare-obsolete").exists())
            for name, content in original.items():
                self.assertEqual(content, (first / name).read_bytes())
            if shutil.which("named-checkzone") is None:
                self.fail("named-checkzone is required to validate generated DNS")
            stanzas = (second / "named.conf.akoria").read_text() + (second / "named.conf.bare").read_text()
            zones = re.findall(r'zone "([^"]+)".*?file "([^"]+)"', stanzas)
            self.assertGreater(len(zones), 10)
            for origin, runtimePath in zones:
                run = subprocess.run(["named-checkzone", origin,
                                      str(second / pathlib.Path(runtimePath).name)],
                                     capture_output=True, text=True)
                self.assertEqual(0, run.returncode, run.stdout + run.stderr)
            candidate = folder / "named.conf"
            candidate.write_text(f'options {{ directory "{second}"; }};\n' +
                                 stanzas.replace("/etc/bind/zones", str(second)), encoding="utf-8")
            run = subprocess.run(["named-checkconf", "-z", str(candidate)],
                                 capture_output=True, text=True)
            self.assertEqual(0, run.returncode, run.stdout + run.stderr)

    def test_bare_cname_cycle_fails_before_writing(self):
        with tempfile.TemporaryDirectory() as folder:
            source = pathlib.Path(folder) / "forward"
            output = pathlib.Path(folder) / "output"
            source.write_text("a IN CNAME b\nb IN CNAME a\n", encoding="utf-8")
            run = subprocess.run([sys.executable, str(ROOT / "deploy/dns-bare/gen-bare.py"),
                                  str(source), str(output)], capture_output=True, text=True)
            self.assertNotEqual(0, run.returncode)
            self.assertFalse(output.exists())


if __name__ == "__main__":
    unittest.main()
