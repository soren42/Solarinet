#!/usr/bin/env python3
"""test_prov_helpers — deploy/fleet/prov-stage.py, stack-apply.py, prov-finish.py end to end.

Runs both helpers as subprocesses against a SCRATCH MariaDB this test creates
and always drops (`sor_test_p3_helpers`: schema.sql + every migration in
order), with no network and no ssh:

  * SOLARI_PROV_LOCAL=1, PROV_ROOT / HTTP_ROOT -> temp dirs (provlib runs the
    "remote" side locally);
  * fake `ansible` / `ansible-playbook` first on PATH record argv + cwd and
    emit canned output; SOLARI_FLEET_PROVISION -> a fake recording script;
  * the helpers connect as an ephemeral SELECT/INSERT/UPDATE user on the
    scratch DB only (password via env, SQL via stdin — never argv).

Local:  passwordless `sudo -n mariadb`; STACKGEN_TEST_PYTHON=<python with
        pymysql> (e.g. /tmp/stackgen-venv/bin/python), default python3.
CI:     SOLARI_TEST_SOR_DB=1 MYSQL_HOST=127.0.0.1 [MYSQL_PORT] [MYSQL_USER]
        MYSQL_PWD=<root password of the disposable service container>;
        STACKGEN_TEST_USER_HOST=% when the client is not on loopback.
No DB reachable -> prints SKIP and exits 0, unless PROV_HELPERS_REQUIRE_DB=1.
"""
from __future__ import annotations

import importlib.util
import json
import os
import re
import secrets
import shutil
import stat
import subprocess
import sys
import tempfile
import unittest

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
FLEET = os.path.join(ROOT, "deploy", "fleet")
SOR = os.path.join(ROOT, "netdb", "sor")
PROV_STAGE = os.path.join(FLEET, "prov-stage.py")
STACK_APPLY = os.path.join(FLEET, "stack-apply.py")
PROV_FINISH = os.path.join(FLEET, "prov-finish.py")
DB = "sor_test_p3_helpers"
LIVE_DBS = {"sor", "solarinet", "solarinet_stage", "sor_probe"}
PY = os.environ.get("STACKGEN_TEST_PYTHON", "python3")
CI_MODE = os.environ.get("SOLARI_TEST_SOR_DB") == "1"
DB_PORT = os.environ.get("MYSQL_PORT", "3306") if CI_MODE else "3306"
USER_HOST = os.environ.get("STACKGEN_TEST_USER_HOST", "127.0.0.1")
HELPER_USER = f"p3h_{os.getpid()}"
HELPER_PASS = secrets.token_hex(16)

assert DB not in LIVE_DBS and DB.startswith("sor_test_p3_")

if CI_MODE:
    ADMIN = ["mariadb", "--protocol=TCP", "--host=127.0.0.1", f"--port={DB_PORT}",
             f"--user={os.environ.get('MYSQL_USER', 'root')}"]
else:
    ADMIN = ["sudo", "-n", "mariadb"]

STATE: dict = {"created": False, "user": False, "tmp": None}


# ---------------------------------------------------------------------------
# scratch DB plumbing (admin side: CLI, SQL on stdin)
# ---------------------------------------------------------------------------
def admin(sql: str, db: str | None = DB, check: bool = True) -> str:
    argv = ADMIN + ["-N", "-B"] + ([db] if db else [])
    proc = subprocess.run(argv, input=sql, capture_output=True, text=True, timeout=300)
    if check and proc.returncode != 0:
        raise RuntimeError(f"admin SQL failed: {proc.stderr.strip()[:400]}")
    return proc.stdout.strip()


def admin_file(path: str) -> None:
    with open(path, encoding="utf-8") as fh:
        proc = subprocess.run(ADMIN + [DB], stdin=fh, capture_output=True, text=True, timeout=300)
    if proc.returncode != 0:
        raise RuntimeError(f"applying {os.path.basename(path)} failed: {proc.stderr.strip()[:400]}")


def db_reachable() -> bool:
    if CI_MODE and os.environ.get("MYSQL_HOST") != "127.0.0.1":
        print("refusing: CI mode needs MYSQL_HOST=127.0.0.1", file=sys.stderr)
        sys.exit(2)
    try:
        proc = subprocess.run(ADMIN + ["-N", "-B", "-e", "SELECT 1"], capture_output=True,
                              text=True, timeout=30, stdin=subprocess.DEVNULL)
    except (OSError, subprocess.TimeoutExpired):
        return False
    return proc.returncode == 0 and proc.stdout.strip() == "1"


def build_db() -> None:
    if admin(f"SELECT 1 FROM information_schema.SCHEMATA WHERE SCHEMA_NAME='{DB}'", db=None):
        print(f"refusing to run: database '{DB}' already exists — drop it yourself if it is scratch",
              file=sys.stderr)
        sys.exit(2)
    admin(f"CREATE DATABASE `{DB}` CHARACTER SET utf8mb4 COLLATE utf8mb4_unicode_ci", db=None)
    STATE["created"] = True
    admin_file(os.path.join(SOR, "schema.sql"))
    migs = sorted(f for f in os.listdir(os.path.join(SOR, "migrations")) if f.endswith(".sql"))
    for m in migs:
        admin_file(os.path.join(SOR, "migrations", m))
    STATE["migrations"] = len(migs)
    admin(f"CREATE USER '{HELPER_USER}'@'{USER_HOST}' IDENTIFIED BY '{HELPER_PASS}'", db=None)
    STATE["user"] = True
    admin(f"GRANT SELECT, INSERT, UPDATE ON `{DB}`.* TO '{HELPER_USER}'@'{USER_HOST}'", db=None)
    seed()


def drop_db() -> None:
    if STATE["user"]:
        admin(f"DROP USER IF EXISTS '{HELPER_USER}'@'{USER_HOST}'", db=None, check=False)
    if STATE["created"]:
        admin(f"DROP DATABASE IF EXISTS `{DB}`", db=None, check=False)


SRC = "(SELECT id FROM sources WHERE slug='provisioner')"
SHA = "a" * 64
TW = "distro='opensuse' AND release_name='tumbleweed' AND arch='x86_64'"


