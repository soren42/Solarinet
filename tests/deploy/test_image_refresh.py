#!/usr/bin/env python3
"""test_image_refresh — netboot images: netbootlib, fetch-images.sh,
image-refresh.py (IMAGE_REFRESH) and fleet-provision.sh's image/stack-vars path.

Fully offline. Upstream trees are tiny fixtures under
tests/deploy/fixtures/netboot/upstream/ in each distro's real checksum format,
reached through file:// URLs under SOLARI_PROV_LOCAL=1 (the test seam);
SOLARI_PROV_URL_REWRITE maps the seeded https upstreams onto them, so the SoR
rows and the rendered menus keep the real URLs. PROV_ROOT / HTTP_ROOT are temp
dirs. Nothing reaches ssh: fetch-images.sh / provlib run locally under the
seam, and fleet-provision.sh gets a stub `ssh` first on PATH that runs the
remote command locally and refuses any host but the test one, plus a stub
solariCtl SIGN socket.

image-refresh.py's SoR side runs against a SCRATCH MariaDB this test creates
and always drops (`sor_test_p3_images`: schema.sql + every migration in order;
refuses to run if it already exists), connecting as an ephemeral
SELECT/INSERT/UPDATE user (password via env / SQL on stdin, never argv).

Local:  passwordless `sudo -n mariadb`; STACKGEN_TEST_PYTHON=<python with
        pymysql> (e.g. /tmp/stackgen-venv/bin/python), default python3.
CI:     SOLARI_TEST_SOR_DB=1 MYSQL_HOST=127.0.0.1 [MYSQL_PORT] [MYSQL_USER]
        MYSQL_PWD=<root password of the disposable service container>;
        STACKGEN_TEST_USER_HOST=% when the client is not on loopback.
No DB reachable -> the SoR tests SKIP (the rest still run), unless
IMAGE_REFRESH_REQUIRE_DB=1. UPDATE_GOLDEN=1 rewrites the golden files.
"""
from __future__ import annotations

import datetime as dt
import hashlib
import importlib.util
import json
import os
import secrets
import shutil
import socket
import stat
import subprocess
import sys
import tempfile
import threading
import unittest
import urllib.parse

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
FLEET = os.path.join(ROOT, "deploy", "fleet")
SOR = os.path.join(ROOT, "netdb", "sor")
FIX = os.path.join(ROOT, "tests", "deploy", "fixtures", "netboot")
UP = os.path.join(FIX, "upstream")
GOLDEN = os.path.join(FIX, "golden")
FETCH_SH = os.path.join(FLEET, "fetch-images.sh")
IMAGE_REFRESH = os.path.join(FLEET, "image-refresh.py")
FLEET_PROVISION = os.path.join(FLEET, "fleet-provision.sh")
STATIC_BOOT = os.path.join(FLEET, "netboot", "ipxe", "boot.ipxe")
UPDATE_GOLDEN = os.environ.get("UPDATE_GOLDEN") == "1"

sys.path.insert(0, FLEET)
import netbootlib  # noqa: E402

_spec = importlib.util.spec_from_file_location("image_refresh", IMAGE_REFRESH)
image_refresh = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(image_refresh)

DEB_SRC = ("https://deb.debian.org/debian/dists/bookworm/main/installer-amd64/current/images/"
           "netboot/debian-installer/amd64/")
DEB_CK = "https://deb.debian.org/debian/dists/bookworm/main/installer-amd64/current/images/SHA256SUMS"
UBU_SRC = "https://releases.ubuntu.com/24.04/"
UBU_CK = "https://releases.ubuntu.com/24.04/SHA256SUMS"
TW_SRC = "https://download.opensuse.org/tumbleweed/repo/oss/boot/x86_64/loader/"
LEAP_SRC = "https://download.opensuse.org/distribution/leap/15.6/repo/oss/boot/x86_64/loader/"
BASE = "http://10.5.2.50:8080"
SHA_A, SHA_B, SHA_C = "a" * 64, "b" * 64, "c" * 64


def rewrite_env(ubuntu_tree: str = "ubuntu", debian_root: str | None = None) -> str:
    deb = debian_root or os.path.join(UP, "debian")
    return " ".join([
        f"https://deb.debian.org/debian/|file://{deb}/",
        f"https://releases.ubuntu.com/|file://{os.path.join(UP, ubuntu_tree)}/",
        f"https://download.opensuse.org/|file://{os.path.join(UP, 'opensuse')}/",
    ])


def row(**kw) -> dict:
    base = {"id": 1, "boot_method": "ipxe", "deleted_at": None, "release_label": None,
            "checksum_url": None, "version": None, "sha256": None, "fetched_at": None}
    base.update(kw)
    return base


DEB12 = row(id=3, distro="debian", family="debian", release_name="12", release_label="12 (bookworm)",
            arch="x86_64", source_url=DEB_SRC, checksum_url=DEB_CK,
            version="fetched-20261031", sha256=SHA_A, fetched_at="2026-10-31 10:00:00")
UBU = row(id=4, distro="ubuntu", family="debian", release_name="24.04", release_label="24.04 LTS (noble)",
          arch="x86_64", source_url=UBU_SRC, checksum_url=UBU_CK,
          version="24.04.5", sha256=SHA_B, fetched_at="2026-10-31 10:05:00")
TW = row(id=1, distro="opensuse", family="suse", release_name="tumbleweed", release_label="Tumbleweed",
         arch="x86_64", source_url=TW_SRC, version="fetched-20261031", sha256=SHA_C,
         fetched_at="2026-10-31 10:10:00")
LEAP_UNFETCHED = row(id=2, distro="opensuse", family="suse", release_name="15.6", release_label="Leap 15.6",
                     arch="x86_64", source_url=LEAP_SRC)
DEB13_DELETED = dict(DEB12, id=9, release_name="13", deleted_at="2026-10-30 00:00:00")
PI = row(id=5, distro="raspios", family="debian", release_name="bookworm", arch="arm64",
         boot_method="pi_netboot", sha256=SHA_A, fetched_at="2026-10-31 10:00:00", version="v1")


def golden(name: str, text: str, case: unittest.TestCase) -> None:
    path = os.path.join(GOLDEN, name)
    if UPDATE_GOLDEN:
        os.makedirs(GOLDEN, exist_ok=True)
        with open(path, "w", encoding="utf-8") as fh:
            fh.write(text)
    with open(path, encoding="utf-8") as fh:
        case.assertEqual(fh.read(), text, f"golden {name} differs (UPDATE_GOLDEN=1 to regenerate)")


def sha256_file(path: str) -> str:
    with open(path, "rb") as fh:
        return hashlib.sha256(fh.read()).hexdigest()


def tree_dirs(root: str) -> list[str]:
    out = []
    for dirpath, dirnames, _ in os.walk(root, followlinks=False):
        for d in dirnames:
            out.append(os.path.join(dirpath, d))
    for dirpath, _, filenames in os.walk(root):
        for f in filenames:
            p = os.path.join(dirpath, f)
            if os.path.islink(p):
                out.append(p)
    return out


