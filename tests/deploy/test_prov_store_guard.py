#!/usr/bin/env python3
"""test_prov_store_guard — prov-store-guard.sh and the writers that rely on it.

No network, no DB, no root: an HTTP root linking into a "store" directory
under a temp dir, SOLARI_PROV_LOCAL=1 + fstype none (the seam's directory
check). Covers:

  * the guard fails closed without timeout(1) and when touching the store
    hangs (a stalled hard CIFS mount), instead of blocking or passing;
  * the store maintenance lock (prov-store-migrate's) is honoured by the guard,
    by fetch-images.sh and by build-pi-image.sh, and held by them while they
    write;
  * build-pi-image.sh re-checks the store immediately before writing and
    writes to the RESOLVED store path, so HTTP_ROOT/images replaced by a real
    directory (local disk) after the first check gets nothing.
"""
from __future__ import annotations

import fcntl
import hashlib
import os
import shutil
import subprocess
import sys
import tempfile
import time
import unittest

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
FLEET = os.path.join(ROOT, "deploy", "fleet")
GUARD = os.path.join(FLEET, "prov-store-guard.sh")
FETCH_SH = os.path.join(FLEET, "fetch-images.sh")
BUILDER = os.path.join(FLEET, "pi", "build-pi-image.sh")

with open(GUARD, encoding="utf-8") as _fh:
    GUARD_TEXT = _fh.read()


def write_exe(path: str, body: str) -> None:
    with open(path, "w", encoding="utf-8") as fh:
        fh.write(body)
    os.chmod(path, 0o755)


def sha256_file(path: str) -> str:
    with open(path, "rb") as fh:
        return hashlib.sha256(fh.read()).hexdigest()