def seed() -> None:
    admin(f"""
UPDATE prov_images SET sha256='{SHA}', fetched_at=NOW(), version='t1' WHERE {TW};
UPDATE prov_images SET sha256='{SHA}', fetched_at=NOW(), version='t1' WHERE boot_method='pi_netboot';
UPDATE prov_images SET sha256='{SHA}' WHERE distro='debian' AND release_name='12';
INSERT INTO entities (name, entity_type, source_id) VALUES
  ('p3h-node1','server',{SRC}), ('p3h-nomac','server',{SRC}), ('Bad_Name','server',{SRC}),
  ('p3h-gone','server',{SRC}), ('p3h-node2','server',{SRC});
UPDATE entities SET deleted_at=NOW() WHERE name='p3h-gone';
INSERT INTO interfaces (entity_id, name, if_kind, mac, is_management, source_id) VALUES
  ((SELECT id FROM entities WHERE name='p3h-node1'),'wlan0','wifi','02:00:00:00:00:01',0,{SRC}),
  ((SELECT id FROM entities WHERE name='p3h-node1'),'eth0','physical','02:00:00:00:00:02',0,{SRC}),
  ((SELECT id FROM entities WHERE name='p3h-node1'),'mgmt0','virtual','02:00:00:00:00:03',1,{SRC}),
  ((SELECT id FROM entities WHERE name='p3h-node1'),'old0','physical','02:00:00:00:00:04',1,{SRC}),
  ((SELECT id FROM entities WHERE name='p3h-node2'),'wlan0','wifi','02:00:00:00:00:11',0,{SRC}),
  ((SELECT id FROM entities WHERE name='p3h-node2'),'eth1','physical','02:00:00:00:00:12',0,{SRC}),
  ((SELECT id FROM entities WHERE name='p3h-nomac'),'eth0','physical',NULL,1,{SRC}),
  ((SELECT id FROM entities WHERE name='p3h-nomac'),'eth1','physical','02:00:00:00:00:21',0,{SRC});
UPDATE interfaces SET deleted_at=NOW() WHERE name='old0'
  OR (name='eth1' AND entity_id=(SELECT id FROM entities WHERE name='p3h-nomac'));
INSERT INTO hardware_models (make, model, hw_type, arch, source_id) VALUES
  ('P3H','Box x86','server','x86_64',{SRC}), ('P3H','Box arm','sbc','arm64',{SRC});
INSERT INTO hardware_units (model_id, serial, source_id) VALUES
  ((SELECT id FROM hardware_models WHERE model='Box x86'),'p3h-sn-2',{SRC}),
  ((SELECT id FROM hardware_models WHERE model='Box arm'),'p3h-sn-3',{SRC});
INSERT INTO entities (name, entity_type, source_id, hardware_unit_id) VALUES
  ('p3h-node3','server',{SRC},(SELECT id FROM hardware_units WHERE serial='p3h-sn-3'));
UPDATE entities SET hardware_unit_id=(SELECT id FROM hardware_units WHERE serial='p3h-sn-2')
  WHERE name='p3h-node2';
INSERT INTO interfaces (entity_id, name, if_kind, mac, is_management, source_id) VALUES
  ((SELECT id FROM entities WHERE name='p3h-node3'),'eth0','physical','02:00:00:00:00:31',1,{SRC});
""")


def ent(name: str) -> str:
    return admin(f"SELECT id FROM entities WHERE name='{name}'")


def img(where: str) -> str:
    return admin(f"SELECT id FROM prov_images WHERE {where} AND deleted_at IS NULL")


def runs(entity: str, mode: str | None = None) -> list[dict]:
    cond = f" AND mode='{mode}'" if mode else ""
    out = admin("SELECT id, mode, status, dry_run, IFNULL(detail,''), requested_by, "
                "IFNULL(image_id,''), CAST(stack_slugs AS CHAR), started_at IS NOT NULL, "
                f"finished_at IS NOT NULL FROM prov_runs WHERE entity_id={entity}{cond} ORDER BY id")
    rows = []
    for line in filter(None, out.splitlines()):
        f = line.split("\t")
        rows.append({"id": int(f[0]), "mode": f[1], "status": f[2], "dry_run": int(f[3]),
                     "detail": f[4], "requested_by": f[5], "image_id": f[6],
                     "stacks": json.loads(f[7]), "started": f[8] == "1", "finished": f[9] == "1"})
    return rows


# ---------------------------------------------------------------------------
# fakes
# ---------------------------------------------------------------------------
FAKE_ANSIBLE = r'''#!__PY__
import json, os, sys
rec = {"tool": "ansible", "argv": sys.argv[1:], "cwd": os.getcwd(),
       "load_cb": os.environ.get("ANSIBLE_LOAD_CALLBACK_PLUGINS"),
       "result_fmt": os.environ.get("ANSIBLE_CALLBACK_RESULT_FORMAT"),
       "ssh_args": os.environ.get("ANSIBLE_SSH_COMMON_ARGS"),
       "hkc": os.environ.get("ANSIBLE_HOST_KEY_CHECKING"),
       "become": os.environ.get("ANSIBLE_BECOME")}
with open(os.environ["FAKE_LOG"], "a") as fh:
    fh.write(json.dumps(rec) + "\n")
host = sys.argv[sys.argv.index("-i") + 1].rstrip(",")
if sys.argv[sys.argv.index("-m") + 1] == "ansible.builtin.raw":
    cmd = sys.argv[sys.argv.index("-a") + 1]
    if cmd.startswith("cat /etc/ansible/facts.d/"):
        marker = os.environ.get("FAKE_MARKER")
        if marker is None:
            print(host + " | UNREACHABLE! => {\"changed\": false, \"unreachable\": true}")
            sys.exit(4)
        print(host + " | CHANGED | rc=0 >>\n" + marker)
        sys.exit(0)
    rc = int(os.environ.get("FAKE_RAW_RC", "0"))
    print(host + (" | CHANGED | rc=0 >>" if rc == 0 else " | FAILED | rc=%d >>" % rc))
    sys.exit(rc)
facts = {"ansible_os_family": os.environ.get("FAKE_FAMILY", "Suse"),
         "ansible_architecture": os.environ.get("FAKE_ARCH", "x86_64")}
mode = os.environ.get("FAKE_FACTS_MODE", "oneline")
if mode == "fail":
    print(host + " | UNREACHABLE! => {\"changed\": false, \"unreachable\": true}")
    sys.exit(4)
if mode == "oneline":
    print("[WARNING]: Platform linux on host " + host + " is using the discovered Python")
    print(host + " | SUCCESS => " + json.dumps({"ansible_facts": facts, "changed": False}, indent=4))
else:
    print(json.dumps({"custom_stats": {}, "plays": [{"play": {"name": "Ansible Ad-Hoc"},
          "tasks": [{"task": {"name": "ansible.builtin.setup"},
                     "hosts": {host: {"ansible_facts": facts, "changed": False}}}]}],
          "stats": {host: {"ok": 1}}}))
'''