# ===========================================================================
# netbootlib — pure rendering
# ===========================================================================
class NetbootLibTests(unittest.TestCase):
    MIXED = [UBU, LEAP_UNFETCHED, PI, DEB13_DELETED, TW, DEB12]

    def test_empty_menu_golden(self):
        golden("boot-empty.ipxe", netbootlib.render_boot_menu([], http_base=BASE), self)

    def test_mixed_menu_golden(self):
        golden("boot-mixed.ipxe", netbootlib.render_boot_menu(self.MIXED, http_base=BASE), self)

    def test_mixed_menu_only_fetched_rows(self):
        text = netbootlib.render_boot_menu(self.MIXED, http_base=BASE)
        for want in ("item debian-12-x86_64 ", "item ubuntu-24.04-x86_64 ",
                     "item opensuse-tumbleweed-x86_64 "):
            self.assertIn(want, text)
        for unwanted in ("15.6", "debian-13", "raspios", "arm64", "aarch64"):
            self.assertNotIn(unwanted, text)
        self.assertNotIn("/amd64", text.replace("live-server-amd64.iso", ""))
        self.assertIn("installers/debian/12/x86_64", text)
        self.assertIn("installers/ubuntu/24.04/x86_64", text)

    def test_menu_is_order_independent(self):
        a = netbootlib.render_boot_menu(self.MIXED, http_base=BASE)
        b = netbootlib.render_boot_menu(list(reversed(self.MIXED)), http_base=BASE)
        self.assertEqual(a, b)

    def test_suse_install_points_upstream(self):
        text = netbootlib.render_boot_menu([TW], http_base=BASE)
        self.assertIn(" install=https://download.opensuse.org/tumbleweed/repo/oss/ ", text)
        self.assertNotIn("installers/opensuse/tumbleweed/x86_64/repo", text)
        self.assertEqual(netbootlib.suse_install_repo(LEAP_UNFETCHED),
                         "https://download.opensuse.org/distribution/leap/15.6/repo/oss/")

    def test_ubuntu_iso_url(self):
        text = netbootlib.render_boot_menu([UBU], http_base=BASE)
        self.assertIn("iso-url=https://releases.ubuntu.com/24.04/ubuntu-24.04.5-live-server-amd64.iso", text)
        self.assertIn('"ds=nocloud-net;s=${base}/configs/ubuntu/${cfgkey}/"', text)

    def test_cfgkey_set_before_menu(self):
        text = netbootlib.render_boot_menu([DEB12], http_base=BASE)
        self.assertLess(text.index("set cfgkey ${net0/mac:hexhyp}"), text.index("choose "))

    def test_static_boot_ipxe_is_bootstrap_menu(self):
        with open(STATIC_BOOT, encoding="utf-8") as fh:
            self.assertEqual(fh.read(), netbootlib.bootstrap_menu())
        self.assertEqual(netbootlib.bootstrap_menu().replace("set base @@HTTP_BASE@@", f"set base {BASE}"),
                         netbootlib.render_boot_menu([], http_base=BASE))
        self.assertIn("No installer images fetched yet", netbootlib.bootstrap_menu())

    def test_mac_profile_golden(self):
        text = netbootlib.render_mac_profile(DEB12, mac="AA:BB:CC:00:11:22", hostname="node-7",
                                             http_base=BASE)
        golden("mac-debian-12-x86_64.ipxe", text, self)
        self.assertIn("set cfgkey aa-bb-cc-00-11-22", text)
        menu = netbootlib.render_boot_menu([DEB12], http_base=BASE)
        kernel = next(ln for ln in text.splitlines() if ln.startswith("kernel "))
        self.assertIn(kernel.replace("goto giveup", ""), menu.replace("goto retry", ""))

    def test_mac_profile_refuses_unfetched_and_non_ipxe(self):
        for bad in (LEAP_UNFETCHED, PI, DEB13_DELETED):
            with self.assertRaises(ValueError):
                netbootlib.render_mac_profile(bad, mac="aa-bb-cc-dd-ee-ff", hostname="n1", http_base=BASE)

    def test_image_dir(self):
        self.assertEqual(netbootlib.image_dir(DEB12), "installers/debian/12/x86_64")
        self.assertEqual(netbootlib.image_dir(TW), "installers/opensuse/tumbleweed/x86_64")
        with self.assertRaises(ValueError):
            netbootlib.image_dir(dict(DEB12, arch="amd64"))
        with self.assertRaises(ValueError):
            netbootlib.image_dir(dict(DEB12, release_name="../12"))

    def test_injection_rejected(self):
        evil = {
            "release_label": ["12\nshell", "x ${base}", "a;b", "x || shell", "ok\x00", "a'b"],
            "version": ["1.0 && shell", "1\nchain http://evil/x", "${x}", "", "a" * 65],
            "release_name": ["12 ", "12;x", "../x"],
            "source_url": ["https://evil.example/a b/", "https://evil.example/${x}/",
                           "https://evil.example/x\n/", "ftp://evil.example/x/"],
            "sha256": ["A" * 64, "z" * 64],
        }
        for field, values in evil.items():
            for v in values:
                r = dict(UBU if field == "source_url" else DEB12, **{field: v})
                with self.subTest(field=field, value=v):
                    with self.assertRaises(ValueError):
                        netbootlib.render_boot_menu([r], http_base=BASE)
                    with self.assertRaises(ValueError):
                        netbootlib.render_mac_profile(r, mac="aa-bb-cc-dd-ee-ff", hostname="n1",
                                                      http_base=BASE)
        tw_evil = dict(TW, source_url="https://evil.example/x;shell/boot/x86_64/loader/")
        with self.assertRaises(ValueError):
            netbootlib.render_boot_menu([tw_evil], http_base=BASE)
        for host in ("a;b", "x y", "-lead", "a" * 64, "n1\nshell", "${x}"):
            with self.subTest(hostname=host), self.assertRaises(ValueError):
                netbootlib.render_mac_profile(DEB12, mac="aa-bb-cc-dd-ee-ff", hostname=host, http_base=BASE)
        for mac in ("aa-bb-cc-dd-ee", "zz-bb-cc-dd-ee-ff", "aa-bb-cc-dd-ee-ff;x"):
            with self.subTest(mac=mac), self.assertRaises(ValueError):
                netbootlib.render_mac_profile(DEB12, mac=mac, hostname="n1", http_base=BASE)
        for base in ("http://x/ ; shell", "http://x/", "javascript:x", "http://x/${y}"):
            with self.subTest(base=base), self.assertRaises(ValueError):
                netbootlib.render_boot_menu([DEB12], http_base=base)

    def test_duplicate_rows_rejected(self):
        with self.assertRaises(ValueError):
            netbootlib.render_boot_menu([DEB12, dict(DEB12, id=99)], http_base=BASE)


# ===========================================================================
# fetch-images.sh — staged, verified, atomic
# ===========================================================================
DEB_DIR = os.path.join(UP, "debian/dists/bookworm/main/installer-amd64/current/images/netboot/"
                            "debian-installer/amd64")


def deb_args(kernel_sha: str | None = None, root: str = DEB_DIR) -> list[str]:
    k = os.path.join(root, "linux")
    i = os.path.join(root, "initrd.gz")
    return ["--dest", "installers/debian/12/x86_64",
            "--file", "linux", f"file://{k}", kernel_sha or sha256_file(k),
            "--file", "initrd.gz", f"file://{i}", sha256_file(os.path.join(DEB_DIR, "initrd.gz")),
            "--meta", "distro", "debian", "--meta", "release_name", "12", "--meta", "arch", "x86_64",
            "--meta", "version", "fetched-20261031"]


def fetch_lines(stdout: str) -> dict:
    out = {}
    for ln in stdout.splitlines():
        key, _, val = ln.partition(" ")
        if key in ("staged", "previous", "swapped"):
            out[key] = val
    return out


class FetchImagesTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.mkdtemp(prefix="p3c-fetch-")
        self.http = os.path.join(self.tmp, "http")
        self.env = dict(os.environ, SOLARI_PROV_LOCAL="1", HTTP_ROOT=self.http,
                        PROV_ROOT=os.path.join(self.tmp, "prov"), BENZENE="p3-test-no-ssh")

    def tearDown(self):
        shutil.rmtree(self.tmp, ignore_errors=True)

    def run_fetch(self, args, env=None):
        return subprocess.run(["bash", FETCH_SH, *args], env=env or self.env, capture_output=True,
                              text=True, timeout=120)

    def test_debian_happy_then_mismatch_keeps_previous(self):
        p = self.run_fetch(deb_args())
        self.assertEqual(p.returncode, 0, p.stderr)
        final = os.path.join(self.http, "installers/debian/12/x86_64")
        self.assertTrue(os.path.islink(final))
        manifest = os.path.join(final, "SHA256SUMS.solari")
        digest = sha256_file(manifest)
        self.assertIn(f"manifest-sha256 {digest}", p.stdout)
        with open(manifest, encoding="utf-8") as fh:
            names = [ln.split()[1] for ln in fh.read().splitlines()]
        self.assertEqual(names, ["initrd.gz", "linux"])
        check = subprocess.run(["sha256sum", "-c", "--quiet", "SHA256SUMS.solari"], cwd=final,
                               capture_output=True, text=True)
        self.assertEqual(check.returncode, 0, check.stdout + check.stderr)
        with open(os.path.join(final, "solari-image.json"), encoding="utf-8") as fh:
            meta = json.load(fh)
        self.assertEqual((meta["distro"], meta["release_name"], meta["arch"], meta["manifest_sha256"]),
                         ("debian", "12", "x86_64", digest))
        before = os.readlink(final)

        p = self.run_fetch(deb_args(kernel_sha="0" * 64))
        self.assertEqual(p.returncode, 1)
        self.assertIn("sha256 MISMATCH for linux", p.stderr)
        self.assertEqual(os.readlink(final), before)
        self.assertEqual(sha256_file(manifest), digest)
        leftovers = [e for e in os.listdir(os.path.dirname(final)) if e.startswith(".x86_64.v.")]
        self.assertEqual(leftovers, [before])          # the failed staging dir was removed

        p = self.run_fetch(deb_args())                 # a re-fetch swaps but KEEPS the old dir
        self.assertEqual(p.returncode, 0, p.stderr)
        new = os.readlink(final)
        self.assertNotEqual(new, before)
        self.assertEqual(fetch_lines(p.stdout), {"staged": new, "previous": before, "swapped": ""})
        self.assertTrue(os.path.isdir(os.path.join(os.path.dirname(final), before)))
        p = self.run_fetch(["--dest", "installers/debian/12/x86_64", "--prune", new])   # commit
        self.assertEqual(p.returncode, 0, p.stderr)
        self.assertEqual(os.readlink(final), new)
        self.assertEqual(self.versions(), [new])
        self.assertFalse([d for d in tree_dirs(self.http) if os.path.basename(d) == "amd64"])

    def versions(self, rel: str = "installers/debian/12") -> list[str]:
        return sorted(e for e in os.listdir(os.path.join(self.http, rel)) if e.startswith(".x86_64.v."))

    def test_rollback_restores_previous(self):
        dest = ["--dest", "installers/debian/12/x86_64"]
        final = os.path.join(self.http, "installers/debian/12/x86_64")
        p = self.run_fetch(deb_args())
        self.assertEqual(p.returncode, 0, p.stderr)
        first = fetch_lines(p.stdout)
        self.assertEqual(first["previous"], "-")
        old = os.readlink(final)
        old_manifest = sha256_file(os.path.join(final, "SHA256SUMS.solari"))
        p = self.run_fetch(deb_args())
        self.assertEqual(p.returncode, 0, p.stderr)
        new = os.readlink(final)
        # Refusals: prune/rollback only act on the version the symlink points at.
        for args in (["--prune", old], ["--rollback", new, old], ["--rollback", "-", old]):
            with self.subTest(args=args):
                q = self.run_fetch(dest + args)
                self.assertEqual(q.returncode, 1, q.stderr)
                self.assertIn("refusing", q.stderr)
                self.assertEqual(os.readlink(final), new)
                self.assertEqual(self.versions(), sorted([old, new]))
        for args in (["--prune", "../x"], ["--prune", ".arm64.v.abcdefgh"], ["--rollback", old, old],
                     ["--rollback", "x", new], ["--prune", new, "--file", "linux", "file:///x", "0" * 64]):
            with self.subTest(args=args):
                self.assertEqual(self.run_fetch(dest + args).returncode, 2)
        p = self.run_fetch(dest + ["--rollback", old, new])
        self.assertEqual(p.returncode, 0, p.stderr)
        self.assertEqual(os.readlink(final), old)
        self.assertEqual(self.versions(), [old])
        self.assertEqual(sha256_file(os.path.join(final, "SHA256SUMS.solari")), old_manifest)
        p = self.run_fetch(dest + ["--rollback", "-", old])    # first-ever fetch undone: nothing served
        self.assertEqual(p.returncode, 0, p.stderr)
        self.assertFalse(os.path.lexists(final))
        self.assertEqual(self.versions(), [])

    def test_foreign_symlink_target_refused(self):
        parent = os.path.join(self.http, "installers/debian/12")
        os.makedirs(os.path.join(parent, "handmade"))
        os.symlink("handmade", os.path.join(parent, "x86_64"))
        p = self.run_fetch(deb_args())
        self.assertEqual(p.returncode, 1)
        self.assertIn("not a version dir", p.stderr)
        self.assertEqual(os.readlink(os.path.join(parent, "x86_64")), "handmade")
        self.assertEqual(self.versions(), [])

    def test_ubuntu_tarball_extract(self):
        tb = os.path.join(UP, "ubuntu/24.04/ubuntu-24.04.5-netboot-amd64.tar.gz")
        p = self.run_fetch(["--dest", "installers/ubuntu/24.04/x86_64",
                            "--file", os.path.basename(tb), f"file://{tb}", sha256_file(tb),
                            "--extract", os.path.basename(tb), "amd64/linux", "linux",
                            "--extract", os.path.basename(tb), "amd64/initrd", "initrd"])
        self.assertEqual(p.returncode, 0, p.stderr)
        final = os.path.join(self.http, "installers/ubuntu/24.04/x86_64")
        self.assertEqual(sorted(os.listdir(final)), ["SHA256SUMS.solari", "initrd", "linux",
                                                     "solari-image.json"])

    def test_missing_tar_member_refused(self):
        tb = os.path.join(UP, "ubuntu/24.04/ubuntu-24.04.5-netboot-amd64.tar.gz")
        p = self.run_fetch(["--dest", "installers/ubuntu/24.04/x86_64",
                            "--file", "t.tar.gz", f"file://{tb}", sha256_file(tb),
                            "--extract", "t.tar.gz", "arm64/linux", "linux"])
        self.assertEqual(p.returncode, 1)
        self.assertFalse(os.path.exists(os.path.join(self.http, "installers/ubuntu/24.04/x86_64")))

    def test_argument_validation(self):
        k = f"file://{DEB_DIR}/linux"
        bad = [
            ["--dest", "installers/debian/12/amd64", "--file", "linux", k, SHA_A],
            ["--dest", "installers/debian/../../etc/x86_64", "--file", "linux", k, SHA_A],
            ["--dest", "/etc/x86_64", "--file", "linux", k, SHA_A],
            ["--dest", "installers/debian/12/x86_64", "--file", "../linux", k, SHA_A],
            ["--dest", "installers/debian/12/x86_64", "--file", "SHA256SUMS.solari", k, SHA_A],
            ["--dest", "installers/debian/12/x86_64", "--file", "linux", "https://x/$(id)", SHA_A],
            ["--dest", "installers/debian/12/x86_64", "--file", "linux", "https://x/a'b", SHA_A],
            ["--dest", "installers/debian/12/x86_64", "--file", "linux", k, "A" * 64],
            ["--dest", "installers/debian/12/x86_64", "--file", "linux", k, SHA_A,
             "--meta", "version", "1\"; rm -rf /"],
            ["--dest", "installers/debian/12/x86_64", "--file", "linux", k, SHA_A,
             "--meta", "notes", "x"],
            ["--dest", "installers/debian/12/x86_64", "--file", "t", k, SHA_A,
             "--extract", "t", "../../etc/passwd", "x"],
            ["--dest", "installers/debian/12/x86_64"],
        ]
        for args in bad:
            with self.subTest(args=args):
                p = self.run_fetch(args)
                self.assertEqual(p.returncode, 2, p.stderr)
        env = dict(self.env)
        env.pop("SOLARI_PROV_LOCAL")
        env["BENZENE"] = "p3-test-must-not-ssh"
        p = self.run_fetch(deb_args(), env=env)                  # file:// only under the seam
        self.assertEqual(p.returncode, 2)
        self.assertIn("disallowed URL", p.stderr)


