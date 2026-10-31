#!/usr/bin/env python3
"""test_prov_store_migrate — deploy/fleet/prov-store-migrate.py on temp dirs.

No network, no DB, no root: an HTTP root and a "store" directory under a temp
dir, SOLARI_PROV_LOCAL=1 + --fstype none (the seam's directory check), plus
the refusals that must hold without the seam. After a migration the real
prov-store-guard.sh check must accept the result.
"""
from __future__ import annotations

import contextlib
import fcntl
import hashlib
import importlib.util
import io
import os
import shutil
import stat
import subprocess
import sys
import tempfile
import time
import unittest

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
FLEET = os.path.join(ROOT, "deploy", "fleet")
MIGRATE = os.path.join(FLEET, "prov-store-migrate.py")
GUARD = os.path.join(FLEET, "prov-store-guard.sh")
FETCH_SH = os.path.join(FLEET, "fetch-images.sh")
with open(GUARD, encoding="utf-8") as _fh:
    GUARD_TEXT = _fh.read()


def load_migrate():
    spec = importlib.util.spec_from_file_location("prov_store_migrate", MIGRATE)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def sha(path: str) -> str:
    with open(path, "rb") as fh:
        return hashlib.sha256(fh.read()).hexdigest()


def tree(top: str) -> dict[str, str]:
    """rel -> 'file:<sha>' | 'link:<target>' | 'dir' (no following links)."""
    out: dict[str, str] = {}
    for d, dirs, files in os.walk(top):
        for name in dirs + files:
            p = os.path.join(d, name)
            rel = os.path.relpath(p, top)
            if os.path.islink(p):
                out[rel] = "link:" + os.readlink(p)
            elif os.path.isdir(p):
                out[rel] = "dir"
            else:
                out[rel] = "file:" + sha(p)
    return out