FAKE_PLAYBOOK = r'''#!__PY__
import json, os, stat, sys
argv = sys.argv[1:]
rec = {"tool": "ansible-playbook", "argv": argv, "cwd": os.getcwd(),
       "stdout_cb": os.environ.get("ANSIBLE_STDOUT_CALLBACK"),
       "ssh_args": os.environ.get("ANSIBLE_SSH_COMMON_ARGS"),
       "hkc": os.environ.get("ANSIBLE_HOST_KEY_CHECKING")}
ev = argv[argv.index("-e") + 1] if "-e" in argv else ""
if ev.startswith("@"):
    try:
        st = os.stat(ev[1:])
        rec["vars_mode"] = oct(stat.S_IMODE(st.st_mode))
        with open(ev[1:]) as fh:
            rec["vars_order"] = json.load(fh).get("solari_stack_order")
    except (OSError, ValueError) as exc:
        rec["vars_error"] = str(exc)
with open(os.environ["FAKE_LOG"], "a") as fh:
    fh.write(json.dumps(rec) + "\n")
check = "--check" in argv
print("PLAY [Provision SolariNet fleet host] " + ("(check)" if check else "(apply)"))
rc = int(os.environ.get("FAKE_CHECK_RC" if check else "FAKE_APPLY_RC", "0"))
print("PLAY RECAP ok=3 failed=%d" % (1 if rc else 0))
sys.exit(rc)
'''

FAKE_PROVISION = r'''#!__PY__
import json, os, sys
argv = sys.argv[1:]
rec = {"tool": "fleet-provision", "argv": argv}
sv = argv[argv.index("--stack-vars") + 1] if "--stack-vars" in argv else None
if sv:
    try:
        with open(sv) as fh:
            rec["vars_order"] = json.load(fh).get("solari_stack_order")
    except (OSError, ValueError) as exc:
        rec["vars_error"] = str(exc)
with open(os.environ["FAKE_LOG"], "a") as fh:
    fh.write(json.dumps(rec) + "\n")
print("[fleet] fake staged " + " ".join(argv))
sys.exit(int(os.environ.get("FAKE_PROVISION_RC", "0")))
'''


def make_fakes(tmp: str) -> str:
    fakebin = os.path.join(tmp, "fakebin")
    os.makedirs(fakebin)
    interp = shutil.which("python3") or sys.executable
    for name, body in (("ansible", FAKE_ANSIBLE), ("ansible-playbook", FAKE_PLAYBOOK),
                       ("fleet-provision", FAKE_PROVISION)):
        p = os.path.join(fakebin, name)
        with open(p, "w", encoding="utf-8") as fh:
            fh.write(body.replace("__PY__", interp))
        os.chmod(p, 0o755)
    return fakebin


# ---------------------------------------------------------------------------
class Base(unittest.TestCase):
    def setUp(self):
        tmp = STATE["tmp"]
        self.prov_root = os.path.join(tmp, "prov")
        self.log_path = os.path.join(tmp, "fake.log")
        shutil.rmtree(self.prov_root, ignore_errors=True)
        os.makedirs(os.path.join(self.prov_root, "ansible", "playbooks"))
        with open(os.path.join(self.prov_root, "ansible", "playbooks", "site.yml"), "w") as fh:
            fh.write("---\n")
        if os.path.exists(self.log_path):
            os.unlink(self.log_path)
        fakebin = os.path.join(tmp, "fakebin")
        self.env = {
            "PATH": fakebin + os.pathsep + os.environ.get("PATH", "/usr/bin:/bin"),
            "HOME": os.environ.get("HOME", tmp), "LANG": "C.UTF-8",
            "SOLARI_PROV_LOCAL": "1", "PROV_ROOT": self.prov_root,
            "HTTP_ROOT": os.path.join(self.prov_root, "netboot", "http"),
            "SOLARI_FLEET_PROVISION": os.path.join(fakebin, "fleet-provision"),
            "SOLARI_ALLOW_UNVERIFIED_PROVISIONING": "1",
            "SOR_DB_NAME": DB, "SOR_DB_HOST": "127.0.0.1", "SOR_DB_PORT": DB_PORT,
            "SOR_DB_USER": HELPER_USER, "SOR_DB_PASS": HELPER_PASS,
            "FAKE_LOG": self.log_path,
        }

    def run_helper(self, script, *args, **env):
        e = dict(self.env)
        for k, v in env.items():
            if v is None:
                e.pop(k, None)
            else:
                e[k] = v
        proc = subprocess.run([PY, script, *args], env=e, capture_output=True, text=True,
                              timeout=600, cwd=ROOT)
        self.assertNotIn(HELPER_PASS, proc.stdout + proc.stderr, "password leaked to output")
        return proc

    def calls(self, tool=None):
        if not os.path.exists(self.log_path):
            return []
        with open(self.log_path) as fh:
            recs = [json.loads(l) for l in fh if l.strip()]
        return [r for r in recs if tool is None or r["tool"] == tool]


class ArgsAndGate(Base):
    def test_usage_errors_exit_2(self):
        n1 = ent("p3h-node1")
        ma = ["--mac", "02:00:00:00:00:02", "--arch", "x86_64"]
        cases = [
            (PROV_STAGE, ["--entity", n1, "--image", "1", "--stacks", "core", *ma]),      # no --op
            (PROV_STAGE, ["--entity", "01", "--image", "1", "--stacks", "core", *ma, "--op", "j"]),
            (PROV_STAGE, ["--entity", "0", "--image", "1", "--stacks", "core", *ma, "--op", "j"]),
            (PROV_STAGE, ["--entity", n1, "--image", "4294967296", "--stacks", "core", *ma,
                          "--op", "j"]),
            (PROV_STAGE, ["--entity", n1, "--image", "1", "--stacks", "core,", *ma, "--op", "j"]),
            (PROV_STAGE, ["--entity", n1, "--image", "1", "--stacks", "Core", *ma, "--op", "j"]),
            (PROV_STAGE, ["--entity", n1, "--image", "1", "--stacks", "core", *ma, "--op", "j k"]),
            (PROV_STAGE, ["--entity", n1, "--image", "1", "--stacks", "core", *ma, "--op", "$(id)"]),
            (PROV_STAGE, ["--entity", n1, "--image", "1", "--stacks", "core", "--arch", "x86_64",
                          "--op", "j"]),                                              # no --mac
            (PROV_STAGE, ["--entity", n1, "--image", "1", "--stacks", "core", "--mac",
                          "02:00:00:00:00:02", "--op", "j"]),                          # no --arch
            (PROV_STAGE, ["--entity", n1, "--image", "1", "--stacks", "core", "--mac",
                          "02-00-00-00-00-02", "--arch", "x86_64", "--op", "j"]),
            (PROV_STAGE, ["--entity", n1, "--image", "1", "--stacks", "core", "--mac",
                          "02:00:00:00:00:02;id", "--arch", "x86_64", "--op", "j"]),
            (PROV_STAGE, ["--entity", n1, "--image", "1", "--stacks", "core", "--mac",
                          "02:00:00:00:00:02", "--arch", "amd64", "--op", "j"]),
            (PROV_STAGE, ["--entity", n1, "--image", "1", "--stacks", "core", "--mac",
                          "02:00:00:00:00:02", "--arch", "", "--op", "j"]),
            (STACK_APPLY, ["--entity", "-1", "--stacks", "core", "--op", "j"]),
            (STACK_APPLY, ["--entity", n1, "--stacks", "core,,web", "--op", "j"]),
            (STACK_APPLY, ["--entity", n1, "--stacks", ",".join(f"s{i}" for i in range(33)), "--op", "j"]),
            (STACK_APPLY, ["--entity", n1, "--stacks", "core", "--op", ""]),
            (STACK_APPLY, ["--entity", n1, "--stacks", "core", "--dry-run", "yes", "--op", "j"]),
        ]
        for script, args in cases:
            with self.subTest(script=os.path.basename(script), args=args):
                p = self.run_helper(script, *args)
                self.assertEqual(p.returncode, 2, p.stderr)
        self.assertEqual(self.calls(), [])
        self.assertEqual(runs(n1), [])

    def test_f07_gate_before_db(self):
        # Points the SoR at a database that does not exist: the gate must refuse
        # before any connect attempt, so the only message is the F-07 denial.
        p = self.run_helper(PROV_STAGE, "--entity", ent("p3h-node1"), "--image", "1",
                            "--stacks", "core", "--mac", "02:00:00:00:00:02", "--arch", "x86_64",
                            "--op", "jason", SOLARI_ALLOW_UNVERIFIED_PROVISIONING=None,
                            SOR_DB_NAME="sor_test_p3_does_not_exist")
        self.assertEqual(p.returncode, 1)
        self.assertIn("[fleet][error] F-07: unsigned provisioning is disabled.", p.stderr)
        self.assertNotIn("connect", p.stderr)
        self.assertEqual(self.calls(), [])
        p = self.run_helper(PROV_STAGE, "--entity", ent("p3h-node1"), "--image", "1",
                            "--stacks", "core", "--mac", "02:00:00:00:00:02", "--arch", "x86_64",
                            "--op", "jason", SOLARI_ALLOW_UNVERIFIED_PROVISIONING="yes")
        self.assertEqual(p.returncode, 1)
        self.assertIn("F-07: unsigned provisioning is disabled.", p.stderr)


