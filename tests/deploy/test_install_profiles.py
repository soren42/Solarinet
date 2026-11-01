#!/usr/bin/env python3
"""test_install_profiles — rendered unattended-install configs, end to end.

Runs the REAL deploy/fleet/fleet-provision.sh (stub ssh that executes the
"remote" side locally, a stub solariCtl SIGN socket, a hand-written
solari-image.json in place of a fetch) for openSUSE Tumbleweed, Debian 12 and
Ubuntu 24.04, then parses what it rendered:

  * AutoYaST: well-formed XML; exactly one <package> per name (SUSE names, no
    space-joined Debian list); both the operator and the provisioning-host
    control key as separate authorized_keys <listentry>s; the run-id marker
    (/etc/ansible/facts.d/solari_prov.fact) in the chroot script;
  * Ubuntu autoinstall: valid YAML (if PyYAML is importable); packages and
    authorized-keys are lists of single items; the marker late-command;
  * Debian preseed: pkgsel/include is the bootstrap set; both keys; marker.

No network, no DB. Needs bash + openssl + python3.
"""
from __future__ import annotations

import json
import os
import re
import shutil
import socket
import stat
import subprocess
import tempfile
import threading
import unittest
import urllib.parse
import xml.etree.ElementTree as ET

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
FLEET_PROVISION = os.path.join(ROOT, "deploy", "fleet", "fleet-provision.sh")
BASE = "http://10.5.2.50:8080"
MAC = "02:00:5e:00:00:31"
MACH = "02-00-5e-00-00-31"
OPERATOR_KEY = "ssh-ed25519 AAAAOPERATORKEY operator@xenon"
CONTROL_KEY = "ssh-ed25519 AAAACONTROLKEY+/= jason@benzene"
SUSE_BOOTSTRAP = ["openssh", "sudo", "curl", "ca-certificates"]
DEB_BOOTSTRAP = ["openssh-server", "sudo", "curl", "ca-certificates", "python3"]
NS = {"a": "http://www.suse.com/1.0/yast2ns"}
MARKER = "/etc/ansible/facts.d/solari_prov.fact"

IMAGES = {
    "opensuse": ("tumbleweed", "https://download.opensuse.org/tumbleweed/repo/oss/boot/x86_64/loader/"),
    "debian": ("12", "https://deb.debian.org/debian/dists/bookworm/main/installer-amd64/current/"
                     "images/netboot/debian-installer/amd64/"),
    "ubuntu": ("24.04", "https://releases.ubuntu.com/24.04/"),
}
VERSIONS = {"ubuntu": "24.04.3"}

STUB_SSH = """#!/usr/bin/env bash
# test stub: run the "remote" command locally; refuse any host but the test one.
# STUB_SSH_FAIL_ON=<text>: fail (without running) any remote command containing it.
# STUB_SSH_SWAP_ON=<text> + STUB_SSH_SWAP_LINK=<path>: before running a command
# containing <text>, replace the symlink <path> with an empty real directory
# (HTTP_ROOT/installers falling back onto the local disk between check and write).
while [ $# -gt 0 ]; do case "$1" in -o) shift 2 ;; -*) shift ;; *) break ;; esac; done
[ "${1:-}" = "p5-test-benzene" ] || { echo "stub ssh: refusing host ${1:-?}" >&2; exit 97; }
shift
if [ -n "${STUB_SSH_FAIL_ON:-}" ] && [[ "$*" == *"${STUB_SSH_FAIL_ON}"* ]]; then
  echo "stub ssh: injected failure" >&2; exit 1
fi
if [ -n "${STUB_SSH_SWAP_ON:-}" ] && [[ "$*" == *"${STUB_SSH_SWAP_ON}"* ]] && [ -L "${STUB_SSH_SWAP_LINK}" ]; then
  rm -f "${STUB_SSH_SWAP_LINK}" && mkdir "${STUB_SSH_SWAP_LINK}"
fi
exec bash -c "$*"
"""

# stub findmnt: reports $STUB_STORE as a cifs mount, so the production store
# check (prov-store-guard.sh) runs; HTTP_ROOT/installers links into the store.
STUB_FINDMNT = """#!/usr/bin/env bash
t= m=
while [ $# -gt 0 ]; do case "$1" in -t) t=$2; shift 2 ;; -M) m=$2; shift 2 ;; *) shift ;; esac; done
[ "$t" = cifs ] && [ "$m" = "$STUB_STORE" ] && { echo //nas.test/SolariNet; exit 0; }
exit 1
"""