class StoreBase(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.mkdtemp(prefix="p6-guard-")
        self.addCleanup(shutil.rmtree, self.tmp, True)
        self.netboot = os.path.join(self.tmp, "netboot")
        self.http = os.path.join(self.netboot, "http")
        self.store = os.path.join(self.tmp, "store")
        self.lock = os.path.join(self.netboot, ".prov-store-migrate.lock")
        os.makedirs(self.http)
        for sub in ("installers", "images"):
            os.makedirs(os.path.join(self.store, sub))
            os.symlink(os.path.join(self.store, sub), os.path.join(self.http, sub))
        self.bin = os.path.join(self.tmp, "bin")
        os.makedirs(self.bin)
        self.env = dict(os.environ, SOLARI_PROV_LOCAL="1", HTTP_ROOT=self.http,
                        PROV_ROOT=os.path.join(self.tmp, "prov"), PROV_STORE=self.store,
                        PROV_STORE_FSTYPE="none", BENZENE="p6-test-no-ssh",
                        PROV_STORE_LOCK_WAIT="1")
        for k in ("PROV_STORE_TIMEOUT",):
            self.env.pop(k, None)

    def guard(self, mode="check", *, tail="", env=None, subs=("installers", "images")):
        script = GUARD_TEXT + '\nprov_store "$@"' + tail
        return subprocess.run(["sh", "-c", script, "prov-store", mode, self.store, "none", self.http,
                               *subs], env=env or self.env, capture_output=True, text=True,
                              timeout=120)

    def hold_lock(self, kind=fcntl.LOCK_EX):
        """Take the maintenance lock as prov-store-migrate --apply does."""
        fd = os.open(self.lock, os.O_RDONLY | os.O_CREAT, 0o644)
        self.addCleanup(os.close, fd)
        fcntl.flock(fd, kind | fcntl.LOCK_NB)
        return fd

    def lock_is_free_for_ex(self) -> bool:
        fd = os.open(self.lock, os.O_RDONLY | os.O_CREAT, 0o644)
        try:
            fcntl.flock(fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
            return True
        except OSError:
            return False
        finally:
            os.close(fd)


# ---------------------------------------------------------------------------
class Guard(StoreBase):
    def test_ok_sets_real_path_and_holds_shared_lock(self):
        # the check leaves PROV_STORE_REAL and fd 7 (shared lock) in the calling shell
        probe = os.path.join(self.tmp, "probe")
        tail = (f'\nrc=$?; printf "%s\\n" "$PROV_STORE_REAL" > {probe}; '
                f'flock -n -x {self.lock} true && echo free >> {probe} || echo held >> {probe}; exit $rc')
        p = self.guard(tail=tail)
        self.assertEqual(p.returncode, 0, p.stderr)
        with open(probe, encoding="utf-8") as fh:
            real, state = fh.read().split()
        self.assertEqual(real, os.path.realpath(self.store))
        self.assertEqual(state, "held")
        self.assertTrue(self.lock_is_free_for_ex())          # released when the shell exits

    def test_no_timeout_binary_fails_closed(self):
        # a PATH with every tool the guard uses EXCEPT timeout(1)
        for tool in ("ls", "findmnt", "readlink", "test", "sed", "awk", "df", "flock", "sh"):
            path = shutil.which(tool)
            if path:
                os.symlink(path, os.path.join(self.bin, tool))
        env = dict(self.env, PATH=self.bin)
        p = self.guard(env=env)
        self.assertEqual(p.returncode, 1, p.stdout + p.stderr)
        self.assertIn("timeout(1) is not installed", p.stderr)
        p = self.guard("status", env=env)
        self.assertEqual(p.returncode, 0, p.stderr)
        self.assertIn("ok=no", p.stdout)
        self.assertIn("reason=timeout(1) is not installed", p.stdout)

    def test_hung_store_touch_is_bounded_and_fails_closed(self):
        # `ls` on the store never returns (a stalled hard CIFS mount)
        write_exe(os.path.join(self.bin, "ls"), "#!/bin/sh\nexec sleep 600\n")
        env = dict(self.env, PATH=self.bin + os.pathsep + os.environ["PATH"], PROV_STORE_TIMEOUT="2")
        t0 = time.monotonic()
        p = self.guard(env=env)
        elapsed = time.monotonic() - t0
        self.assertEqual(p.returncode, 1, p.stdout + p.stderr)
        self.assertIn("provisioning image store not responding", p.stderr)
        self.assertLess(elapsed, 15)

    def test_maintenance_lock_refuses_check_not_status(self):
        self.hold_lock()
        p = self.guard()
        self.assertEqual(p.returncode, 1, p.stdout + p.stderr)
        self.assertIn("image store maintenance in progress", p.stderr)
        p = self.guard("status")
        self.assertEqual(p.returncode, 0, p.stderr)
        self.assertIn("ok=yes", p.stdout)                    # status reports the store, never locks


# ---------------------------------------------------------------------------
class FetchImages(StoreBase):
    def setUp(self):
        super().setUp()
        self.src = os.path.join(self.tmp, "upstream-linux")
        with open(self.src, "wb") as fh:
            fh.write(b"kernel\n" * 100)

    def fetch(self, env=None):
        args = ["--dest", "installers/debian/12/x86_64",
                "--file", "linux", f"file://{self.src}", sha256_file(self.src)]
        return subprocess.run(["bash", FETCH_SH, *args], env=env or self.env, capture_output=True,
                              text=True, timeout=120)

    def test_refuses_while_migration_holds_lock(self):
        self.hold_lock()
        p = self.fetch()
        self.assertEqual(p.returncode, 1, p.stdout + p.stderr)
        self.assertIn("image store maintenance in progress", p.stderr)
        self.assertEqual(os.listdir(os.path.join(self.store, "installers")), [])

    def test_holds_shared_lock_while_downloading(self):
        # curl reports whether a migrate could take the lock mid-download
        probe = os.path.join(self.tmp, "probe")
        real_curl = shutil.which("curl")
        write_exe(os.path.join(self.bin, "curl"),
                  f"#!/bin/sh\nflock -n -x {self.lock} true && echo free > {probe} "
                  f"|| echo held > {probe}\nexec {real_curl} \"$@\"\n")
        env = dict(self.env, PATH=self.bin + os.pathsep + os.environ["PATH"])
        p = self.fetch(env)
        self.assertEqual(p.returncode, 0, p.stderr)
        with open(probe, encoding="utf-8") as fh:
            self.assertEqual(fh.read().strip(), "held")

    def test_writes_into_store_even_if_link_is_swapped_mid_download(self):
        # HTTP_ROOT/installers becomes a real (local) directory during the download
        link = os.path.join(self.http, "installers")
        real_curl = shutil.which("curl")
        write_exe(os.path.join(self.bin, "curl"),
                  f"#!/bin/sh\nrm -f {link} && mkdir {link}\nexec {real_curl} \"$@\"\n")
        env = dict(self.env, PATH=self.bin + os.pathsep + os.environ["PATH"])
        p = self.fetch(env)
        self.assertEqual(p.returncode, 0, p.stdout + p.stderr)
        self.assertEqual(os.listdir(link), [], "nothing may land on the local disk")
        final = os.path.join(self.store, "installers/debian/12/x86_64")
        self.assertTrue(os.path.islink(final))
        self.assertEqual(sha256_file(os.path.join(final, "linux")), sha256_file(self.src))


# ---------------------------------------------------------------------------
class PiBuilder(StoreBase):
    """build-pi-image.sh up to its first root-only step (losetup, stubbed to fail)."""

    def setUp(self):
        super().setUp()
        write_exe(os.path.join(self.bin, "id"),
                  '#!/bin/sh\n[ "$1" = -u ] && { echo 0; exit 0; }\nexec /usr/bin/id "$@"\n')
        for tool in ("losetup", "mount", "umount"):
            write_exe(os.path.join(self.bin, tool), "#!/bin/sh\nexit 1\n")
        # truncate (the 1 GiB padding) is the last step before the image is
        # written: the hook runs a scenario there, then skips the padding
        write_exe(os.path.join(self.bin, "truncate"),
                  '#!/bin/sh\n[ -n "${STUB_TRUNCATE_HOOK:-}" ] && sh -c "$STUB_TRUNCATE_HOOK"\nexit 0\n')
        self.base = os.path.join(self.tmp, "base.img")
        with open(self.base, "wb") as fh:
            fh.write(os.urandom(64 * 1024))
        self.env.update(PATH=self.bin + os.pathsep + os.environ["PATH"],
                        SOLARI_ALLOW_UNVERIFIED_PROVISIONING="1")
        self.name = "pitest-arm64.img"

    def build(self, **env):
        e = dict(self.env)
        e.update(env)
        argv = ["bash", BUILDER, "--hostname", "pitest", "--arch", "arm64",
                "--ssh-pubkey", "ssh-ed25519 AAAATEST test", "--base-img", self.base,
                "--out", os.path.join(self.http, "images", self.name)]
        return subprocess.run(argv, env=e, capture_output=True, text=True, timeout=120)

    def test_linked_store_image_lands_in_store(self):
        p = self.build()
        self.assertNotEqual(p.returncode, 0)                 # losetup stub: stops right after the write
        self.assertIn("attaching loop device", p.stdout)
        out = os.path.join(self.store, "images", self.name)
        self.assertEqual(sha256_file(out), sha256_file(self.base), p.stdout + p.stderr)
        self.assertIn(f"store path {os.path.realpath(self.store)}/images/{self.name}", p.stdout)

    def test_unlinked_images_dir_refused_before_download(self):
        link = os.path.join(self.http, "images")
        os.unlink(link)
        os.makedirs(link)                                    # old layout: local disk
        p = self.build()
        self.assertEqual(p.returncode, 1, p.stdout + p.stderr)
        self.assertIn("does not resolve into the image store", p.stderr)
        self.assertNotIn("using local base image", p.stdout)
        self.assertEqual(os.listdir(link), [])

    def test_link_replaced_after_first_check_writes_nothing_locally(self):
        link = os.path.join(self.http, "images")
        p = self.build(STUB_TRUNCATE_HOOK=f"rm -f {link} && mkdir {link}")
        self.assertEqual(p.returncode, 1, p.stdout + p.stderr)
        self.assertIn("using local base image", p.stdout)    # the first check passed
        self.assertIn("does not resolve into the image store", p.stderr)
        self.assertEqual(os.listdir(link), [], "a multi-GB image must never land on the local disk")
        self.assertEqual(os.listdir(os.path.join(self.store, "images")), [])

    def test_refuses_during_migration_and_holds_lock_while_writing(self):
        fd = self.hold_lock()
        p = self.build()
        self.assertEqual(p.returncode, 1, p.stdout + p.stderr)
        self.assertIn("image store maintenance in progress", p.stderr)
        self.assertEqual(os.listdir(os.path.join(self.store, "images")), [])
        fcntl.flock(fd, fcntl.LOCK_UN)
        probe = os.path.join(self.tmp, "probe")
        p = self.build(STUB_TRUNCATE_HOOK=f"flock -n -x {self.lock} true && echo free > {probe} "
                                          f"|| echo held > {probe}")
        self.assertIn("attaching loop device", p.stdout)
        with open(probe, encoding="utf-8") as fh:
            self.assertEqual(fh.read().strip(), "held")

    def test_out_outside_store_and_fstype_none_refused(self):
        elsewhere = os.path.join(self.tmp, "elsewhere.img")
        argv = ["bash", BUILDER, "--hostname", "pitest", "--arch", "arm64",
                "--ssh-pubkey", "ssh-ed25519 AAAATEST test", "--base-img", self.base,
                "--out", elsewhere]
        p = subprocess.run(argv, env=self.env, capture_output=True, text=True, timeout=120)
        self.assertEqual(p.returncode, 1, p.stdout + p.stderr)
        self.assertIn("--out must be", p.stderr)
        self.assertFalse(os.path.exists(elsewhere))
        p = self.build(SOLARI_PROV_LOCAL="")                 # the fstype-none seam is test-only
        self.assertEqual(p.returncode, 1, p.stdout + p.stderr)
        self.assertIn("PROV_STORE_FSTYPE=none is only allowed under SOLARI_PROV_LOCAL=1", p.stderr)
        self.assertEqual(os.listdir(os.path.join(self.store, "images")), [])

if __name__ == "__main__":
    unittest.main(verbosity=2)