MACS = {"p3h-node1": "02:00:00:00:00:02", "p3h-node2": "02:00:00:00:00:12",
        "p3h-node3": "02:00:00:00:00:31", "p3h-nomac": "02:00:00:00:00:21",
        "Bad_Name": "02:00:00:00:00:99", "p3h-gone": "02:00:00:00:00:98"}


class StageMixin:
    def stage(self, entity, image, stacks="core", mac=None, arch="x86_64", **env):
        if mac is None:
            name = admin(f"SELECT name FROM entities WHERE id={entity}") if entity.isdigit() else ""
            mac = MACS.get(name, "02:00:00:00:00:02")
        return self.run_helper(PROV_STAGE, "--entity", entity, "--image", image,
                               "--stacks", stacks, "--mac", mac, "--arch", arch,
                               "--op", "jason+ops@akoria.org", **env)


class ProvStage(StageMixin, Base):
    def assert_refused(self, entity, image, needle, stacks="core", **kw):
        before = runs(entity)
        p = self.stage(entity, image, stacks, **kw)
        self.assertEqual(p.returncode, 1, p.stdout + p.stderr)
        self.assertIn(needle, p.stderr)
        self.assertEqual(runs(entity), before, "a refusal must not open a run")
        self.assertEqual(self.calls(), [])
        self.assertFalse(os.path.exists(os.path.join(self.prov_root, "runs")),
                         "a refusal must not write a run record")

    def test_refusals_open_no_run(self):
        n1 = ent("p3h-node1")
        tw = img(TW)
        self.assert_refused(n1, img("distro='opensuse' AND release_name='15.6'"),
                            "not fetched/verified — run IMAGE_REFRESH")
        self.assert_refused(n1, img("distro='debian' AND release_name='12'"),
                            "not fetched/verified")          # sha256 but no fetched_at
        self.assert_refused(n1, img("boot_method='pi_netboot'"), "supports only ipxe")
        self.assert_refused(n1, img("boot_method='media'"), "supports only ipxe")
        self.assert_refused(n1, "999999", "does not exist")
        self.assert_refused(ent("p3h-nomac"), tw, "is not a live interface of entity")
        self.assert_refused(ent("Bad_Name"), tw, "not a valid hostname label")
        self.assert_refused(ent("p3h-gone"), tw, "does not exist or is deleted")
        self.assert_refused(n1, tw, "unknown or deleted stack(s): nope", stacks="core,nope")

    def test_mac_must_belong_to_entity(self):
        n1 = ent("p3h-node1")
        tw = img(TW)
        # another entity's MAC, and this entity's deleted interface
        self.assert_refused(n1, tw, "MAC 02:00:00:00:00:12 is not a live interface",
                            mac="02:00:00:00:00:12")
        self.assert_refused(n1, tw, "MAC 02:00:00:00:00:04 is not a live interface",
                            mac="02:00:00:00:00:04")
        # upper-case input is normalised, not refused
        p = self.stage(n1, tw, mac="02:00:00:00:00:01".upper())
        self.assertEqual(p.returncode, 0, p.stdout + p.stderr)
        argv = self.calls("fleet-provision")[0]["argv"]
        self.assertEqual(argv[argv.index("--target") + 1], "02:00:00:00:00:01")   # wlan0 as picked

    def test_arch_fail_closed(self):
        n1, n2, n3 = ent("p3h-node1"), ent("p3h-node2"), ent("p3h-node3")
        tw = img(TW)
        # no hardware model arch: the asserted (agent-reported) arch must match the image
        self.assert_refused(n1, tw, f"image {tw} is x86_64 but entity {n1} is arm64",
                            arch="arm64")
        # the hardware model arch wins over the request ...
        self.assert_refused(n2, tw, f"entity {n2} hardware model arch is x86_64 but the "
                            "request says arm64", arch="arm64")
        self.assert_refused(n3, tw, f"entity {n3} hardware model arch is arm64 but the "
                            "request says x86_64")
        # ... and must equal the image arch
        self.assert_refused(n3, tw, f"image {tw} is x86_64 but entity {n3} is arm64",
                            arch="arm64")
        p = self.stage(n2, tw)
        self.assertEqual(p.returncode, 0, p.stdout + p.stderr)
        self.assertIn("arch x86_64 (hardware model)", p.stdout)
        p = self.stage(n1, tw)
        self.assertEqual(p.returncode, 0, p.stdout + p.stderr)
        self.assertIn("arch x86_64 (asserted)", p.stdout)

    def test_stage_supersede_and_failure(self):
        n2 = ent("p3h-node2")
        tw = img(TW)
        base = len(runs(n2))                 # earlier tests may have staged node2
        p = self.stage(n2, tw, "dev-tools,core,dev-tools")
        self.assertEqual(p.returncode, 0, p.stdout + p.stderr)
        self.assertIn("F-07 override active", p.stderr)
        r = runs(n2)[base:]
        self.assertEqual(len(r), 1)
        first = r[0]
        self.assertEqual((first["mode"], first["status"], first["detail"]),
                         ("reimage", "running", "staged; awaiting netboot install"))
        self.assertEqual(first["requested_by"], "jason+ops@akoria.org")
        self.assertEqual(first["image_id"], tw)
        self.assertEqual(first["stacks"], ["core", "dev-tools"])     # dependency order, deduped
        self.assertTrue(first["started"] and not first["finished"])
        prov = self.calls("fleet-provision")
        self.assertEqual(len(prov), 1)
        rid = first["id"]
        self.assertEqual(prov[0]["argv"], [
            "--target", "02:00:00:00:00:12", "--distro", "opensuse", "--arch", "x86_64",
            "--release", "tumbleweed", "--hostname", "p3h-node2", "--domain", "akoria.net",
            "--run-id", str(rid), "--op", "jason+ops@akoria.org"])
        self.assertIn("fake staged", p.stdout)                       # child output streamed
        # the run record + vars stay operator-side (0600), never in the web root
        runs_dir = os.path.join(self.prov_root, "runs")
        for suffix in (".json", ".stack.json"):
            st = os.stat(os.path.join(runs_dir, f"reimage-{rid}{suffix}"))
            self.assertEqual(stat.S_IMODE(st.st_mode), 0o600, suffix)
        with open(os.path.join(runs_dir, f"reimage-{rid}.json")) as fh:
            rec = json.load(fh)
        self.assertEqual({k: rec[k] for k in ("run_id", "entity_id", "fqdn", "mac", "cfgkey",
                                              "family", "arch", "admin_user", "stack_order")},
                         {"run_id": rid, "entity_id": int(n2), "fqdn": "p3h-node2.akoria.net",
                          "mac": "02:00:00:00:00:12", "cfgkey": "02-00-00-00-00-12",
                          "family": "suse", "arch": "x86_64", "admin_user": "solari-admin",
                          "stack_order": ["core", "dev-tools"]})
        with open(os.path.join(runs_dir, f"reimage-{rid}.stack.json")) as fh:
            self.assertEqual(json.load(fh)["solari_stack_order"], ["core", "dev-tools"])
        self.assertFalse(os.path.exists(os.path.join(self.prov_root, "netboot", "http", "configs",
                                                     "02-00-00-00-00-12.stack.json")))

        # second stage supersedes the first
        p = self.stage(n2, tw)
        self.assertEqual(p.returncode, 0, p.stdout + p.stderr)
        r = runs(n2)[base:]
        self.assertEqual(len(r), 2)
        self.assertEqual((r[0]["status"], r[0]["detail"]),
                         ("cancelled", f"superseded by run {r[1]['id']}"))
        self.assertTrue(r[0]["finished"])
        self.assertEqual((r[1]["status"], r[1]["stacks"]), ("running", ["core"]))

        # a failing fleet-provision marks the (new) run failed
        p = self.stage(n2, tw, FAKE_PROVISION_RC="3")
        self.assertEqual(p.returncode, 1)
        r = runs(n2)[base:]
        self.assertEqual([x["status"] for x in r], ["cancelled", "cancelled", "failed"])
        self.assertEqual(r[2]["detail"], "fleet-provision failed (exit 3)")
        self.assertEqual(r[1]["detail"], f"superseded by run {r[2]['id']}")

    def test_bad_domain_or_admin_user_refused(self):
        n1 = ent("p3h-node1")
        before = runs(n1)
        self.assertEqual(self.stage(n1, img(TW), SOLARI_PROV_DOMAIN="Lab;example").returncode, 1)
        self.assertEqual(self.stage(n1, img(TW), ADMIN_USER="root user").returncode, 1)
        self.assertEqual(runs(n1), before)
        self.assertEqual(self.calls(), [])

    def test_forward_only_status(self):
        n1 = ent("p3h-node1")
        p = self.stage(n1, img(TW), FAKE_PROVISION_RC="1")
        self.assertEqual(p.returncode, 1)
        rid = runs(n1)[-1]["id"]
        self.assertEqual(runs(n1)[-1]["status"], "failed")
        snippet = (
            "import sys; sys.path.insert(0, sys.argv[1]); import provlib\n"
            "conn = provlib.connect_sor(); rid = int(sys.argv[2]); out = []\n"
            "for st in ('running', 'queued', 'succeeded'):\n"
            "    try:\n"
            "        provlib.advance_run(conn, rid, st, 'x'); out.append(st + ':moved')\n"
            "    except provlib.ProvError:\n"
            "        out.append(st + ':refused')\n"
            "try:\n"
            "    with conn.cursor() as c:\n"
            "        c.execute(\"UPDATE prov_runs SET status='running' WHERE id=%s\", (rid,))\n"
            "    conn.commit(); out.append('sql:moved')\n"
            "except Exception:\n"
            "    conn.rollback(); out.append('sql:refused')\n"
            "print(' '.join(out))\n")
        proc = subprocess.run([PY, "-c", snippet, FLEET, str(rid)], env=self.env,
                              capture_output=True, text=True, timeout=60)
        self.assertEqual(proc.stdout.strip(),
                         "running:refused queued:refused succeeded:refused sql:refused",
                         proc.stderr)
        self.assertEqual(runs(n1)[-1]["status"], "failed")


