#!/usr/bin/env python3
"""Unit tests for the stack-items root installer
deploy/fleet/ansible/roles/stack-items/files/stage_install.py (stdlib
unittest, unprivileged: ownership is only asserted as root:root when run as
root; everything else — refusals, modes, inodes, atomic replace — is exercised).

Run: python3 tests/deploy/test_stage_install.py
"""
from __future__ import annotations

import importlib.util
import io
import os
import socket
import stat
import sys
import tempfile
import unittest
from contextlib import redirect_stderr, redirect_stdout

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
SCRIPT = os.path.join(ROOT, "deploy", "fleet", "ansible", "roles", "stack-items", "files", "stage_install.py")
_spec = importlib.util.spec_from_file_location("stage_install", SCRIPT)
si = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(si)


class StageInstallTest(unittest.TestCase):
    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory()
        self.t = os.path.realpath(self._tmp.name)
        self.prefix = os.path.join(self.t, "dest", "opt", "pkg")
        self.stage = os.path.join(self.t, "build", "_stage")
        self.sp = self.stage + self.prefix          # <stage><prefix>
        os.makedirs(self.sp)
        os.makedirs(self.prefix)

    def tearDown(self):
        self._tmp.cleanup()

    def put(self, rel, data="x\n", mode=0o644):
        p = os.path.join(self.sp, rel)
        os.makedirs(os.path.dirname(p), exist_ok=True)
        with open(p, "w") as f:
            f.write(data)
        os.chmod(p, mode)
        return p

    def run_main(self, *extra):
        out, err = io.StringIO(), io.StringIO()
        with redirect_stdout(out), redirect_stderr(err):
            rc = si.main(["--stage", self.stage, "--prefix", self.prefix, *extra])
        return rc, out.getvalue(), err.getvalue()

    def assert_refused(self, needle, *extra):
        rc, _out, err = self.run_main(*extra)
        self.assertEqual(rc, 1, err)
        self.assertIn(needle, err)
        return err

    # ---- normal tree ---------------------------------------------------------
    def test_normal_tree_installed(self):
        self.put("bin/tool", "#!/bin/sh\necho hi\n", 0o755)
        self.put("share/doc/README", "readme\n", 0o644)
        os.makedirs(os.path.join(self.sp, "lib", "empty"))
        self.put("lib/libx.so.1", "elf\n", 0o755)
        os.symlink("libx.so.1", os.path.join(self.sp, "lib", "libx.so"))
        os.symlink(self.prefix + "/bin/tool", os.path.join(self.sp, "bin", "tool-abs"))
        os.symlink("../bin/tool", os.path.join(self.sp, "share", "tool-rel"))
        rc, out, err = self.run_main()
        self.assertEqual(rc, 0, err)
        with open(os.path.join(self.prefix, "bin", "tool")) as f:
            self.assertEqual(f.read(), "#!/bin/sh\necho hi\n")
        self.assertEqual(stat.S_IMODE(os.stat(os.path.join(self.prefix, "bin", "tool")).st_mode), 0o755)
        self.assertEqual(stat.S_IMODE(os.stat(os.path.join(self.prefix, "share/doc/README")).st_mode), 0o644)
        self.assertTrue(os.path.isdir(os.path.join(self.prefix, "lib", "empty")))
        self.assertEqual(stat.S_IMODE(os.stat(os.path.join(self.prefix, "lib")).st_mode), 0o755)
        self.assertEqual(os.readlink(os.path.join(self.prefix, "lib", "libx.so")), "libx.so.1")
        self.assertEqual(os.readlink(os.path.join(self.prefix, "share", "tool-rel")), "../bin/tool")
        self.assertIn('"files": 3', out)
        self.assertIn('"symlinks": 3', out)
        if os.geteuid() == 0:
            st = os.stat(os.path.join(self.prefix, "bin", "tool"))
            self.assertEqual((st.st_uid, st.st_gid), (0, 0))
        # Nothing but the tree: no temp files left behind.
        left = [n for _d, _s, fs in os.walk(self.prefix) for n in fs if n.startswith(".stage-install.")]
        self.assertEqual(left, [])

    def test_existing_file_replaced_atomically_new_inode(self):
        self.put("bin/tool", "v2\n", 0o755)
        os.makedirs(os.path.join(self.prefix, "bin"))
        dst = os.path.join(self.prefix, "bin", "tool")
        with open(dst, "w") as f:
            f.write("v1\n")
        old_ino = os.stat(dst).st_ino
        keep = os.open(dst, os.O_RDONLY)            # an open reader keeps the old inode alive
        try:
            rc, out, err = self.run_main()
            self.assertEqual(rc, 0, err)
            self.assertNotEqual(os.stat(dst).st_ino, old_ino)
            self.assertEqual(os.read(keep, 10), b"v1\n")
        finally:
            os.close(keep)
        with open(dst) as f:
            self.assertEqual(f.read(), "v2\n")
        self.assertIn('"files_replaced": 1', out)

    def test_hardlink_in_stage_becomes_separate_files(self):
        a = self.put("bin/a", "same\n", 0o755)
        os.link(a, os.path.join(self.sp, "bin", "b"))
        rc, out, err = self.run_main()
        self.assertEqual(rc, 0, err)
        da, db = (os.stat(os.path.join(self.prefix, "bin", n)) for n in ("a", "b"))
        self.assertNotEqual(da.st_ino, db.st_ino)
        self.assertEqual((da.st_nlink, db.st_nlink), (1, 1))
        self.assertNotEqual(da.st_ino, os.stat(a).st_ino)
        self.assertIn('"hardlinked_sources": 2', out)

    def test_setuid_setgid_sticky_stripped(self):
        self.put("bin/suid", "s\n", 0o4755)
        self.put("bin/sgid", "g\n", 0o2755)
        self.put("share/sticky", "t\n", 0o1644)
        self.put("share/odd", "o\n", 0o600)
        self.put("share/grpx", "gx\n", 0o610)
        rc, _out, err = self.run_main()
        self.assertEqual(rc, 0, err)
        mode = lambda r: stat.S_IMODE(os.stat(os.path.join(self.prefix, r)).st_mode)  # noqa: E731
        self.assertEqual(mode("bin/suid"), 0o755)
        self.assertEqual(mode("bin/sgid"), 0o755)
        self.assertEqual(mode("share/sticky"), 0o644)
        self.assertEqual(mode("share/odd"), 0o644)
        self.assertEqual(mode("share/grpx"), 0o755)

    # ---- destination symlinks ------------------------------------------------
    def test_destination_symlink_component_refused(self):
        outside = os.path.join(self.t, "etc")
        os.makedirs(outside)
        os.symlink(outside, os.path.join(self.prefix, "lib"))       # prefix/lib -> /etc
        self.put("lib/evil.conf", "pwned\n")
        self.assert_refused("destination symlink")
        self.assertEqual(os.listdir(outside), [])

    def test_destination_symlink_deep_component_refused(self):
        outside = os.path.join(self.t, "etc")
        os.makedirs(outside)
        os.makedirs(os.path.join(self.prefix, "share"))
        os.symlink(outside, os.path.join(self.prefix, "share", "doc"))
        self.put("share/doc/x/evil", "pwned\n")
        self.assert_refused("destination symlink")
        self.assertEqual(os.listdir(outside), [])

    def test_prefix_itself_symlink_resolved_once(self):
        real = os.path.join(self.t, "real-prefix")
        os.makedirs(real)
        os.rmdir(self.prefix)
        os.symlink(real, self.prefix)
        self.put("bin/tool", "ok\n", 0o755)
        rc, _out, err = self.run_main()
        self.assertEqual(rc, 0, err)
        self.assertTrue(os.path.isfile(os.path.join(real, "bin", "tool")))

    def test_missing_prefix_refused(self):
        os.rmdir(self.prefix)
        self.put("bin/tool")
        self.assert_refused("must be an existing directory")

    def test_destination_file_where_dir_staged_refused(self):
        with open(os.path.join(self.prefix, "lib"), "w"):
            pass
        self.put("lib/x")
        self.assert_refused("staged a directory")

    # ---- staged entries ------------------------------------------------------
    def test_staged_symlink_escape_refused(self):
        for target in ("../../../../../../etc/passwd", "/etc/passwd", "../..", "../../pkg-sibling"):
            with self.subTest(target=target):
                p = os.path.join(self.sp, "lib", "evil")
                os.makedirs(os.path.dirname(p), exist_ok=True)
                if os.path.lexists(p):
                    os.unlink(p)
                os.symlink(target, p)
                err = self.assert_refused("target leaves the prefix")
                self.assertIn(target, err)
                self.assert_refused("target leaves the prefix", "--check")
                self.assertFalse(os.path.lexists(os.path.join(self.prefix, "lib", "evil")))

    def test_fifo_refused(self):
        self.put("bin/tool")
        os.mkfifo(os.path.join(self.sp, "bin", "pipe"))
        self.assert_refused("refusing staged fifo")
        self.assertFalse(os.path.exists(os.path.join(self.prefix, "bin")))

    def test_socket_refused(self):
        path = os.path.join(self.sp, "s")
        if len(path) > 100:
            self.skipTest("tmp path too long for AF_UNIX")
        s = socket.socket(socket.AF_UNIX)
        try:
            s.bind(path)
            self.assert_refused("refusing staged socket")
        finally:
            s.close()

    def test_foreign_owned_entry_refused(self):
        # Simulates a hardlink to a file owned by someone else (e.g. root's
        # /etc/shadow): every entry must belong to the stage owner.
        self.put("bin/tool")
        inst = si.Installer(self.stage, self.prefix, owner_uid=os.getuid() + 1)
        with self.assertRaises(si.Refused) as cm:
            inst.validate()
        self.assertIn("not the build user", str(cm.exception))

    def test_stage_root_symlink_refused(self):
        real = self.stage + "-real"
        os.rename(self.stage, real)
        os.symlink(real, self.stage)
        self.assert_refused("symlink or not a directory")

    def test_staged_prefix_component_symlink_refused(self):
        # <stage>/<...>/pkg is a symlink to somewhere else.
        elsewhere = os.path.join(self.t, "elsewhere")
        os.makedirs(os.path.join(elsewhere, "bin"))
        with open(os.path.join(elsewhere, "bin", "x"), "w"):
            pass
        os.rmdir(self.sp)
        os.symlink(elsewhere, self.sp)
        self.assert_refused("not a directory")

    def test_outside_prefix_refused(self):
        self.put("bin/tool")
        out = self.stage + os.path.join(self.t, "dest", "etc", "evil")
        os.makedirs(os.path.dirname(out))
        with open(out, "w"):
            pass
        err = self.assert_refused("staged files outside its prefix")
        self.assertIn("/etc/evil", err)
        self.assertEqual(os.listdir(self.prefix), [])

    def test_bare_ancestor_dirs_outside_prefix_tolerated(self):
        self.put("bin/tool")
        os.makedirs(self.stage + os.path.join(self.t, "dest", "opt", "empty-sibling"))
        rc, _out, err = self.run_main()
        self.assertEqual(rc, 0, err)

    def test_empty_stage_explains_destdir(self):
        for setup in ("empty-prefix-dir", "no-prefix-dir"):
            with self.subTest(setup=setup):
                if setup == "no-prefix-dir":
                    os.rmdir(self.sp)
                err = self.assert_refused("did not honour DESTDIR")
                self.assertIn("`script` item", err)

    def test_check_mode_writes_nothing_and_needs_no_prefix(self):
        os.rmdir(self.prefix)
        self.put("bin/tool", "x\n", 0o755)
        rc, _out, err = self.run_main("--check")
        self.assertEqual(rc, 0, err)
        self.assertFalse(os.path.exists(self.prefix))

    def test_unnormalised_paths_refused(self):
        for bad in ("relative/path", "/", self.prefix + "/../x"):
            with self.subTest(prefix=bad):
                with self.assertRaises(si.Refused):
                    si.Installer(self.stage, bad)


if __name__ == "__main__":
    unittest.main(verbosity=2)