# ===========================================================================
# image-refresh.py — plan building (no DB)
# ===========================================================================
class PlanTests(unittest.TestCase):
    TODAY = dt.date(2026, 10, 31)

    def setUp(self):
        self._saved = {k: os.environ.get(k) for k in ("SOLARI_PROV_LOCAL", "SOLARI_PROV_URL_REWRITE")}
        os.environ["SOLARI_PROV_LOCAL"] = "1"
        os.environ["SOLARI_PROV_URL_REWRITE"] = rewrite_env()

    def tearDown(self):
        for k, v in self._saved.items():
            if v is None:
                os.environ.pop(k, None)
            else:
                os.environ[k] = v

    def test_debian_plan(self):
        plan = image_refresh.build_plan(DEB12, self.TODAY)
        self.assertEqual([f[0] for f in plan["files"]], ["linux", "initrd.gz"])
        self.assertEqual(plan["files"][0][2], sha256_file(os.path.join(DEB_DIR, "linux")))
        self.assertEqual(plan["files"][0][1], DEB_SRC + "linux")
        self.assertEqual(plan["version"], "fetched-20261031")

    def test_ubuntu_plan_resolves_tarball_from_checksums(self):
        plan = image_refresh.build_plan(UBU, self.TODAY)
        self.assertEqual(plan["version"], "24.04.5")
        self.assertEqual(plan["files"], [("ubuntu-24.04.5-netboot-amd64.tar.gz",
                                          UBU_SRC + "ubuntu-24.04.5-netboot-amd64.tar.gz",
                                          sha256_file(os.path.join(UP, "ubuntu/24.04/"
                                                                   "ubuntu-24.04.5-netboot-amd64.tar.gz")))])
        self.assertEqual(plan["extracts"][0][1:], ("amd64/linux", "linux"))

    def test_opensuse_derives_repo_checksums(self):
        plan = image_refresh.build_plan(TW, self.TODAY)
        self.assertEqual(plan["checksum_url"], "https://download.opensuse.org/tumbleweed/repo/oss/CHECKSUMS")
        self.assertEqual([f[0] for f in plan["files"]], ["linux", "initrd"])

    def test_refusals(self):
        cases = {
            "no checksum source": dict(DEB12, checksum_url=None),
            "missing entry": dict(UBU),
            "arm64 has no entry": dict(DEB12, arch="arm64",
                                       source_url=DEB_SRC.replace("amd64/", "arm64/")),
            "unsupported distro": dict(DEB12, distro="fedora"),
            "source outside checksum dir": dict(DEB12, source_url="https://deb.debian.org/other/"),
        }
        os.environ["SOLARI_PROV_URL_REWRITE"] = rewrite_env(ubuntu_tree="ubuntu-upstream-format")
        for label, r in cases.items():
            with self.subTest(label), self.assertRaises(image_refresh.ProvError):
                image_refresh.build_plan(r, self.TODAY)
        os.environ.pop("SOLARI_PROV_LOCAL")
        with self.assertRaises(image_refresh.ProvError):
            image_refresh.build_plan(dict(DEB12, checksum_url=DEB_CK.replace("https:", "http:")), self.TODAY)

    def test_parse_checksums(self):
        sums = image_refresh.parse_checksums(f"{SHA_A}  ./a/b\n{SHA_B} *c.iso\njunk\n{SHA_A}  ./a/b\n")
        self.assertEqual(sums, {"a/b": SHA_A, "c.iso": SHA_B})
        with self.assertRaises(image_refresh.ProvError):
            image_refresh.parse_checksums(f"{SHA_A}  x\n{SHA_B}  ./x\n")

    def test_rewrite_only_under_seam(self):
        self.assertTrue(image_refresh.rewrite(DEB_CK).startswith("file://"))
        os.environ.pop("SOLARI_PROV_LOCAL")
        self.assertEqual(image_refresh.rewrite(DEB_CK), DEB_CK)


# ===========================================================================
# fleet-provision.sh — release-aware profile, stack vars, no menu clobber
# ===========================================================================
class _SignStub:
    """Minimal solariCtl SIGN responder on a unix socket."""

    def __init__(self, path: str):
        self.path, self.calls = path, []
        self.sock = socket.socket(socket.AF_UNIX)
        self.sock.bind(path)
        self.sock.listen(4)
        self.sock.settimeout(0.2)
        self.stop = False
        self.thread = threading.Thread(target=self._serve, daemon=True)
        self.thread.start()

    def _serve(self):
        while not self.stop:
            try:
                conn, _ = self.sock.accept()
            except (socket.timeout, OSError):
                continue
            with conn:
                buf = b""
                while b"\n" not in buf:
                    d = conn.recv(65536)
                    if not d:
                        break
                    buf += d
                self.calls.append(buf.decode(errors="replace").split(" ", 2)[:2])
                pem = "-----BEGIN CERTIFICATE-----\nTEST\n-----END CERTIFICATE-----\n"
                conn.sendall(f"OK cert={urllib.parse.quote(pem, safe='')}\n".encode())

    def close(self):
        self.stop = True
        self.thread.join(2)
        self.sock.close()


STUB_SSH = """#!/usr/bin/env bash
# test stub: run the "remote" command locally; refuse any host but the test one.
while [ $# -gt 0 ]; do case "$1" in -o) shift 2 ;; -*) shift ;; *) break ;; esac; done
[ "${1:-}" = "p3-test-benzene" ] || { echo "stub ssh: refusing host ${1:-?}" >&2; exit 97; }
shift
exec bash -c "$*"
"""