class ProvFinish(StageMixin, Base):
    """prov-finish.py: first-boot detection -> un-stage -> stacks -> terminal status."""
    CFG = "02-00-00-00-00-02"

    def staged(self) -> int:
        p = self.stage(ent("p3h-node1"), img(TW))
        self.assertEqual(p.returncode, 0, p.stdout + p.stderr)
        rid = runs(ent("p3h-node1"))[-1]["id"]
        # what fleet-provision.sh --run-id would have published (the fake does not)
        self.http = os.path.join(self.prov_root, "netboot", "http")
        self.pdir = os.path.join(self.prov_root, "profiles", self.CFG)
        for rel, body in ((f"ipxe/mac-{self.CFG}.ipxe", "#!ipxe\n"),
                          (f"configs/{self.CFG}.xml", "<profile/>\n")):
            os.makedirs(os.path.dirname(os.path.join(self.http, rel)), exist_ok=True)
            with open(os.path.join(self.http, rel), "w") as fh:
                fh.write(body)
        os.makedirs(self.pdir, exist_ok=True)
        self.write_profile_env(rid)
        if os.path.exists(self.log_path):
            os.unlink(self.log_path)
        return rid

    def write_profile_env(self, rid):
        with open(os.path.join(self.pdir, "profile.env"), "w") as fh:
            fh.write(f"mac={self.CFG.replace('-', ':')}\nrun_id={rid}\n")

    def finish(self, rid, **env):
        p = self.run_helper(PROV_FINISH, "--run", str(rid), **env)
        return p

    def status(self, rid):
        st, detail = admin(f"SELECT status, IFNULL(detail,'') FROM prov_runs WHERE id={rid}").split("\t")
        return st, detail

    def staged_files(self):
        return {name: os.path.exists(path) for name, path in (
            ("ipxe", os.path.join(self.http, "ipxe", f"mac-{self.CFG}.ipxe")),
            ("xml", os.path.join(self.http, "configs", f"{self.CFG}.xml")),
            ("profile", os.path.join(self.pdir, "profile.env")))}

    def test_waits_until_marker_matches(self):
        rid = self.staged()
        for marker in (None, '{"run_id": 999999}'):
            with self.subTest(marker=marker):
                p = self.finish(rid, FAKE_MARKER=marker)
                self.assertEqual(p.returncode, 0, p.stdout + p.stderr)
                self.assertIn(f"run {rid}: waiting", p.stdout)
                self.assertEqual(self.status(rid), ("running", "staged; awaiting netboot install"))
                self.assertEqual(set(self.staged_files().values()), {True})
        probes = self.calls("ansible")
        self.assertEqual(len(probes), 2)
        kh = os.path.join(self.prov_root, "runs", f"reimage-{rid}.known_hosts")
        for c in probes:
            self.assertEqual(c["argv"][:6], ["all", "-i", "p3h-node1.akoria.net,", "-u",
                                             "solari-admin", "-m"])
            self.assertEqual(c["argv"][-1], "cat /etc/ansible/facts.d/solari_prov.fact")
            self.assertEqual(c["ssh_args"], f"-o UserKnownHostsFile={kh} "
                             "-o StrictHostKeyChecking=accept-new")
            self.assertEqual((c["hkc"], c["become"]), ("True", "False"))
            self.assertEqual(c["cwd"], os.path.join(self.prov_root, "ansible"))
        self.assertEqual(self.calls("ansible-playbook"), [])

    def test_marker_match_applies_and_unstages(self):
        rid = self.staged()
        p = self.finish(rid, FAKE_MARKER=f'{{"run_id": {rid}}}')
        self.assertEqual(p.returncode, 0, p.stdout + p.stderr)
        self.assertEqual(self.status(rid), ("succeeded", "installed; stacks applied"))
        self.assertTrue(runs(ent("p3h-node1"))[-1]["finished"])
        self.assertEqual(set(self.staged_files().values()), {False})
        self.assertTrue(os.path.exists(os.path.join(self.pdir, f"finished-{rid}.env")))
        kh = os.path.join(self.prov_root, "runs", f"reimage-{rid}.known_hosts")
        pinned = f"-o UserKnownHostsFile={kh} -o StrictHostKeyChecking=yes"
        ans = self.calls("ansible")
        self.assertEqual([c["argv"][c["argv"].index("-m") + 1] for c in ans],
                         ["ansible.builtin.raw", "ansible.builtin.raw", "ansible.builtin.setup"])
        self.assertIn("zypper --non-interactive install", ans[1]["argv"][-1])
        self.assertEqual((ans[1]["become"], ans[1]["ssh_args"]), ("True", pinned))
        self.assertEqual(ans[2]["ssh_args"], pinned)
        self.assertIn("-u", ans[2]["argv"])
        pb = self.calls("ansible-playbook")
        self.assertEqual(len(pb), 2)
        vars_path = os.path.join(self.prov_root, "runs", f"reimage-{rid}.stack.json")
        for c, check in zip(pb, (True, False)):
            self.assertEqual(c["argv"][:4], ["-i", "p3h-node1.akoria.net,", "-u", "solari-admin"])
            self.assertEqual(c["argv"][c["argv"].index("-e") + 1], "@" + vars_path)
            self.assertEqual("--check" in c["argv"], check)
            self.assertEqual((c["ssh_args"], c["hkc"]), (pinned, "True"))
            self.assertEqual((c["vars_mode"], c["vars_order"]), ("0o600", ["core"]))
        self.assertFalse(os.path.exists(kh), "per-run known_hosts is removed on success")
        # terminal: a later tick does nothing
        os.unlink(self.log_path)
        p = self.finish(rid, FAKE_MARKER=f'{{"run_id": {rid}}}')
        self.assertEqual(p.returncode, 0, p.stdout + p.stderr)
        self.assertEqual(self.calls(), [])

    def test_apply_failure_retries_then_fails(self):
        rid = self.staged()
        env = {"FAKE_MARKER": f'{{"run_id": {rid}}}', "FAKE_APPLY_RC": "2",
               "SOLARI_PROV_APPLY_ATTEMPTS": "2"}
        p = self.finish(rid, **env)
        self.assertEqual(p.returncode, 0, p.stdout + p.stderr)
        self.assertEqual(self.status(rid), ("running", "installed; stack apply pass failed; "
                                            "retrying (attempt 1 of 2)"))
        self.assertEqual(set(self.staged_files().values()), {False})    # never reinstalls
        p = self.finish(rid, **env)
        self.assertEqual(p.returncode, 0, p.stdout + p.stderr)
        self.assertEqual(self.status(rid), ("failed", "installed; stack apply pass failed; "
                                            "stacks not applied (attempt 2 of 2)"))
        self.assertEqual(len(self.calls("ansible-playbook")), 4)

    def test_fact_mismatch_and_bootstrap_failure(self):
        rid = self.staged()
        p = self.finish(rid, FAKE_MARKER=f'{{"run_id": {rid}}}', FAKE_FAMILY="Debian",
                        SOLARI_PROV_APPLY_ATTEMPTS="1")
        self.assertEqual(p.returncode, 0, p.stdout + p.stderr)
        st, detail = self.status(rid)
        self.assertEqual(st, "failed")
        self.assertIn("installed system reports debian/x86_64, expected suse/x86_64", detail)
        self.assertEqual(self.calls("ansible-playbook"), [])

        rid = self.staged()
        env = {"FAKE_MARKER": f'{{"run_id": {rid}}}', "FAKE_RAW_RC": "1",
               "SOLARI_PROV_APPLY_ATTEMPTS": "2"}
        self.finish(rid, **env)
        st, detail = self.status(rid)
        self.assertEqual(st, "running")
        self.assertTrue(detail.startswith("installed; python3 bootstrap failed:"), detail)
        self.assertTrue(detail.endswith("(attempt 1 of 2)"), detail)
        # the installed host then drops off the network: counts as the last attempt
        self.finish(rid, FAKE_MARKER=None, SOLARI_PROV_APPLY_ATTEMPTS="2")
        self.assertEqual(self.status(rid), ("failed", "installed; installed host not reachable; "
                                            "stacks not applied (attempt 2 of 2)"))

    def test_install_timeout_disarms_profile(self):
        rid = self.staged()
        admin(f"UPDATE prov_runs SET started_at = NOW() - INTERVAL 7 HOUR WHERE id={rid}")
        p = self.finish(rid)
        self.assertEqual(p.returncode, 0, p.stdout + p.stderr)
        self.assertEqual(self.status(rid), ("failed", "install not detected within 6 h; boot "
                                            "profile disarmed; re-stage to retry"))
        self.assertEqual(set(self.staged_files().values()), {False})
        self.assertTrue(os.path.exists(os.path.join(self.pdir, f"disarmed-{rid}.ipxe")))
        self.assertTrue(os.path.exists(os.path.join(self.pdir, f"disarmed-{rid}.env")))
        self.assertEqual(self.calls(), [])                  # no probe once timed out

    def test_newer_staging_profile_left_alone(self):
        rid = self.staged()
        self.write_profile_env(rid + 1000)                  # a newer PROV_STAGE owns the MAC
        admin(f"UPDATE prov_runs SET started_at = NOW() - INTERVAL 7 HOUR WHERE id={rid}")
        p = self.finish(rid, SOLARI_PROV_INSTALL_TIMEOUT="3600")
        self.assertEqual(p.returncode, 0, p.stdout + p.stderr)
        self.assertEqual(self.status(rid), ("failed", "install not detected within 1 h; boot "
                                            "profile left to the newer staging; re-stage to retry"))
        self.assertEqual(set(self.staged_files().values()), {True})

    def test_legacy_staging_and_malformed_records(self):
        rid = self.staged()
        os.unlink(os.path.join(self.prov_root, "runs", f"reimage-{rid}.json"))
        self.finish(rid)
        self.assertEqual(self.status(rid), ("failed", "staged before completion tracking (no "
                                            "run record); re-stage to retry"))

        rid = self.staged()                                 # prov-stage still mid-flight
        admin(f"UPDATE prov_runs SET detail='staging netboot install' WHERE id={rid}")
        os.unlink(os.path.join(self.prov_root, "runs", f"reimage-{rid}.json"))
        p = self.finish(rid)
        self.assertIn(f"run {rid}: staging", p.stdout)
        self.assertEqual(self.status(rid), ("running", "staging netboot install"))
        admin(f"UPDATE prov_runs SET started_at = NOW() - INTERVAL 7 HOUR WHERE id={rid}")
        self.finish(rid)
        self.assertEqual(self.status(rid), ("failed", "staging never completed (no run "
                                            "record); re-stage to retry"))

        rid = self.staged()
        path = os.path.join(self.prov_root, "runs", f"reimage-{rid}.json")
        with open(path) as fh:
            rec = json.load(fh)
        rec["fqdn"] = "p3h-node1.akoria.net -oProxyCommand=id"
        with open(path, "w") as fh:
            json.dump(rec, fh)
        self.finish(rid, FAKE_MARKER=f'{{"run_id": {rid}}}')
        self.assertEqual(self.status(rid), ("failed", "run record is malformed or does not "
                                            "match the run"))
        self.assertEqual(self.calls(), [])

    def test_usage_and_env_errors(self):
        self.assertEqual(self.run_helper(PROV_FINISH, "--run", "01").returncode, 2)
        self.assertEqual(self.run_helper(PROV_FINISH, "--run", "x").returncode, 2)
        p = self.run_helper(PROV_FINISH, SOLARI_PROV_INSTALL_TIMEOUT="5")
        self.assertEqual(p.returncode, 1)
        self.assertIn("SOLARI_PROV_INSTALL_TIMEOUT must be an integer >= 600", p.stderr)
        p = self.run_helper(PROV_FINISH, PROV_ROOT="/srv/prov root")
        self.assertEqual(p.returncode, 1)
        self.assertIn("is not a plain absolute path", p.stderr)
        self.assertEqual(self.calls(), [])