class _SignStub:
    """Minimal solariCtl SIGN responder on a unix socket."""

    def __init__(self, path: str):
        self.path, self.calls = path, 0
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
                self.calls += 1
                pem = "-----BEGIN CERTIFICATE-----\nTEST\n-----END CERTIFICATE-----\n"
                conn.sendall(f"OK cert={urllib.parse.quote(pem, safe='')}\n".encode())

    def close(self):
        self.stop = True
        self.thread.join(timeout=2)
        self.sock.close()


@unittest.skipUnless(shutil.which("openssl"), "openssl needed by fleet-provision.sh")
class InstallProfiles(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.mkdtemp(prefix="p5-prof-")
        self.http = os.path.join(self.tmp, "http")
        self.prov = os.path.join(self.tmp, "prov")
        bindir = os.path.join(self.tmp, "bin")
        home = os.path.join(self.tmp, "home")
        os.makedirs(bindir)
        os.makedirs(os.path.join(home, ".ssh"))
        with open(os.path.join(home, ".ssh", "id_ed25519.pub"), "w") as fh:
            fh.write(OPERATOR_KEY + "\n")
        with open(os.path.join(bindir, "ssh"), "w") as fh:
            fh.write(STUB_SSH)
        os.chmod(os.path.join(bindir, "ssh"), 0o755)
        with open(os.path.join(bindir, "findmnt"), "w") as fh:
            fh.write(STUB_FINDMNT)
        os.chmod(os.path.join(bindir, "findmnt"), 0o755)
        store = os.path.join(self.tmp, "store")
        for sub in ("installers", "images"):
            os.makedirs(os.path.join(store, sub))
            os.makedirs(self.http, exist_ok=True)
            os.symlink(os.path.join(store, sub), os.path.join(self.http, sub))
        self.sign = _SignStub(os.path.join(self.tmp, "ctl.sock"))
        self.client_bin = os.path.join(self.tmp, "solariClient")
        with open(self.client_bin, "wb") as fh:
            fh.write(b"\x7fELF fake agent\n")
        os.chmod(self.client_bin, 0o755)
        self.env = dict(os.environ, PATH=f"{bindir}:{os.environ['PATH']}", HOME=home,
                        BENZENE="p5-test-benzene", HTTP_ROOT=self.http, PROV_ROOT=self.prov,
                        HTTP_BASE=BASE, CTL_SOCK=self.sign.path,
                        FLEET_CONTROL_PUBKEY=CONTROL_KEY,
                        SOLARI_ALLOW_UNVERIFIED_PROVISIONING="1",
                        PROV_STORE=store, PROV_STORE_FSTYPE="cifs", STUB_STORE=store,
                        SOLARI_CLIENT_BIN=self.client_bin)
        for k in ("SOLARI_PROV_LOCAL", "ADMIN_USER", "DOMAIN"):
            self.env.pop(k, None)
        for distro, (release, src) in IMAGES.items():
            d = os.path.join(self.http, "installers", distro, release, "x86_64")
            os.makedirs(d)
            with open(os.path.join(d, "solari-image.json"), "w") as fh:
                json.dump({"distro": distro, "release_name": release, "release_label": None,
                           "arch": "x86_64", "version": VERSIONS.get(distro, "t1"), "source_url": src,
                           "manifest_sha256": "a" * 64,
                           "fetched_at": "2026-10-31 10:00:00"}, fh)

    def tearDown(self):
        self.sign.close()
        shutil.rmtree(self.tmp, ignore_errors=True)

    def provision(self, distro, *extra, target=MAC, **env):
        e = dict(self.env)
        e.update(env)
        argv = ["bash", FLEET_PROVISION, "--target", target, "--distro", distro,
                "--arch", "x86_64", "--release", IMAGES[distro][0], "--hostname", "node-31",
                "--domain", "lab.example", "--op", "p5-test", *extra]
        return subprocess.run(argv, env=e, capture_output=True, text=True, timeout=120)

    def read(self, *parts):
        with open(os.path.join(self.http, *parts), encoding="utf-8") as fh:
            return fh.read()

    def profile_env(self):
        with open(os.path.join(self.prov, "profiles", MACH, "profile.env")) as fh:
            return dict(l.split("=", 1) for l in fh.read().splitlines() if "=" in l)

    # -- openSUSE ------------------------------------------------------------
    def autoyast(self, *extra, **env):
        p = self.provision("opensuse", *extra, **env)
        self.assertEqual(p.returncode, 0, p.stdout + p.stderr)
        text = self.read("configs", f"{MACH}.xml")
        self.assertNotIn("@@", text)
        return text, ET.fromstring(text)

    def test_tumbleweed_autoyast_one_package_per_name(self):
        text, root = self.autoyast("--run-id", "4242")
        pkgs = [e.text for e in root.findall("./a:software/a:packages/a:package", NS)]
        self.assertEqual(pkgs, SUSE_BOOTSTRAP)
        for name in pkgs:
            self.assertRegex(name, r"^[a-z0-9][a-z0-9.+_-]*$")       # one name, no spaces
        self.assertNotIn("openssh-server", pkgs)                       # Debian name
        keys = [e.text for e in root.iter("{%s}listentry" % NS["a"])
                if (e.text or "").startswith("ssh-")]
        self.assertEqual(keys, [OPERATOR_KEY, CONTROL_KEY])
        user = [u for u in root.iter("{%s}user" % NS["a"])
                if u.findtext("a:username", namespaces=NS) == "solari-admin"]
        self.assertEqual(len(user), 1)
        scripts = "\n".join(s.text or "" for s in root.iter("{%s}source" % NS["a"]))
        self.assertIn(MARKER, scripts)
        self.assertIn("if [ -n '4242' ]", scripts)
        self.assertIn('printf \'{"run_id": 4242}\\n\'', scripts)
        self.assertIn("zypper --non-interactive install --no-recommends python3", scripts)
        self.assertEqual(self.profile_env()["run_id"], "4242")
        self.assertIn("node-31.lab.example", text)

    def test_tumbleweed_autoyast_schema(self):
        # A profile that fails AutoYaST validation stops the install at a dialog.
        text, root = self.autoyast("--run-id", "4242")
        self.assertEqual(root.findtext("./a:general/a:mode/a:confirm", namespaces=NS), "false")
        # No second stage: it holds logins on first boot and, after an ssh=1
        # installer boot, waits for a yast.ssh login nobody can make.
        self.assertEqual(root.findtext("./a:general/a:mode/a:second_stage", namespaces=NS), "false")
        # Warnings time out; errors still stop the install.
        self.assertEqual(root.findtext("./a:report/a:warnings/a:timeout", namespaces=NS), "10")
        self.assertEqual(root.findtext("./a:report/a:errors/a:timeout", namespaces=NS), "0")
        self.assertEqual(root.findtext("./a:bootloader/a:loader_type", namespaces=NS), "default")
        enabled = root.findall("./a:services-manager/a:services/a:enable/a:service", NS)
        self.assertEqual([e.text for e in enabled], ["sshd"])
        self.assertIsNone(root.find(".//a:service_status", NS))
        products = root.findall("./a:software/a:products/a:product", NS)
        self.assertEqual([e.text for e in products], ["openSUSE"])
        # Full RELAX NG check where the YaST schema is installed (openSUSE:
        # yast2-schema-default), e.g. SOLARI_AUTOYAST_RNG=/usr/share/YaST2/
        # schema/autoyast/rng/profile.rng.
        rng = os.environ.get("SOLARI_AUTOYAST_RNG")
        if not rng or not shutil.which("xmllint"):
            self.skipTest("SOLARI_AUTOYAST_RNG unset or xmllint missing")
        with tempfile.NamedTemporaryFile("w", suffix=".xml") as fh:
            fh.write(text)
            fh.flush()
            r = subprocess.run(["xmllint", "--noout", "--relaxng", rng, fh.name],
                               capture_output=True, text=True)
        self.assertEqual(r.returncode, 0, r.stderr)

    def test_unknown_opensuse_release_has_no_base_product(self):
        # The base product is required; an unmapped release must not render.
        p = self.provision("opensuse", "--release", "slowroll")
        self.assertNotEqual(p.returncode, 0)
        self.assertIn("no AutoYaST base product", p.stdout + p.stderr)
        self.assertFalse(os.path.exists(os.path.join(self.http, "configs", f"{MACH}.xml")))

    def test_agent_binary_published_with_its_libraries(self):
        # firstboot installs installers/solariClient.<arch>; staging publishes it
        # and the chroot script installs its shared libraries (non-fatally).
        _text, root = self.autoyast("--run-id", "4242")
        with open(os.path.join(self.http, "installers", "solariClient.x86_64"), "rb") as fh:
            self.assertEqual(fh.read(), b"\x7fELF fake agent\n")
        scripts = "\n".join(s.text or "" for s in root.iter("{%s}source" % NS["a"]))
        self.assertIn("libnng1 libmbedtls21 libmbedcrypto16 libmbedx509-7", scripts)
        self.assertIn("|| echo 'solari: agent libraries not all installed", scripts)

    def test_missing_agent_binary_stages_nothing(self):
        # No binary to publish and none in the store: refuse, so a node never
        # installs agentless.
        p = self.provision("opensuse", "--run-id", "4242", SOLARI_CLIENT_BIN="",
                           SOLARI_DIST_DIR=os.path.join(self.tmp, "nodist"))
        self.assertNotEqual(p.returncode, 0)
        self.assertIn("no solariClient.x86_64 to install at first boot", p.stdout + p.stderr)
        self.assertFalse(os.path.exists(os.path.join(self.http, "configs", f"{MACH}.xml")))

    def test_tumbleweed_without_run_id_has_no_marker_or_control_key(self):
        _text, root = self.autoyast(FLEET_CONTROL_PUBKEY="")
        keys = [e.text for e in root.iter("{%s}listentry" % NS["a"])
                if (e.text or "").startswith("ssh-")]
        self.assertEqual(keys, [OPERATOR_KEY])
        scripts = "\n".join(s.text or "" for s in root.iter("{%s}source" % NS["a"]))
        self.assertIn("if [ -n '' ]", scripts)                         # marker block is inert
        pkgs = [e.text for e in root.findall("./a:software/a:packages/a:package", NS)]
        self.assertEqual(pkgs, SUSE_BOOTSTRAP)
        self.assertEqual(self.profile_env()["run_id"], "")

    # -- Debian --------------------------------------------------------------
    def test_debian_preseed_bootstrap_and_marker(self):
        p = self.provision("debian", "--run-id", "17")
        self.assertEqual(p.returncode, 0, p.stdout + p.stderr)
        text = self.read("configs", f"{MACH}.preseed")
        self.assertNotIn("@@", text)
        inc = re.search(r"^d-i pkgsel/include\s+string (.*)$", text, re.M)
        self.assertIsNotNone(inc)
        self.assertEqual(inc.group(1).split(), DEB_BOOTSTRAP)
        self.assertIn(f"'{OPERATOR_KEY}' '{CONTROL_KEY}'", text)
        self.assertIn("if [ -n '17' ]", text)
        self.assertIn(MARKER, text)

    def test_debian_legacy_keeps_core_packages(self):
        p = self.provision("debian")
        self.assertEqual(p.returncode, 0, p.stdout + p.stderr)
        inc = re.search(r"^d-i pkgsel/include\s+string (.*)$",
                        self.read("configs", f"{MACH}.preseed"), re.M).group(1).split()
        self.assertEqual(inc[:len(DEB_BOOTSTRAP)], DEB_BOOTSTRAP)
        for name in ("htop", "tmux", "chrony", "jq"):                  # FLEET_CORE_PACKAGES
            self.assertIn(name, inc)
        self.assertEqual(len(inc), len(set(inc)), "duplicates must be dropped")

    # -- Ubuntu --------------------------------------------------------------
    def test_ubuntu_autoinstall_lists(self):
        p = self.provision("ubuntu", "--run-id", "9")
        self.assertEqual(p.returncode, 0, p.stdout + p.stderr)
        text = self.read("configs", "ubuntu", MACH, "user-data")
        self.assertNotIn("@@", text)
        self.assertIn(f'      - "{OPERATOR_KEY}"\n      - "{CONTROL_KEY}"\n', text)
        for name in DEB_BOOTSTRAP:
            self.assertIn(f"\n    - {name}\n", text)
        try:
            import yaml  # noqa: PLC0415 — optional
        except ImportError:
            self.skipTest("PyYAML not installed; textual checks only")
        doc = yaml.safe_load(text)["autoinstall"]
        self.assertEqual(doc["packages"], DEB_BOOTSTRAP)
        self.assertEqual(doc["ssh"]["authorized-keys"], [OPERATOR_KEY, CONTROL_KEY])
        marker = [c for c in doc["late-commands"] if MARKER in c]
        self.assertEqual(len(marker), 1)
        self.assertIn("printf '{\"run_id\": 9}\\n' > /target" + MARKER, marker[0])

    # -- refusals ------------------------------------------------------------
    def test_run_id_refusals(self):
        for extra, target, env, needle in (
                (["--run-id", "0"], MAC, {}, "invalid --run-id"),
                (["--run-id", "12;id"], MAC, {}, "invalid --run-id"),
                (["--run-id", "5"], "node-31", {}, "--run-id needs a MAC"),
                (["--run-id", "5"], MAC, {"FLEET_CONTROL_PUBKEY": "not a key"},
                 "control key is not a plain OpenSSH public key"),
        ):
            with self.subTest(extra=extra, target=target):
                p = self.provision("opensuse", *extra, target=target, **env)
                self.assertEqual(p.returncode, 1, p.stdout + p.stderr)
                self.assertIn(needle, p.stderr)
        self.assertFalse(os.path.exists(os.path.join(self.http, "configs", f"{MACH}.xml")))

    def test_no_stack_vars_in_web_root(self):
        pub = os.path.join(self.http, "configs", f"{MACH}.stack.json")
        os.makedirs(os.path.dirname(pub))
        with open(pub, "w") as fh:
            fh.write("{}")                                              # P4 leftover
        vars_path = os.path.join(self.tmp, "vars.json")
        with open(vars_path, "w") as fh:
            json.dump({"solari_stack_order": ["core"]}, fh)
        p = self.provision("opensuse", "--run-id", "3", "--stack-vars", vars_path)
        self.assertEqual(p.returncode, 0, p.stdout + p.stderr)
        self.assertFalse(os.path.exists(pub))
        priv = os.path.join(self.prov, "profiles", MACH, "stack.json")
        self.assertEqual(stat.S_IMODE(os.stat(priv).st_mode), 0o600)

    # -- arming order (P1) ---------------------------------------------------
    def armed(self):
        return os.path.exists(os.path.join(self.http, "ipxe", f"mac-{MACH}.ipxe"))

    def leftovers(self):
        return [os.path.join(d, f) for top in (self.http, self.prov)
                for d, _, files in os.walk(top) for f in files if ".tmp." in f]

    def test_profile_published_last(self):
        # a failure anywhere after the configs push must leave nothing armed
        vars_path = os.path.join(self.tmp, "vars.json")
        with open(vars_path, "w") as fh:
            json.dump({"solari_stack_order": ["core"]}, fh)
        for fail_on in ("solari-firstboot.sh", "/profile.env", "/stack.json",
                        f"mac-{MACH}.ipxe.tmp"):
            with self.subTest(fail_on=fail_on):
                p = self.provision("opensuse", "--run-id", "7", "--stack-vars", vars_path,
                                   STUB_SSH_FAIL_ON=fail_on)
                self.assertNotEqual(p.returncode, 0, p.stdout + p.stderr)
                self.assertFalse(self.armed(), "per-MAC boot profile armed by a failed staging")
                self.assertEqual(self.leftovers(), [])
        p = self.provision("opensuse", "--run-id", "7", "--stack-vars", vars_path)
        self.assertEqual(p.returncode, 0, p.stdout + p.stderr)
        self.assertTrue(self.armed())
        self.assertEqual(self.profile_env()["run_id"], "7")
        self.assertEqual(self.leftovers(), [])

    def test_failed_restage_drops_previous_profile(self):
        # run 5 armed; a re-stage for run 6 fails mid-way: run 5's install must
        # not stay armed under run 6's (partial) configs
        p = self.provision("opensuse", "--run-id", "5")
        self.assertEqual(p.returncode, 0, p.stdout + p.stderr)
        self.assertTrue(self.armed())
        p = self.provision("opensuse", "--run-id", "6", STUB_SSH_FAIL_ON="/profile.env")
        self.assertNotEqual(p.returncode, 0, p.stdout + p.stderr)
        self.assertFalse(self.armed())


    # -- store writes re-check the store at write time ---------------------------
    def test_firstboot_write_rechecks_store(self):
        # installers/ stops resolving into the store after the up-front check:
        # the firstboot write must refuse, not land on benzene's local disk
        link = os.path.join(self.http, "installers")
        p = self.provision("debian", STUB_SSH_SWAP_ON="solari-firstboot.sh", STUB_SSH_SWAP_LINK=link)
        self.assertNotEqual(p.returncode, 0, p.stdout + p.stderr)
        self.assertIn("cannot write installers/solari-firstboot.sh into the image store", p.stderr)
        self.assertFalse(os.path.islink(link))
        self.assertEqual(os.listdir(link), [], "nothing may be written onto the local disk")
        self.assertFalse(os.path.exists(os.path.join(self.tmp, "store", "installers",
                                                     "solari-firstboot.sh")))
        self.assertFalse(self.armed())


if __name__ == "__main__":
    unittest.main()