@unittest.skipUnless(shutil.which("openssl"), "openssl needed by fleet-provision.sh")
class FleetProvisionTests(unittest.TestCase):
    MAC = "02:00:5e:00:00:07"
    MACH = "02-00-5e-00-00-07"

    def setUp(self):
        self.tmp = tempfile.mkdtemp(prefix="p3c-fp-")
        self.http = os.path.join(self.tmp, "http")
        self.prov = os.path.join(self.tmp, "prov")
        bindir = os.path.join(self.tmp, "bin")
        home = os.path.join(self.tmp, "home")
        os.makedirs(bindir)
        os.makedirs(os.path.join(home, ".ssh"))
        with open(os.path.join(home, ".ssh", "id_ed25519.pub"), "w") as fh:
            fh.write("ssh-ed25519 AAAATEST p3-test\n")
        with open(os.path.join(bindir, "ssh"), "w") as fh:
            fh.write(STUB_SSH)
        os.chmod(os.path.join(bindir, "ssh"), 0o755)
        self.sign = _SignStub(os.path.join(self.tmp, "ctl.sock"))
        self.env = dict(os.environ, PATH=f"{bindir}:{os.environ['PATH']}", HOME=home,
                        BENZENE="p3-test-benzene", HTTP_ROOT=self.http, PROV_ROOT=self.prov,
                        HTTP_BASE=BASE, CTL_SOCK=self.sign.path,
                        SOLARI_ALLOW_UNVERIFIED_PROVISIONING="1")
        self.env.pop("SOLARI_PROV_LOCAL", None)
        self.vars = os.path.join(self.tmp, "vars.json")
        with open(self.vars, "w") as fh:
            json.dump({"solari_stacks": ["core"], "hostname": "node-7"}, fh)

    def tearDown(self):
        self.sign.close()
        shutil.rmtree(self.tmp, ignore_errors=True)

    def fetch_debian(self):
        env = dict(self.env, SOLARI_PROV_LOCAL="1")
        p = subprocess.run(["bash", FETCH_SH, *deb_args(),
                            "--meta", "source_url", DEB_SRC, "--meta", "release_label", "12 (bookworm)"],
                           env=env, capture_output=True, text=True, timeout=120)
        self.assertEqual(p.returncode, 0, p.stderr)

    def provision(self, *extra, release=True):
        argv = ["bash", FLEET_PROVISION, "--target", self.MAC, "--distro", "debian", "--arch", "amd64",
                "--hostname", "node-7", "--op", "p3-test", *extra]
        if release:
            argv += ["--release", "12"]
        return subprocess.run(argv, env=self.env, capture_output=True, text=True, timeout=120)

    def test_stages_profile_and_stack_vars_without_clobbering_menu(self):
        self.fetch_debian()
        menu = os.path.join(self.http, "ipxe", "boot.ipxe")
        os.makedirs(os.path.dirname(menu))
        with open(menu, "w") as fh:
            fh.write("#!ipxe\n# generated by image-refresh (sentinel)\n")
        p = self.provision("--stack-vars", self.vars)
        self.assertEqual(p.returncode, 0, p.stdout + p.stderr)
        with open(menu) as fh:
            self.assertIn("sentinel", fh.read())
        with open(os.path.join(self.http, "ipxe", f"mac-{self.MACH}.ipxe")) as fh:
            prof = fh.read()
        self.assertIn("set installer ${base}/installers/debian/12/x86_64", prof)
        self.assertIn(f"set base {BASE}", prof)
        self.assertIn("hostname : node-7", prof)
        self.assertNotIn("@@", prof)
        # stack vars stay operator-side (0600); never published in the web root
        pub = os.path.join(self.http, "configs", f"{self.MACH}.stack.json")
        priv = os.path.join(self.prov, "profiles", self.MACH, "stack.json")
        self.assertFalse(os.path.exists(pub), pub)
        self.assertEqual(stat.S_IMODE(os.stat(priv).st_mode), 0o600, priv)
        with open(priv) as fh:
            self.assertEqual(json.load(fh)["solari_stacks"], ["core"])
        with open(os.path.join(self.prov, "profiles", self.MACH, "profile.env")) as fh:
            env = fh.read()
        self.assertIn("release=12\n", env)
        self.assertIn("arch=x86_64\n", env)
        self.assertTrue(os.path.exists(os.path.join(self.http, "configs", f"{self.MACH}.preseed")))
        self.assertEqual(len(self.sign.calls), 1)
        self.assertFalse([d for d in tree_dirs(self.http) if os.path.basename(d) == "amd64"])

    def test_bootstraps_menu_when_absent_and_release_default_warns(self):
        self.fetch_debian()
        p = self.provision(release=False)
        self.assertEqual(p.returncode, 0, p.stdout + p.stderr)
        self.assertIn("--release not given; defaulting to debian 12", p.stderr)
        with open(os.path.join(self.http, "ipxe", "boot.ipxe")) as fh:
            self.assertEqual(fh.read(), netbootlib.render_boot_menu([], http_base=BASE))

    def test_unfetched_image_refused_before_minting(self):
        p = self.provision()
        self.assertEqual(p.returncode, 1)
        self.assertIn("has not been fetched", p.stderr)
        self.assertEqual(self.sign.calls, [])
        self.assertFalse(os.path.exists(os.path.join(self.http, "ipxe", f"mac-{self.MACH}.ipxe")))

    def test_bad_inputs_refused(self):
        self.fetch_debian()
        arr = os.path.join(self.tmp, "arr.json")
        with open(arr, "w") as fh:
            fh.write("[1, 2]")
        for extra, release in ((["--stack-vars", arr], True), (["--stack-vars", "/nonexistent"], True),
                               (["--release", "12;id"], False)):
            with self.subTest(extra=extra):
                p = self.provision(*extra, release=release)
                self.assertEqual(p.returncode, 1, p.stderr)
        self.assertEqual(self.sign.calls, [])

    def test_gate_still_first(self):
        env = dict(self.env)
        env.pop("SOLARI_ALLOW_UNVERIFIED_PROVISIONING")
        p = subprocess.run(["bash", FLEET_PROVISION, "--target", self.MAC, "--distro", "debian",
                            "--arch", "amd64", "--hostname", "node-7", "--op", "t"],
                           env=env, capture_output=True, text=True, timeout=60)
        self.assertEqual(p.returncode, 1)
        self.assertIn("F-07", p.stderr)
        self.assertEqual(self.sign.calls, [])


# ===========================================================================
# image-refresh.py end to end on a scratch SoR
# ===========================================================================
DB = "sor_test_p3_images"
LIVE_DBS = {"sor", "solarinet", "solarinet_stage", "sor_probe"}
PY = os.environ.get("STACKGEN_TEST_PYTHON", "python3")
CI_MODE = os.environ.get("SOLARI_TEST_SOR_DB") == "1"
DB_PORT = os.environ.get("MYSQL_PORT", "3306") if CI_MODE else "3306"
USER_HOST = os.environ.get("STACKGEN_TEST_USER_HOST", "127.0.0.1")
APP_USER = f"p3img_{os.getpid()}"
APP_PASS = secrets.token_hex(16)
assert DB not in LIVE_DBS and DB.startswith("sor_test_p3_")
ADMIN = (["mariadb", "--protocol=TCP", "--host=127.0.0.1", f"--port={DB_PORT}",
          f"--user={os.environ.get('MYSQL_USER', 'root')}"] if CI_MODE else ["sudo", "-n", "mariadb"])


def admin(sql: str, db: str | None = DB, check: bool = True) -> str:
    proc = subprocess.run(ADMIN + ["-N", "-B"] + ([db] if db else []), input=sql, capture_output=True,
                          text=True, timeout=300)
    if check and proc.returncode != 0:
        raise RuntimeError(f"admin SQL failed: {proc.stderr.strip()[:400]}")
    return proc.stdout.strip()