class StackApply(Base):
    def apply(self, *extra, entity=None, stacks="core", **env):
        return self.run_helper(STACK_APPLY, "--entity", entity or ent("p3h-node1"),
                               "--stacks", stacks, *extra, "--op", "jason", **env)

    def last_apply(self):
        return runs(ent("p3h-node1"), "apply")[-1]

    def expected_pb(self, rid, check):
        argv = ["-i", "p3h-node1.akoria.net,", "playbooks/site.yml", "--tags", "stack",
                "-e", f"@{self.prov_root}/runs/apply-{rid}.stack.json"]
        return argv + (["--check", "--diff"] if check else [])

    def test_real_apply_check_then_apply(self):
        p = self.apply()
        self.assertEqual(p.returncode, 0, p.stdout + p.stderr)
        run = self.last_apply()
        self.assertEqual((run["status"], run["dry_run"], run["detail"], run["image_id"]),
                         ("succeeded", 0, "applied", ""))
        self.assertTrue(run["finished"])
        facts = self.calls("ansible")
        self.assertEqual(len(facts), 1)
        self.assertEqual(facts[0]["argv"], ["all", "-i", "p3h-node1.akoria.net,", "-m",
                                            "ansible.builtin.setup", "-a",
                                            "filter=ansible_os_family,ansible_architecture"])
        self.assertEqual(facts[0]["cwd"], os.path.join(self.prov_root, "ansible"))
        self.assertEqual(facts[0]["load_cb"], "0")
        self.assertEqual(facts[0]["result_fmt"], "json")   # cfg says yaml
        pbs = self.calls("ansible-playbook")
        self.assertEqual([c["argv"] for c in pbs],
                         [self.expected_pb(run["id"], True), self.expected_pb(run["id"], False)])
        for c in pbs:
            self.assertEqual(c["cwd"], os.path.join(self.prov_root, "ansible"))
            self.assertEqual((c.get("vars_mode"), c.get("vars_order")), ("0o644", ["core"]))
            self.assertEqual(c["stdout_cb"], "ansible.builtin.default")
        self.assertIn("(check)", p.stdout)
        self.assertIn("(apply)", p.stdout)
        self.assertLess(p.stdout.index("(check)"), p.stdout.index("(apply)"))

    def test_dry_run_only_check(self):
        p = self.apply("--dry-run", stacks="dev-tools")
        self.assertEqual(p.returncode, 0, p.stdout + p.stderr)
        run = self.last_apply()
        self.assertEqual((run["status"], run["dry_run"], run["detail"], run["stacks"]),
                         ("succeeded", 1, "dry-run: check+diff only", ["core", "dev-tools"]))
        pbs = self.calls("ansible-playbook")
        self.assertEqual(len(pbs), 1)
        self.assertEqual(pbs[0]["argv"], self.expected_pb(run["id"], True))

    def test_check_failure_marks_failed(self):
        p = self.apply(FAKE_CHECK_RC="2")
        self.assertEqual(p.returncode, 1)
        run = self.last_apply()
        self.assertEqual((run["status"], run["detail"]), ("failed", "check pass failed"))
        self.assertEqual(len(self.calls("ansible-playbook")), 1)
        self.assertIn("PLAY RECAP", p.stdout)

    def test_apply_failure_marks_failed(self):
        p = self.apply(FAKE_APPLY_RC="2")
        self.assertEqual(p.returncode, 1)
        run = self.last_apply()
        self.assertEqual((run["status"], run["detail"]), ("failed", "apply pass failed"))
        self.assertEqual(len(self.calls("ansible-playbook")), 2)

    def test_concurrent_apply_refused(self):
        n1 = ent("p3h-node1")
        admin("INSERT INTO prov_runs (mode, entity_id, stack_slugs, requested_by, status, "
              f"source_id) VALUES ('apply', {n1}, '[\"core\"]', 'someone', 'queued', {SRC})")
        busy = runs(n1, "apply")[-1]
        try:
            before = len(runs(n1))
            p = self.apply()
            self.assertEqual(p.returncode, 1)
            self.assertIn(f"apply run {busy['id']} for entity {n1} is still active", p.stderr)
            self.assertEqual(len(runs(n1)), before)
            self.assertEqual(self.calls(), [])
            # a reimage run in flight does not block an apply (different mode)
        finally:
            admin(f"UPDATE prov_runs SET status='cancelled', detail='test cleanup' WHERE id={busy['id']}")

    def test_fact_handling(self):
        n1 = ent("p3h-node1")
        before = len(runs(n1))
        for env, needle in (({"FAKE_FAMILY": "Windows"}, "unsupported OS family 'Windows'"),
                            ({"FAKE_ARCH": "ppc64le"}, "unsupported architecture 'ppc64le'"),
                            ({"FAKE_FACTS_MODE": "fail"}, "fact probe of p3h-node1.akoria.net failed")):
            with self.subTest(env=env):
                p = self.apply(**env)
                self.assertEqual(p.returncode, 1)
                self.assertIn(needle, p.stderr)
                self.assertEqual(len(runs(n1)), before)
        self.assertEqual(self.calls("ansible-playbook"), [])
        # json-document output form, Debian armv7l -> debian/armhf, custom domain
        p = self.apply("--dry-run", FAKE_FACTS_MODE="json", FAKE_FAMILY="Debian",
                       FAKE_ARCH="armv7l", SOLARI_PROV_DOMAIN="lab.example")
        self.assertEqual(p.returncode, 0, p.stdout + p.stderr)
        self.assertIn("facts: Debian/armv7l -> debian/armhf", p.stdout)
        self.assertIn("p3h-node1.lab.example,", self.calls("ansible-playbook")[0]["argv"])
        p = self.apply(SOLARI_PROV_DOMAIN="bad domain;x")
        self.assertEqual(p.returncode, 1)
        self.assertIn("not a valid domain", p.stderr)

    def test_entity_and_stack_refusals(self):
        for entity, needle in ((ent("Bad_Name"), "not a valid hostname label"),
                               (ent("p3h-gone"), "does not exist or is deleted")):
            p = self.apply(entity=entity)
            self.assertEqual(p.returncode, 1)
            self.assertIn(needle, p.stderr)
        p = self.apply(stacks="core,nope")
        self.assertEqual(p.returncode, 1)
        self.assertIn("unknown or deleted stack(s): nope", p.stderr)
        self.assertEqual(self.calls(), [])


