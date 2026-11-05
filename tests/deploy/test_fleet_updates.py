#!/usr/bin/env python3
"""deploy/fleet/fleet-updates.py: roster, ordering and per-host log lines."""
import contextlib
import importlib.util
import io
import os
import subprocess
import sys
import types
import unittest
from unittest import mock

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.join(ROOT, "deploy", "fleet"))
_spec = importlib.util.spec_from_file_location(
    "fleet_updates", os.path.join(ROOT, "deploy", "fleet", "fleet-updates.py"))
fleet_updates = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(fleet_updates)


class Cursor:
    def __init__(self, rows): self.rows = rows
    def execute(self, *_args): pass
    def fetchall(self): return self.rows


def host(name, os_name, kind="server"):
    return {"id": hash(name) & 0xffff, "name": name, "entity_type": kind, "os": os_name}


def run_host(stdout, rc, mode="check"):
    done = subprocess.CompletedProcess([], rc, stdout=stdout.encode(), stderr=b"")
    out = io.StringIO()
    with mock.patch.object(fleet_updates.provlib, "remote_run", return_value=done) as run, \
            contextlib.redirect_stdout(out):
        failed = fleet_updates.run_host("/ansible", "h.example", None, mode, False)
    return failed, out.getvalue(), run


class Roster(unittest.TestCase):
    def test_distributions_only_and_core_last(self):
        managed, unknown = fleet_updates.load_roster(Cursor([
            host("xenon", "Ubuntu"), host("radium", "Debian"), host("cesium", "openSUSE"),
            host("benzene", "openSUSE"), host("chlorine", "Ubuntu", "appliance"),
            host("old", "Linux 4.14.111"), host("amino", None),
            host("valence", None, "appliance"), host("Bad_Name", "Debian"),
        ]))
        self.assertEqual(["chlorine", "radium", "cesium", "benzene", "xenon"],
                         [r["name"] for r in managed])
        # An appliance with no distribution is expected; a server with none is news.
        self.assertEqual(["Bad_Name", "amino", "old"], unknown)


class HostOutput(unittest.TestCase):
    def test_marker_lines_are_read_from_the_default_callback(self):
        text = 'ok: [h] => {\n    "msg": "SOLARI_UPDATE PENDING count=3 new=1 remove=0"\n}\n'
        self.assertEqual({"PENDING": "count=3 new=1 remove=0"}, fleet_updates.parse_host_output(text))

    def test_check_prints_pending(self):
        failed, out, _ = run_host('"msg": "SOLARI_UPDATE PENDING count=3 new=1 remove=0"\n', 0)
        self.assertEqual(0, failed)
        self.assertIn("PENDING h.example count=3 new=1 remove=0\n", out)
        self.assertNotIn("RESULT", out)

    def test_apply_prints_result(self):
        failed, out, run = run_host('"msg": "SOLARI_UPDATE PENDING count=3 new=1 remove=0"\n'
                                    '"msg": "SOLARI_UPDATE RESULT reboot_required=yes"\n', 0, "apply")
        self.assertEqual(0, failed)
        self.assertIn("RESULT h.example status=complete reboot_required=yes\n", out)
        self.assertEqual(fleet_updates.TIMEOUT["apply"], run.call_args.kwargs["timeout"])

    def test_skip_reason_has_no_trailing_quote(self):
        failed, out, _ = run_host('"msg": "SOLARI_UPDATE SKIPPED OS family Alpine is not managed"\n', 0)
        self.assertEqual(0, failed)
        self.assertEqual("HOST h.example\nSKIPPED h.example: OS family Alpine is not managed\n", out)

    def test_unreachable_is_skipped_not_failed(self):
        failed, out, _ = run_host("fatal: [h]: UNREACHABLE! => {}\n", 4)
        self.assertEqual(0, failed)
        self.assertIn("SKIPPED h.example: unreachable\n", out)

    def test_login_refused_says_so(self):
        failed, out, _ = run_host('fatal: [h]: UNREACHABLE! => {"msg": "jason@h: Permission denied (publickey)"}\n', 4)
        self.assertEqual(0, failed)
        self.assertIn("SKIPPED h.example: the control host's login was refused\n", out)

    def test_failure_reports_the_reason_and_counts(self):
        failed, out, _ = run_host('"msg": "SOLARI_UPDATE PENDING count=3 new=0 remove=0"\n'
                                  'fatal: [h]: FAILED! => {"changed": false, "msg": "dpkg was interrupted"}\n',
                                  2, "apply")
        self.assertEqual(1, failed)
        self.assertIn("RESULT h.example status=failed reboot_required=unknown\n", out)
        self.assertIn("h.example: dpkg was interrupted\n", out)


class Main(unittest.TestCase):
    def test_second_run_refuses_and_still_prints_the_exit_line(self):
        out = io.StringIO()
        with mock.patch.object(fleet_updates.provlib, "status_dir", return_value=os.environ.get("TMPDIR", "/tmp")), \
                mock.patch.object(fleet_updates.fcntl, "flock", side_effect=BlockingIOError), \
                mock.patch.object(fleet_updates, "run") as run, contextlib.redirect_stdout(out):
            status = fleet_updates.main(["--mode", "check", "--op", "alice"])
        self.assertEqual(3, status)
        run.assert_not_called()
        self.assertTrue(out.getvalue().endswith("FLEET_UPDATES exit=3\n"))


if __name__ == "__main__":
    unittest.main()