def db_available() -> bool:
    if CI_MODE and os.environ.get("MYSQL_HOST") != "127.0.0.1":
        raise SystemExit("refusing: CI mode needs MYSQL_HOST=127.0.0.1")
    try:
        p = subprocess.run(ADMIN + ["-N", "-B", "-e", "SELECT 1"], capture_output=True, text=True,
                           timeout=30, stdin=subprocess.DEVNULL)
        q = subprocess.run([PY, "-c", "import pymysql"], capture_output=True, timeout=30)
    except (OSError, subprocess.TimeoutExpired):
        return False
    return p.returncode == 0 and p.stdout.strip() == "1" and q.returncode == 0


class ImageRefreshDBTests(unittest.TestCase):
    created = user = False

    @classmethod
    def setUpClass(cls):
        if not db_available():
            if os.environ.get("IMAGE_REFRESH_REQUIRE_DB") == "1":
                raise RuntimeError("scratch MariaDB / pymysql not available")
            raise unittest.SkipTest("SKIP: no scratch MariaDB (sudo -n mariadb / CI env) or no pymysql in "
                                    "STACKGEN_TEST_PYTHON")
        if admin(f"SELECT 1 FROM information_schema.SCHEMATA WHERE SCHEMA_NAME='{DB}'", db=None):
            raise RuntimeError(f"refusing to run: database '{DB}' already exists — drop it yourself if it is scratch")
        cls.tmp = tempfile.mkdtemp(prefix="p3c-img-")
        try:
            admin(f"CREATE DATABASE `{DB}` CHARACTER SET utf8mb4 COLLATE utf8mb4_unicode_ci", db=None)
            cls.created = True
            files = [os.path.join(SOR, "schema.sql")] + [
                os.path.join(SOR, "migrations", m)
                for m in sorted(os.listdir(os.path.join(SOR, "migrations"))) if m.endswith(".sql")]
            for path in files:
                with open(path, encoding="utf-8") as fh:
                    p = subprocess.run(ADMIN + [DB], stdin=fh, capture_output=True, text=True, timeout=300)
                if p.returncode != 0:
                    raise RuntimeError(f"applying {os.path.basename(path)}: {p.stderr.strip()[:300]}")
            cls.n_migrations = len(files) - 1
            admin(f"CREATE USER '{APP_USER}'@'{USER_HOST}' IDENTIFIED BY '{APP_PASS}'", db=None)
            cls.user = True
            admin(f"GRANT SELECT, INSERT, UPDATE ON `{DB}`.* TO '{APP_USER}'@'{USER_HOST}'", db=None)
            admin("INSERT INTO prov_images (distro, family, release_name, release_label, arch, boot_method,"
                  " source_url, checksum_url, source_id) VALUES ('debian','debian','13','13 (trixie)',"
                  " 'x86_64','ipxe','https://deb.debian.org/debian/dists/trixie/main/installer-amd64/"
                  "current/images/netboot/debian-installer/amd64/', NULL,"
                  " (SELECT id FROM sources WHERE slug='provisioner'))")
        except BaseException:
            cls.tearDownClass()
            raise

    @classmethod
    def tearDownClass(cls):
        if cls.user:
            admin(f"DROP USER IF EXISTS '{APP_USER}'@'{USER_HOST}'", db=None, check=False)
        if cls.created:
            admin(f"DROP DATABASE IF EXISTS `{DB}`", db=None, check=False)
        shutil.rmtree(getattr(cls, "tmp", ""), ignore_errors=True)

    # -- helpers -------------------------------------------------------------
    @property
    def http(self) -> str:
        return os.path.join(self.tmp, "http")

    def image_id(self, distro: str, release: str, arch: str = "x86_64") -> int:
        return int(admin(f"SELECT id FROM prov_images WHERE distro='{distro}' AND release_name='{release}'"
                         f" AND arch='{arch}' AND deleted_at IS NULL"))

    def state(self, image_id: int) -> str:
        return admin(f"SELECT CONCAT_WS('|', IFNULL(version,'-'), IFNULL(sha256,'-'),"
                     f" IFNULL(fetched_at,'-')) FROM prov_images WHERE id={image_id}")

    def refresh(self, image_id, rewrite: str | None = None, op: str = "p3-test@solari",
                http: str | None = None):
        env = dict(os.environ, SOLARI_PROV_LOCAL="1", SOLARI_PROV_URL_REWRITE=rewrite or rewrite_env(),
                   HTTP_ROOT=http or self.http, PROV_ROOT=os.path.join(self.tmp, "prov"), HTTP_BASE=BASE,
                   BENZENE="p3-test-no-ssh", SOR_DB_HOST="127.0.0.1", SOR_DB_PORT=DB_PORT,
                   SOR_DB_NAME=DB, SOR_DB_USER=APP_USER, SOR_DB_PASS=APP_PASS)
        return subprocess.run([PY, IMAGE_REFRESH, "--image", str(image_id), "--op", op], env=env,
                              capture_output=True, text=True, timeout=300)

    def menu(self, http: str | None = None) -> str:
        with open(os.path.join(http or self.http, "ipxe", "boot.ipxe"), encoding="utf-8") as fh:
            return fh.read()

    def runs(self, image_id: int) -> list[dict]:
        cols = ("id", "mode", "entity_id", "image_id", "stack_slugs", "requested_by", "status", "dry_run",
                "started", "finished", "detail")
        out = admin("SELECT id, mode, IFNULL(entity_id,'NULL'), image_id, stack_slugs, requested_by, status,"
                    " dry_run, started_at IS NOT NULL, finished_at IS NOT NULL, IFNULL(detail,'')"
                    f" FROM prov_runs WHERE image_id={int(image_id)} ORDER BY id")
        return [dict(zip(cols, ln.split("\t"))) for ln in out.splitlines() if ln]

    def assert_run(self, image_id: int, status: str, *needles: str, n_before: int | None = None) -> dict:
        runs = self.runs(image_id)
        if n_before is not None:
            self.assertEqual(len(runs), n_before + 1, runs)
        run = runs[-1]
        self.assertEqual((run["mode"], run["entity_id"], run["image_id"], json.loads(run["stack_slugs"]),
                          run["requested_by"], run["status"], run["dry_run"], run["finished"]),
                         ("refresh", "NULL", str(image_id), [], "p3-test@solari", status, "0", "1"), run)
        for needle in needles:
            self.assertIn(needle, run["detail"])
        return run

    def versions(self, rel: str, http: str | None = None) -> list[str]:
        parent = os.path.join(http or self.http, os.path.dirname(rel))
        return sorted(e for e in os.listdir(parent) if e.startswith(".x86_64.v."))

    def snapshot(self, image_id: int, rel: str) -> tuple:
        final = os.path.join(self.http, rel)
        return (self.state(image_id), os.readlink(final), self.versions(rel),
                sha256_file(os.path.join(final, "SHA256SUMS.solari")), self.menu())

    def assert_recorded(self, image_id: int, rel: str, version: str):
        manifest = os.path.join(self.http, rel, "SHA256SUMS.solari")
        v, sha, fetched = self.state(image_id).split("|")
        self.assertEqual(v, version)
        self.assertEqual(sha, sha256_file(manifest))
        self.assertNotEqual(fetched, "-")
        check = subprocess.run(["sha256sum", "-c", "--quiet", "SHA256SUMS.solari"],
                               cwd=os.path.join(self.http, rel), capture_output=True, text=True)
        self.assertEqual(check.returncode, 0, check.stdout + check.stderr)

    # -- tests (ordered) -----------------------------------------------------
    def test_01_debian(self):
        today = dt.datetime.now(dt.timezone.utc).strftime("%Y%m%d")
        iid = self.image_id("debian", "12")
        p = self.refresh(iid)
        self.assertEqual(p.returncode, 0, p.stdout + p.stderr)
        self.assert_recorded(iid, "installers/debian/12/x86_64", f"fetched-{today}")
        menu = self.menu()
        self.assertIn("item debian-12-x86_64 Debian 12 (bookworm) - x86_64 - fetched-", menu)
        self.assertNotIn("ubuntu-", menu)
        self.assertIn("boot menu regenerated with 1 fetched image(s)", p.stdout)
        run = self.assert_run(iid, "succeeded", f"installed fetched-{today}", n_before=0)
        self.assertEqual(run["started"], "1")
        self.assertEqual(self.versions("installers/debian/12/x86_64"),
                         [os.readlink(os.path.join(self.http, "installers/debian/12/x86_64"))])
        p = self.refresh(iid)                              # a second refresh prunes the first version
        self.assertEqual(p.returncode, 0, p.stdout + p.stderr)
        self.assert_run(iid, "succeeded", "installed", n_before=1)
        self.assertEqual(len(self.versions("installers/debian/12/x86_64")), 1)

    def test_02_ubuntu(self):
        iid = self.image_id("ubuntu", "24.04")
        p = self.refresh(iid)
        self.assertEqual(p.returncode, 0, p.stdout + p.stderr)
        rel = "installers/ubuntu/24.04/x86_64"
        self.assert_recorded(iid, rel, "24.04.5")
        self.assertEqual(sorted(os.listdir(os.path.join(self.http, rel))),
                         ["SHA256SUMS.solari", "initrd", "linux", "solari-image.json"])
        menu = self.menu()
        self.assertIn("item ubuntu-24.04-x86_64 Ubuntu 24.04 LTS (noble) - x86_64 - 24.04.5", menu)
        self.assertIn("item debian-12-x86_64 ", menu)
        self.assert_run(iid, "succeeded", "installed 24.04.5")

    def test_03_tumbleweed(self):
        iid = self.image_id("opensuse", "tumbleweed")
        p = self.refresh(iid)
        self.assertEqual(p.returncode, 0, p.stdout + p.stderr)
        self.assertIn("checksum source https://download.opensuse.org/tumbleweed/repo/oss/CHECKSUMS", p.stdout)
        self.assert_recorded(iid, "installers/opensuse/tumbleweed/x86_64",
                             "fetched-" + dt.datetime.now(dt.timezone.utc).strftime("%Y%m%d"))
        menu = self.menu()
        self.assertIn(" install=https://download.opensuse.org/tumbleweed/repo/oss/ ", menu)
        self.assertNotIn("15.6", menu)                       # Leap is seeded but never fetched
        self.assertNotIn("arm64", menu)
        self.assertNotIn("raspios", menu)

    def test_04_mismatch_keeps_previous(self):
        iid = self.image_id("debian", "12")
        before_state = self.state(iid)
        final = os.path.join(self.http, "installers/debian/12/x86_64")
        before_link = os.readlink(final)
        before_manifest = sha256_file(os.path.join(final, "SHA256SUMS.solari"))
        bad_root = os.path.join(self.tmp, "bad-debian")
        shutil.copytree(os.path.join(UP, "debian"), bad_root)
        with open(os.path.join(bad_root, "dists/bookworm/main/installer-amd64/current/images/netboot/"
                                         "debian-installer/amd64/linux"), "ab") as fh:
            fh.write(b"tampered")
        n = len(self.runs(iid))
        p = self.refresh(iid, rewrite=rewrite_env(debian_root=bad_root))
        self.assertEqual(p.returncode, 1)
        self.assertIn("sha256 MISMATCH for linux", p.stderr)
        self.assertEqual(os.readlink(final), before_link)
        self.assertEqual(sha256_file(os.path.join(final, "SHA256SUMS.solari")), before_manifest)
        self.assertEqual(self.state(iid), before_state)
        self.assertEqual(self.versions("installers/debian/12/x86_64"), [before_link])
        self.assert_run(iid, "failed", "fetch-images.sh failed", n_before=n)

    def test_05_missing_checksum_entry(self):
        iid = self.image_id("ubuntu", "24.04")
        before = self.state(iid)
        p = self.refresh(iid, rewrite=rewrite_env(ubuntu_tree="ubuntu-upstream-format"))
        self.assertEqual(p.returncode, 1)
        self.assertIn("lists no ubuntu-24.04[.N]-netboot-amd64.tar.gz", p.stderr)
        self.assertEqual(self.state(iid), before)
        self.assert_run(iid, "failed", "lists no ubuntu-24.04")

    def test_06_no_checksum_source(self):
        iid = self.image_id("debian", "13")
        p = self.refresh(iid)
        self.assertEqual(p.returncode, 1)
        self.assertIn("no checksum_url", p.stderr)
        self.assertEqual(self.state(iid), "-|-|-")
        self.assert_run(iid, "failed", "no checksum_url", n_before=0)
        self.assertFalse(os.path.exists(os.path.join(self.http, "installers/debian/13")))

    def test_07_non_ipxe_refused(self):
        iid = self.image_id("raspios", "bookworm", "arm64")
        p = self.refresh(iid)
        self.assertEqual(p.returncode, 1)
        self.assertIn("boots via pi_netboot", p.stderr)
        self.assert_run(iid, "failed", "boots via pi_netboot", n_before=0)

    def test_08_usage_and_unknown(self):
        for args in (["--image", "0", "--op", "x"], ["--image", "1", "--op", "bad op"],
                     ["--image", "1;id", "--op", "x"], ["--image", "4294967296", "--op", "x"],
                     ["--op", "x"]):
            with self.subTest(args=args):
                p = subprocess.run([PY, IMAGE_REFRESH, *args], capture_output=True, text=True, timeout=60,
                                   env=dict(os.environ, SOR_DB_NAME="sor_test_p3_unused"))
                self.assertEqual(p.returncode, 2, p.stderr)
        p = self.refresh(999999)
        self.assertEqual(p.returncode, 1)
        self.assertIn("no live prov_images row", p.stderr)
        self.assertEqual(self.runs(999999), [])

    def test_09_disk_layout(self):
        self.assertFalse([d for d in tree_dirs(self.http) if os.path.basename(d) in ("amd64", "aarch64")])
        for rel in ("installers/debian/12/x86_64", "installers/ubuntu/24.04/x86_64",
                    "installers/opensuse/tumbleweed/x86_64"):
            self.assertTrue(os.path.islink(os.path.join(self.http, rel)), rel)
        self.assertEqual(stat.S_IMODE(os.stat(os.path.join(self.http, "ipxe", "boot.ipxe")).st_mode), 0o644)
        items = [ln.split()[1] for ln in self.menu().splitlines()
                 if ln.startswith("item ") and not ln.startswith("item --gap")]
        self.assertEqual(items, ["debian-12-x86_64", "opensuse-tumbleweed-x86_64", "ubuntu-24.04-x86_64",
                                 "shell", "reboot", "local"])

    # -- F4: a failure after the swap leaves the OLD image serving -----------
    def test_10_sor_update_failure_keeps_old(self):
        iid = self.image_id("debian", "12")
        rel = "installers/debian/12/x86_64"
        before = self.snapshot(iid, rel)
        n = len(self.runs(iid))
        admin("DELIMITER //\nCREATE TRIGGER trg_test_refresh_fail BEFORE UPDATE ON prov_images FOR EACH ROW"
              f" BEGIN IF NEW.id = {iid} THEN SIGNAL SQLSTATE '45000'"
              " SET MESSAGE_TEXT = 'injected SoR failure'; END IF; END//\nDELIMITER ;\n")
        try:
            p = self.refresh(iid)
        finally:
            admin("DROP TRIGGER IF EXISTS trg_test_refresh_fail")
        self.assertEqual(p.returncode, 1, p.stdout + p.stderr)
        self.assertIn("injected SoR failure", p.stderr)
        self.assertIn("old image kept", p.stderr)
        self.assertEqual(self.snapshot(iid, rel), before)   # SoR, symlink, version dirs, files, menu
        self.assert_run(iid, "failed", "injected SoR failure", "old image kept", n_before=n)

    def _readonly_menu_dir(self, http: str):
        ipxe = os.path.join(http, "ipxe")
        os.makedirs(ipxe, exist_ok=True)
        os.chmod(ipxe, 0o555)
        self.addCleanup(os.chmod, ipxe, 0o755)

    def test_11_menu_failure_keeps_old(self):
        if os.geteuid() == 0:
            self.skipTest("root ignores the read-only menu dir used to inject the failure")
        iid = self.image_id("ubuntu", "24.04")
        rel = "installers/ubuntu/24.04/x86_64"
        before = self.snapshot(iid, rel)
        n = len(self.runs(iid))
        self._readonly_menu_dir(self.http)
        p = self.refresh(iid)
        self.assertEqual(p.returncode, 1, p.stdout + p.stderr)
        self.assertIn("recorded image", p.stdout)             # the SoR row WAS updated, then restored
        self.assertIn("restored image", p.stdout)
        self.assertIn("old image kept", p.stderr)
        self.assertEqual(self.snapshot(iid, rel), before)
        self.assert_run(iid, "failed", "old image kept", n_before=n)

    def test_12_first_fetch_menu_failure_serves_nothing(self):
        if os.geteuid() == 0:
            self.skipTest("root ignores the read-only menu dir used to inject the failure")
        iid = self.image_id("debian", "12")
        before_state = self.state(iid)
        fresh = os.path.join(self.tmp, "http-fresh")
        self._readonly_menu_dir(fresh)
        p = self.refresh(iid, http=fresh)
        self.assertEqual(p.returncode, 1, p.stdout + p.stderr)
        self.assertIn("no image installed", p.stderr)
        self.assertFalse(os.path.lexists(os.path.join(fresh, "installers/debian/12/x86_64")))
        self.assertEqual(self.versions("installers/debian/12/x86_64", http=fresh), [])
        self.assertEqual(self.state(iid), before_state)
        self.assert_run(iid, "failed", "no image installed")

    # -- F6: lock + supersede ------------------------------------------------
    def test_13_concurrent_refresh_refused(self):
        iid = self.image_id("debian", "12")
        n = len(self.runs(iid))
        holder = subprocess.Popen(ADMIN + ["-N", "-B", "--unbuffered", DB], stdin=subprocess.PIPE,
                                  stdout=subprocess.PIPE, text=True)
        try:
            holder.stdin.write(f"SELECT GET_LOCK('solari-prov-refresh-{iid}', 5);\n")
            holder.stdin.flush()
            self.assertEqual(holder.stdout.readline().strip(), "1")
            p = self.refresh(iid)
        finally:
            holder.stdin.close()
            holder.wait(timeout=30)
        self.assertEqual(p.returncode, 1)
        self.assertIn("another refresh of this image is running", p.stderr)
        self.assertEqual(len(self.runs(iid)), n)              # refused before a run was opened

    def test_14_orphaned_run_superseded(self):
        iid = self.image_id("debian", "12")
        admin("INSERT INTO prov_runs (mode, entity_id, image_id, stack_slugs, requested_by, status,"
              " source_id, asserted_kind) VALUES ('refresh', NULL, %d, '[]', 'p3-test@solari', 'queued',"
              " (SELECT id FROM sources WHERE slug='provisioner'), 'human')" % iid)
        admin(f"UPDATE prov_runs SET status='running' WHERE image_id={iid} AND status='queued'")
        stale = self.runs(iid)[-1]
        self.assertEqual(stale["status"], "running")
        p = self.refresh(iid)
        self.assertEqual(p.returncode, 0, p.stdout + p.stderr)
        runs = {r["id"]: r for r in self.runs(iid)}
        new = self.assert_run(iid, "succeeded", "installed")
        self.assertEqual(runs[stale["id"]]["status"], "cancelled")
        self.assertEqual(runs[stale["id"]]["detail"], f"superseded by run {new['id']}")
        self.assertFalse([r for r in runs.values() if r["status"] in ("queued", "running")])

    def test_15_sigterm_mid_fetch_closes_run(self):
        iid = self.image_id("debian", "12")
        rel = "installers/debian/12/x86_64"
        before = self.snapshot(iid, rel)
        n = len(self.runs(iid))
        hang_root = os.path.join(self.tmp, "hang-debian")
        shutil.copytree(os.path.join(UP, "debian"), hang_root)
        fifo = os.path.join(hang_root, "dists/bookworm/main/installer-amd64/current/images/netboot/"
                                       "debian-installer/amd64/linux")
        os.unlink(fifo)
        os.mkfifo(fifo)                                     # curl blocks reading it: a hung download
        env = dict(os.environ, SOLARI_PROV_LOCAL="1", SOLARI_PROV_URL_REWRITE=rewrite_env(debian_root=hang_root),
                   HTTP_ROOT=self.http, PROV_ROOT=os.path.join(self.tmp, "prov"), HTTP_BASE=BASE,
                   BENZENE="p3-test-no-ssh", SOR_DB_HOST="127.0.0.1", SOR_DB_PORT=DB_PORT,
                   SOR_DB_NAME=DB, SOR_DB_USER=APP_USER, SOR_DB_PASS=APP_PASS)
        proc = subprocess.Popen([PY, IMAGE_REFRESH, "--image", str(iid), "--op", "p3-test@solari"], env=env,
                                stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        try:
            deadline = dt.datetime.now() + dt.timedelta(seconds=60)
            while dt.datetime.now() < deadline:
                runs = self.runs(iid)
                if len(runs) > n and runs[-1]["status"] == "running":
                    break
            else:
                self.fail("refresh run never reached running")
            threading.Event().wait(1.5)                     # let curl block on the fifo
            proc.terminate()
            out, err = proc.communicate(timeout=60)
        finally:
            if proc.poll() is None:
                proc.kill()
        self.assertEqual(proc.returncode, 1, out + err)
        self.assert_run(iid, "failed", "SIGTERM", n_before=n)
        with self.assertRaises(OSError):                   # no fetch child left reading the fifo
            os.close(os.open(fifo, os.O_WRONLY | os.O_NONBLOCK))
        self.assertEqual(self.snapshot(iid, rel)[:2], before[:2])
        self.assertEqual(self.snapshot(iid, rel)[3:], before[3:])


if __name__ == "__main__":
    unittest.main(verbosity=2)