class FactMapMirror(unittest.TestCase):
    """stack-apply's maps must equal roles/stack-items/defaults/main.yml."""

    def test_maps_match_role_defaults(self):
        path = os.path.join(FLEET, "ansible", "roles", "stack-items", "defaults", "main.yml")
        maps: dict[str, dict[str, str]] = {}
        cur = None
        with open(path, encoding="utf-8") as fh:
            for line in fh:
                m = re.match(r"^(solari_stack_(?:family|arch)_map):\s*$", line)
                if m:
                    cur = maps.setdefault(m.group(1), {})
                    continue
                m = re.match(r"^\s+([A-Za-z0-9_]+):\s*([A-Za-z0-9_]+)\s*(?:#.*)?$", line)
                if cur is not None and m:
                    cur[m.group(1)] = m.group(2)
                elif not line.startswith((" ", "\t")):
                    cur = None
        spec = importlib.util.spec_from_file_location("stack_apply", STACK_APPLY)
        mod = importlib.util.module_from_spec(spec)
        sys.path.insert(0, FLEET)
        spec.loader.exec_module(mod)
        self.assertEqual(mod.FAMILY_MAP, maps["solari_stack_family_map"])
        self.assertEqual(mod.ARCH_MAP, maps["solari_stack_arch_map"])


