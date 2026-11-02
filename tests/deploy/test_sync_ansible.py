#!/usr/bin/env python3
"""sync-ansible.sh: publish the release Ansible tree to the provisioning host.

A stub ssh runs the "remote" side locally, so no network is touched.
"""
import os
import shutil
import subprocess
import tempfile
import unittest

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
SCRIPT = os.path.join(ROOT, "deploy", "fleet", "sync-ansible.sh")

STUB_SSH = """#!/usr/bin/env bash
while [ $# -gt 0 ]; do case "$1" in -o) shift 2 ;; -*) shift ;; *) break ;; esac; done
[ "${1:-}" = "sync-test-benzene" ] || { echo "stub ssh: refusing host ${1:-?}" >&2; exit 97; }
shift
exec bash -c "$*"
"""


class SyncAnsibleTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.mkdtemp(prefix="sync-ansible-")
        bindir = os.path.join(self.tmp, "bin")
        os.makedirs(bindir)
        with open(os.path.join(bindir, "ssh"), "w") as fh:
            fh.write(STUB_SSH)
        os.chmod(os.path.join(bindir, "ssh"), 0o755)
        self.prov = os.path.join(self.tmp, "prov")
        self.dst = os.path.join(self.prov, "ansible")
        self.env = dict(os.environ, PATH=f"{bindir}:{os.environ['PATH']}",
                        BENZENE="sync-test-benzene", PROV_ROOT=self.prov)

    def tearDown(self):
        shutil.rmtree(self.tmp, ignore_errors=True)

    def run_sync(self, *args):
        return subprocess.run(["bash", SCRIPT, *args], env=self.env,
                              capture_output=True, text=True, timeout=60)

    def seed_remote(self):
        os.makedirs(os.path.join(self.dst, "inventory"))
        os.makedirs(os.path.join(self.dst, "roles", "gone-role"))
        with open(os.path.join(self.dst, "inventory", "hosts.yml"), "w") as fh:
            fh.write("site-local inventory\n")
        with open(os.path.join(self.dst, "ansible.cfg"), "w") as fh:
            fh.write("old\n")

    def test_publishes_tree_keeps_inventory_and_previous(self):
        self.seed_remote()
        p = self.run_sync()
        self.assertEqual(p.returncode, 0, p.stdout + p.stderr)
        src = os.path.join(ROOT, "deploy", "fleet", "ansible")
        with open(os.path.join(src, "playbooks", "site.yml")) as a, \
                open(os.path.join(self.dst, "playbooks", "site.yml")) as b:
            self.assertEqual(a.read(), b.read())
        with open(os.path.join(self.dst, "inventory", "hosts.yml")) as fh:
            self.assertEqual(fh.read(), "site-local inventory\n")
        self.assertFalse(os.path.exists(os.path.join(self.dst, "roles", "gone-role")))
        self.assertTrue(os.path.isdir(os.path.join(self.dst, "roles", "solarinet-client")))
        with open(os.path.join(self.prov, "ansible.prev", "ansible.cfg")) as fh:
            self.assertEqual(fh.read(), "old\n")
        self.assertTrue(os.path.isfile(os.path.join(self.dst, ".release")))
        self.assertFalse(os.path.exists(self.dst + ".new"))

    def test_first_publish_without_existing_tree(self):
        p = self.run_sync()
        self.assertEqual(p.returncode, 0, p.stdout + p.stderr)
        self.assertTrue(os.path.isfile(os.path.join(self.dst, "ansible.cfg")))
        self.assertFalse(os.path.exists(os.path.join(self.dst, "inventory")))

    def test_check_reports_drift_without_changing_remote(self):
        self.seed_remote()
        p = self.run_sync("--check")
        self.assertEqual(p.returncode, 0, p.stdout + p.stderr)
        self.assertIn("differ", p.stdout + p.stderr)
        with open(os.path.join(self.dst, "ansible.cfg")) as fh:
            self.assertEqual(fh.read(), "old\n")
        self.assertEqual(self.run_sync().returncode, 0)
        p = self.run_sync("--check")
        self.assertIn("in sync", p.stdout)

    def test_unsafe_prov_root_refused(self):
        self.env["PROV_ROOT"] = "/srv/x;rm -rf /"
        p = self.run_sync()
        self.assertNotEqual(p.returncode, 0)
        self.assertIn("unsafe PROV_ROOT", p.stderr)


if __name__ == "__main__":
    unittest.main()