class Migrate(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.mkdtemp(prefix="p6-store-")
        self.addCleanup(shutil.rmtree, self.tmp, True)
        self.netboot = os.path.join(self.tmp, "netboot")
        self.http = os.path.join(self.netboot, "http")
        self.store = os.path.join(self.tmp, "store")
        os.makedirs(self.store)
        inst = os.path.join(self.http, "installers")
        v = os.path.join(inst, "debian", "12", ".x86_64.v.ab12CD34")
        os.makedirs(v)
        for name, body in (("linux", b"kernel\n" * 1000), ("initrd.gz", os.urandom(300_000)),
                           ("SHA256SUMS.solari", b"sums\n")):
            with open(os.path.join(v, name), "wb") as fh:
                fh.write(body)
        os.symlink(".x86_64.v.ab12CD34", os.path.join(inst, "debian", "12", "x86_64"))
        open(os.path.join(inst, "debian", "12", ".x86_64.lock"), "w").close()   # fetch-images' lock
        with open(os.path.join(inst, "solari-firstboot.sh"), "w") as fh:
            fh.write("#!/bin/sh\necho firstboot\n")
        os.chmod(os.path.join(inst, "solari-firstboot.sh"), 0o755)
        os.makedirs(os.path.join(self.http, "images"))
        with open(os.path.join(self.http, "images", "node-arm64.img"), "wb") as fh:
            fh.write(os.urandom(64_000))
        for keep in ("ipxe", "configs"):                      # must stay local, untouched
            os.makedirs(os.path.join(self.http, keep))
            with open(os.path.join(self.http, keep, "x"), "w") as fh:
                fh.write(keep)
        self.env = dict(os.environ, SOLARI_PROV_LOCAL="1")
        for k in ("HTTP_ROOT", "PROV_STORE", "PROV_STORE_FSTYPE", "PROV_ROOT"):
            self.env.pop(k, None)
        self.orig = {sub: tree(os.path.join(self.http, sub)) for sub in ("installers", "images")}

    def run_m(self, *args, env=None, fstype="none"):
        argv = [sys.executable, MIGRATE, "--http-root", self.http, "--store", self.store]
        if fstype is not None:
            argv += ["--fstype", fstype]
        return subprocess.run([*argv, *args], env=env or self.env, capture_output=True, text=True,
                              timeout=120)

    def backups(self):
        return sorted(e for e in os.listdir(self.netboot) if ".pre-store." in e)

    def assert_migrated(self):
        for sub in ("installers", "images"):
            link = os.path.join(self.http, sub)
            self.assertTrue(os.path.islink(link), sub)
            self.assertEqual(os.readlink(link), os.path.join(self.store, sub))
            self.assertEqual(tree(os.path.join(self.store, sub)), self.orig[sub], sub)
        for keep in ("ipxe", "configs"):
            self.assertFalse(os.path.islink(os.path.join(self.http, keep)))
        self.assertEqual(sorted(os.listdir(self.http)), ["configs", "images", "installers", "ipxe"])

    def guard(self, mode="check"):
        script = GUARD_TEXT + '\nprov_store "$@"'
        return subprocess.run(["sh", "-c", script, "prov-store", mode, self.store, "none", self.http,
                               "installers", "images"], capture_output=True, text=True, timeout=60)

    # -- dry run -------------------------------------------------------------
    def test_dry_run_changes_nothing(self):
        before = tree(self.tmp)
        p = self.run_m()
        self.assertEqual(p.returncode, 0, p.stdout + p.stderr)
        self.assertIn("dry run", p.stdout)
        self.assertIn("would copy installers/debian/12/.x86_64.v.ab12CD34/linux", p.stdout)
        self.assertIn("would copy images/node-arm64.img", p.stdout)
        self.assertIn("nothing blocks --apply", p.stdout)
        self.assertNotIn("self-test passed", p.stdout)
        self.assertEqual(tree(self.tmp), before)

    # -- apply, idempotent re-run, guard accepts the result ----------------------
    def test_apply_then_rerun_then_guard(self):
        self.assertEqual(self.guard().returncode, 1)          # not migrated yet: the guard refuses
        p = self.run_m("--apply")
        self.assertEqual(p.returncode, 0, p.stdout + p.stderr)
        self.assertIn("store self-test passed", p.stdout)
        self.assertIn("verified", p.stdout)
        self.assert_migrated()
        self.assertEqual(os.readlink(os.path.join(self.store, "installers/debian/12/x86_64")),
                         ".x86_64.v.ab12CD34")              # in-tree link recreated as-is
        self.assertEqual(stat.S_IMODE(os.stat(os.path.join(self.store, "installers",
                                                           "solari-firstboot.sh")).st_mode) & 0o111,
                         0o111)
        # originals kept beside (not in) the web root, byte-identical
        b = self.backups()
        self.assertEqual([x.split(".pre-store.")[0] for x in b], [".images", ".installers"])
        for name in b:
            sub = name.split(".")[1]
            self.assertEqual(tree(os.path.join(self.netboot, name)), self.orig[sub])
        self.assertFalse([x for x in os.listdir(self.store) if x.startswith(".prov-migrate-selftest")])
        g = self.guard()
        self.assertEqual(g.returncode, 0, g.stderr)
        p = self.run_m("--apply")
        self.assertEqual(p.returncode, 0, p.stdout + p.stderr)
        self.assertIn("installers: already linked", p.stdout)
        self.assertIn("images: already linked", p.stdout)
        self.assertEqual(self.backups(), b)                  # nothing new kept
        p = self.run_m()                                     # dry run after: also clean
        self.assertEqual(p.returncode, 0, p.stdout + p.stderr)
        self.assertIn("all done", p.stdout)

    def test_resume_skips_identical_files(self):
        rel = "installers/debian/12/.x86_64.v.ab12CD34/linux"
        os.makedirs(os.path.dirname(os.path.join(self.store, rel)))
        shutil.copyfile(os.path.join(self.http, rel), os.path.join(self.store, rel))
        part = os.path.join(self.store, "installers/debian/12/.x86_64.v.ab12CD34/.initrd.gz.prov-migrate.part")
        with open(part, "wb") as fh:                         # an interrupted copy
            fh.write(b"half")
        p = self.run_m("--apply")
        self.assertEqual(p.returncode, 0, p.stdout + p.stderr)
        self.assertIn("1 already in the store", p.stdout)
        self.assertFalse(os.path.exists(part), "the interrupted temp file must be replaced")
        self.assert_migrated()

    def test_missing_sub_is_created_and_linked(self):
        shutil.rmtree(os.path.join(self.http, "images"))
        self.orig["images"] = {}
        p = self.run_m("--apply")
        self.assertEqual(p.returncode, 0, p.stdout + p.stderr)
        self.assertIn("images: no local tree; creating", p.stdout)
        self.assert_migrated()
        self.assertEqual(self.backups()[0].split(".pre-store.")[0], ".installers")
        self.assertEqual(len(self.backups()), 1)

    # -- refusals: nothing swapped ---------------------------------------------
    def assert_untouched(self):
        for sub in ("installers", "images"):
            self.assertFalse(os.path.islink(os.path.join(self.http, sub)), sub)
            self.assertEqual(tree(os.path.join(self.http, sub)), self.orig[sub])
        self.assertEqual(self.backups(), [])

    def test_conflict_swaps_nothing(self):
        rel = "images/node-arm64.img"
        os.makedirs(os.path.join(self.store, "images"))
        with open(os.path.join(self.store, rel), "wb") as fh:
            fh.write(b"something else")
        for args in ((), ("--apply",)):
            with self.subTest(args=args):
                p = self.run_m(*args)
                self.assertEqual(p.returncode, 1, p.stdout + p.stderr)
                self.assertIn("CONFLICT images/node-arm64.img", p.stdout)
                self.assertIn("nothing copied, nothing swapped (neither installers nor images)",
                              p.stderr)
                # preflight covers EVERY subtree: installers (conflict-free, planned first)
                # is neither copied nor swapped either
                self.assert_untouched()
                self.assertFalse(os.path.exists(os.path.join(self.store, "installers")))
        with open(os.path.join(self.store, rel), "rb") as fh:
            self.assertEqual(fh.read(), b"something else")
        self.assertEqual(os.listdir(self.store), ["images"])

    def test_escaping_and_absolute_links_refused(self):
        inst = os.path.join(self.http, "installers")
        for name, target in (("abs", "/etc/passwd"), ("up", "../../outside")):
            os.symlink(target, os.path.join(inst, name))
            self.orig["installers"] = tree(inst)
            p = self.run_m("--apply")
            self.assertEqual(p.returncode, 1, p.stdout + p.stderr)
            self.assertIn(f"REFUSED installers/{name}: symlink to {target} leaves the tree", p.stdout)
            os.unlink(os.path.join(inst, name))
            self.orig["installers"] = tree(inst)
        self.assertEqual(self.backups(), [])

    def test_fetch_lock_held_aborts(self):
        lock = os.path.join(self.http, "installers/debian/12/.x86_64.lock")
        with open(lock, "rb") as fh:
            fcntl.flock(fh.fileno(), fcntl.LOCK_EX)          # an image refresh is running
            p = self.run_m("--apply")
        self.assertEqual(p.returncode, 1, p.stdout + p.stderr)
        self.assertIn("is locked: an image refresh is running", p.stderr)
        self.assertFalse(os.path.islink(os.path.join(self.http, "installers")))
        self.assertEqual(tree(os.path.join(self.http, "installers")), self.orig["installers"])

    def test_concurrent_migrate_refused(self):
        with open(os.path.join(self.netboot, ".prov-store-migrate.lock"), "a+b") as fh:
            fcntl.flock(fh.fileno(), fcntl.LOCK_EX)
            p = self.run_m("--apply")
        self.assertEqual(p.returncode, 1, p.stdout + p.stderr)
        self.assertIn("maintenance lock", p.stderr)
        self.assertIn("another prov-store-migrate", p.stderr)
        self.assert_untouched()

    def test_helper_writing_into_store_blocks_migrate(self):
        # a re-run after a partial migration: installers is linked, images is not.
        # fetch-images into installers holds the maintenance lock SHARED while it
        # downloads; the migrate must refuse instead of swapping images under it.
        os.rename(os.path.join(self.http, "installers"), os.path.join(self.store, "installers"))
        os.symlink(os.path.join(self.store, "installers"), os.path.join(self.http, "installers"))
        bin_dir = os.path.join(self.tmp, "bin")
        os.makedirs(bin_dir)
        ready = os.path.join(self.tmp, "fetch-started")
        with open(os.path.join(bin_dir, "curl"), "w") as fh:
            fh.write(f"#!/bin/sh\necho started > {ready}\nexec sleep 60\n")
        os.chmod(os.path.join(bin_dir, "curl"), 0o755)
        env = dict(self.env, PATH=bin_dir + os.pathsep + os.environ["PATH"], HTTP_ROOT=self.http,
                   PROV_STORE=self.store, PROV_STORE_FSTYPE="none", PROV_ROOT=self.netboot,
                   BENZENE="p6-test-no-ssh")
        fetch = subprocess.Popen(["bash", FETCH_SH, "--dest", "installers/debian/13/x86_64",
                                  "--file", "linux", "https://example.invalid/linux", "0" * 64],
                                 env=env, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                                 start_new_session=True)
        self.addCleanup(fetch.wait)
        self.addCleanup(os.killpg, fetch.pid, 9)
        deadline = time.monotonic() + 30
        while not os.path.exists(ready) and fetch.poll() is None and time.monotonic() < deadline:
            time.sleep(0.05)
        self.assertTrue(os.path.exists(ready), "fetch-images never reached its download")
        p = self.run_m("--apply")
        self.assertEqual(p.returncode, 1, p.stdout + p.stderr)
        self.assertIn("maintenance lock", p.stderr)
        self.assertFalse(os.path.islink(os.path.join(self.http, "images")))
        self.assertEqual(tree(os.path.join(self.http, "images")), self.orig["images"])
        self.assertFalse(os.path.exists(os.path.join(self.store, "images")))
        self.assertEqual(self.backups(), [])

    def test_hung_store_touch_is_bounded(self):
        # touching the store never returns (stalled hard CIFS mount): refuse, do not hang
        bin_dir = os.path.join(self.tmp, "bin")
        os.makedirs(bin_dir)
        with open(os.path.join(bin_dir, "ls"), "w") as fh:
            fh.write("#!/bin/sh\nexec sleep 600\n")
        os.chmod(os.path.join(bin_dir, "ls"), 0o755)
        env = dict(self.env, PATH=bin_dir + os.pathsep + os.environ["PATH"], PROV_STORE_TIMEOUT="2")
        for args in ((), ("--apply",)):
            with self.subTest(args=args):
                t0 = time.monotonic()
                p = self.run_m(*args, env=env)
                self.assertLess(time.monotonic() - t0, 15)
                self.assertEqual(p.returncode, 1, p.stdout + p.stderr)
                self.assertIn("provisioning image store not responding", p.stderr)
                self.assert_untouched()
        self.assertEqual(os.listdir(self.store), [])
        p = self.run_m(env=dict(self.env, PROV_STORE_TIMEOUT="soon"))
        self.assertEqual(p.returncode, 1, p.stdout + p.stderr)
        self.assertIn("PROV_STORE_TIMEOUT", p.stderr)

    def test_swap_failure_reports_partial_outcome(self):
        # installers swaps, then swapping images fails: the error must say exactly that
        mod = load_migrate()
        real_swap = mod.swap_in

        def swap_in(src, dst, *, backup):
            if src.endswith("/images"):
                raise OSError(28, "No space left on device", src)
            real_swap(src, dst, backup=backup)

        mod.swap_in = swap_in
        out, err = io.StringIO(), io.StringIO()
        old_env = {k: os.environ.get(k) for k in ("SOLARI_PROV_LOCAL",)}
        os.environ["SOLARI_PROV_LOCAL"] = "1"
        try:
            with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
                rc = mod.main(["--http-root", self.http, "--store", self.store, "--fstype", "none",
                               "--apply"])
        finally:
            for k, v in old_env.items():
                if v is None:
                    os.environ.pop(k, None)
                else:
                    os.environ[k] = v
        msg = err.getvalue()
        self.assertEqual(rc, 1, out.getvalue() + msg)
        self.assertIn("PARTIAL: swapped installers ->", msg)
        self.assertIn("NOT swapped: images (still the local directory", msg)
        self.assertNotIn("nothing swapped", msg)
        self.assertTrue(os.path.islink(os.path.join(self.http, "installers")))
        self.assertFalse(os.path.islink(os.path.join(self.http, "images")))
        self.assertEqual(tree(os.path.join(self.http, "images")), self.orig["images"])
        self.assertEqual(tree(os.path.join(self.store, "images")), self.orig["images"])
        p = self.run_m("--apply")                            # a re-run finishes the job
        self.assertEqual(p.returncode, 0, p.stdout + p.stderr)
        self.assert_migrated()

    def test_store_not_mounted_and_usage_refusals(self):
        before = tree(self.tmp)
        p = self.run_m("--apply", fstype="cifs")             # a temp dir is no cifs mount
        self.assertEqual(p.returncode, 1, p.stdout + p.stderr)
        self.assertIn("provisioning image store not mounted", p.stderr)
        env = dict(self.env)
        env.pop("SOLARI_PROV_LOCAL")
        p = self.run_m("--apply", env=env)
        self.assertEqual(p.returncode, 2, p.stderr)
        self.assertIn("--fstype none is only allowed under SOLARI_PROV_LOCAL=1", p.stderr)
        p = subprocess.run([sys.executable, MIGRATE, "--http-root", self.http, "--store",
                            os.path.join(self.http, "store"), "--fstype", "none", "--apply"],
                           env=self.env, capture_output=True, text=True, timeout=60)
        self.assertEqual(p.returncode, 2, p.stderr)
        self.assertIn("may not live inside the HTTP root", p.stderr)
        for bad in (("--store", "rel/store"), ("--store", self.store + "/../x"), ("--fstype", "cifs;id")):
            with self.subTest(bad=bad):
                argv = [sys.executable, MIGRATE, "--http-root", self.http, "--store", self.store,
                        "--fstype", "none", "--apply"]
                argv[argv.index(bad[0]) + 1] = bad[1]
                p = subprocess.run(argv, env=self.env, capture_output=True, text=True, timeout=60)
                self.assertEqual(p.returncode, 2, p.stderr)
        p = self.run_m("--apply", fstype=None, env=dict(env))   # default fstype is cifs
        self.assertEqual(p.returncode, 1, p.stdout + p.stderr)
        self.assertIn("is not a cifs mount", p.stderr)
        self.assertEqual(tree(self.tmp), before)
        self.assert_untouched()

    def test_selftest_only(self):
        p = self.run_m("--selftest-only")
        self.assertEqual(p.returncode, 0, p.stdout + p.stderr)
        self.assertIn("store self-test passed", p.stdout)
        self.assertEqual(os.listdir(self.store), [])
        self.assert_untouched()

    # -- purge -----------------------------------------------------------------
    def test_purge_verifies_before_deleting(self):
        p = self.run_m("--purge", "--apply")
        self.assertEqual(p.returncode, 0, p.stdout + p.stderr)   # nothing kept yet: no-op
        self.assertEqual(self.run_m("--apply").returncode, 0)
        kept = self.backups()
        self.assertEqual(len(kept), 2)
        p = self.run_m("--purge")
        self.assertEqual(p.returncode, 0, p.stdout + p.stderr)
        self.assertIn("would purge .installers.pre-store.", p.stdout)
        self.assertEqual(self.backups(), kept)
        # the store copy goes bad: the backup is the only good copy -> kept
        victim = os.path.join(self.store, "images", "node-arm64.img")
        with open(victim, "rb") as fh:
            good = fh.read()
        with open(victim, "wb") as fh:
            fh.write(b"bitrot")
        p = self.run_m("--purge", "--apply")
        self.assertEqual(p.returncode, 1, p.stdout + p.stderr)
        self.assertIn("not every file is identical in the store", p.stderr)
        self.assertIn("purged .installers.pre-store.", p.stdout)   # per sub: installers verified fine
        self.assertEqual([b.split(".pre-store.")[0] for b in self.backups()], [".images"])
        with open(victim, "wb") as fh:
            fh.write(good)
        p = self.run_m("--purge", "--apply")
        self.assertEqual(p.returncode, 0, p.stdout + p.stderr)
        self.assertIn("purged .images.pre-store.", p.stdout)
        self.assertEqual(self.backups(), [])
        self.assert_migrated()

    def test_purge_refuses_when_not_linked(self):
        self.assertEqual(self.run_m("--apply").returncode, 0)
        link = os.path.join(self.http, "images")
        os.unlink(link)
        os.makedirs(link)
        p = self.run_m("--purge", "--apply")
        self.assertEqual(p.returncode, 1, p.stdout + p.stderr)
        self.assertIn("is not linked into the store; refusing to purge", p.stderr)
        self.assertTrue([b for b in self.backups() if b.startswith(".images.")])


if __name__ == "__main__":
    unittest.main(verbosity=2)