class AnsibleCfg(unittest.TestCase):
    """ansible.cfg must not name a callback ansible-core no longer ships."""

    def test_stdout_callback_is_builtin(self):
        import configparser
        cfg = configparser.ConfigParser(inline_comment_prefixes=None)
        cfg.read(os.path.join(FLEET, "ansible", "ansible.cfg"), encoding="utf-8")
        d = cfg["defaults"]
        # `yaml` left ansible-core (community.general) and aborts every
        # ansible-playbook run with "Could not load 'yaml' callback plugin".
        self.assertEqual(d.get("stdout_callback"), "ansible.builtin.default")
        self.assertEqual(d.get("callback_result_format"), "yaml")
        for cb in (c.strip() for c in d.get("callbacks_enabled", "").split(",")):
            self.assertFalse(cb.startswith("ansible.builtin.") or cb in {"yaml", "json"}, cb)


def main() -> int:
    if PY != sys.executable and subprocess.run([PY, "-c", "import pymysql"],
                                                capture_output=True).returncode != 0 \
            or PY == sys.executable and importlib.util.find_spec("pymysql") is None:
        print(f"need pymysql in {PY} (set STACKGEN_TEST_PYTHON)", file=sys.stderr)
        return 2
    if not db_reachable():
        if os.environ.get("PROV_HELPERS_REQUIRE_DB") == "1":
            print("FAIL: no scratch MariaDB reachable and PROV_HELPERS_REQUIRE_DB=1", file=sys.stderr)
            return 1
        print("SKIP: test_prov_helpers — no MariaDB reachable (sudo -n mariadb / "
              "SOLARI_TEST_SOR_DB=1); set PROV_HELPERS_REQUIRE_DB=1 to make this fatal")
        return 0
    tmp = tempfile.mkdtemp(prefix="test-prov-helpers-")
    STATE["tmp"] = tmp
    try:
        build_db()
        make_fakes(tmp)
        print(f"== scratch {DB}: schema.sql + {STATE['migrations']} migrations ==")
        suite = unittest.defaultTestLoader.loadTestsFromModule(sys.modules[__name__])
        result = unittest.TextTestRunner(verbosity=2).run(suite)
        return 0 if result.wasSuccessful() else 1
    finally:
        drop_db()
        shutil.rmtree(tmp, ignore_errors=True)


if __name__ == "__main__":
    sys.exit(main())
