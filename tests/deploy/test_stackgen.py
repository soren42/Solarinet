#!/usr/bin/env python3
"""Unit + golden tests for deploy/fleet/stackgen.py (stdlib unittest; golden
legs need PyYAML to read group_vars/all.yml).

Run: python3 tests/deploy/test_stackgen.py
"""
from __future__ import annotations

import copy
import io
import json
import os
import random
import sys
import tempfile
import unittest
from contextlib import redirect_stderr, redirect_stdout

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
sys.path.insert(0, ROOT)

from deploy.fleet import stackgen  # noqa: E402
from deploy.fleet.stackgen import StackGenError  # noqa: E402

FIXTURE = os.path.join(ROOT, "tests", "deploy", "fixtures", "stacks_seed.json")
ALL_YML = os.path.join(ROOT, "deploy", "fleet", "ansible", "group_vars", "all.yml")
SHA = "a" * 64
SHA2 = "b" * 64
FPR = "0123456789ABCDEF0123456789ABCDEF01234567"


def seed_doc() -> dict:
    with open(FIXTURE, encoding="utf-8") as fh:
        return json.load(fh)


def render_doc(doc, stacks=(), family="suse", arch="x86_64"):
    return stackgen.render(stackgen.load_definitions(doc), list(stacks), family, arch)


def pkg(name, suse=None, debian=None, fedora=None, **kw):
    same = name
    return {"logical_name": name, "item_type": "pkg",
            "names": {"suse": suse or same, "debian": debian or same, "fedora": fedora or same}, **kw}


def other(name, itype, fedora=True, **kw):
    return {"logical_name": name, "item_type": itype,
            "names": {"suse": "=", "debian": "=", "fedora": "=" if fedora else None}, **kw}


def script(name, **spec):
    spec.setdefault("body", "echo hi")
    spec.setdefault("creates", "/opt/x")
    return other(name, "script", spec=spec)


COMMIT = "0123456789abcdef0123456789abcdef01234567"


def build(name, pin=COMMIT, **spec):
    spec.setdefault("source", {"git": "https://example.invalid/x.git"})
    spec.setdefault("system", "cmake")
    spec.setdefault("creates", "/usr/local/bin/x")
    return other(name, "build", version_pin=pin, spec=spec)


def doc_with(*stacks):
    core = {"slug": "core", "locked": True, "sort_order": 0, "items": [pkg("git"), pkg("make")]}
    return {"stacks": [core, *stacks]}


class GoldenAgainstAllYml(unittest.TestCase):
    """The seeded core stack must equal today's group_vars/all.yml resolution."""

    @classmethod
    def setUpClass(cls):
        try:
            import yaml
        except ImportError:  # pragma: no cover - CI installs pyyaml
            raise unittest.SkipTest("PyYAML not installed")
        with open(ALL_YML, encoding="utf-8") as fh:
            cls.all_vars = yaml.safe_load(fh)

    def expected(self, ansible_family):
        pmap = self.all_vars.get("solari_pkg_map") or {}
        out = []
        for logical in self.all_vars["solari_core_packages"]:
            name = (pmap.get(logical) or {}).get(ansible_family, logical)
            if name not in out:
                out.append(name)
        return out

    def test_core_suse_x86_64_equals_all_yml(self):
        got = render_doc(seed_doc(), family="suse", arch="x86_64")
        self.assertEqual(got["solari_stack_order"], ["core"])
        self.assertEqual(got["solari_stack_packages"], self.expected("Suse"))
        self.assertEqual(got["solari_stack_steps"],
                         [{"type": "pkg", "packages": self.expected("Suse")}])
        for k in ("repos", "artifacts", "builds", "scripts", "roles", "skipped"):
            self.assertEqual(got[f"solari_stack_{k}"], [], k)

    def test_core_debian_x86_64_equals_all_yml(self):
        got = render_doc(seed_doc(), family="debian", arch="x86_64")
        self.assertEqual(got["solari_stack_packages"], self.expected("Debian"))

    def test_core_is_arch_independent(self):
        a = render_doc(seed_doc(), family="suse", arch="x86_64")["solari_stack_packages"]
        b = render_doc(seed_doc(), family="suse", arch="arm64")["solari_stack_packages"]
        self.assertEqual(a, b)


class SeedFixture(unittest.TestCase):
    def test_dev_tools_seed_render(self):
        got = render_doc(seed_doc(), ["dev-tools"], "debian", "arm64")
        self.assertEqual(got["solari_stack_order"], ["core", "dev-tools"])
        self.assertEqual(got["solari_stack_roles"], ["dev-tools"])
        self.assertEqual([s["type"] for s in got["solari_stack_steps"]], ["pkg", "role", "script"],
                         "p10k pin (item_order 700) must run after the dev-tools role")
        p10k = got["solari_stack_scripts"][0]
        self.assertEqual(p10k["version"], "ff0311157d6b24fea21aa70699783f362b0f554f")
        self.assertEqual(p10k["check"], ["git", "-C", "/usr/share/oh-my-zsh/custom/themes/powerlevel10k",
                                         "rev-parse", "HEAD"])
        self.assertIsNone(p10k["url"])
        self.assertIn("ff0311157d6b24fea21aa70699783f362b0f554f", p10k["body"])

    def test_fedora_core_is_a_hard_error_on_netcat(self):
        with self.assertRaisesRegex(StackGenError, r"'netcat'.*unmapped package for family fedora"):
            render_doc(seed_doc(), family="fedora")


class StackOrder(unittest.TestCase):
    def stacks(self):
        return doc_with(
            {"slug": "web", "sort_order": 20, "items": [pkg("nginx")]},
            {"slug": "ai", "sort_order": 10, "depends_on": ["web"], "items": [pkg("python3-torch")]},
            {"slug": "db", "sort_order": 20, "items": [pkg("mariadb")]},
            {"slug": "aaa", "sort_order": 5, "items": [pkg("aaa")]},
        )

    def test_closure_topological_with_ties(self):
        got = render_doc(self.stacks(), ["ai", "db"])
        # ai pulls web; web+db tie on sort_order 20 -> slug; ai after web.
        self.assertEqual(got["solari_stack_order"], ["core", "db", "web", "ai"])

    def test_core_first_even_with_lower_sort_order(self):
        got = render_doc(self.stacks(), ["aaa"])
        self.assertEqual(got["solari_stack_order"], ["core", "aaa"])

    def test_locked_always_included(self):
        self.assertEqual(render_doc(self.stacks(), [])["solari_stack_order"], ["core"])

    def test_unknown_slug(self):
        with self.assertRaisesRegex(StackGenError, "unknown stack slug 'nope'"):
            render_doc(self.stacks(), ["nope"])

    def test_unknown_dependency(self):
        with self.assertRaisesRegex(StackGenError, "unknown stack 'ghost'"):
            render_doc(doc_with({"slug": "x", "depends_on": ["ghost"]}), ["x"])

    def test_cycle(self):
        d = doc_with({"slug": "a", "depends_on": ["b"]}, {"slug": "b", "depends_on": ["c"]},
                     {"slug": "c", "depends_on": ["a"]})
        with self.assertRaisesRegex(StackGenError, r"cycle: (a|b|c) -> .* -> (a|b|c)"):
            render_doc(d, ["a"])

    def test_self_dependency(self):
        with self.assertRaisesRegex(StackGenError, "depends on itself"):
            render_doc(doc_with({"slug": "a", "depends_on": ["a"]}), ["a"])

    def test_locked_depending_on_unlocked_is_a_cycle(self):
        d = doc_with({"slug": "x"})
        d["stacks"][0]["depends_on"] = ["x"]
        with self.assertRaisesRegex(StackGenError, "cycle"):
            render_doc(d)

    def test_deterministic_regardless_of_input_order(self):
        base = stackgen.dumps(render_doc(self.stacks(), ["ai", "db"]))
        rng = random.Random(42)
        for _ in range(10):
            d = copy.deepcopy(self.stacks())
            rng.shuffle(d["stacks"])
            for s in d["stacks"]:
                rng.shuffle(s["items"])
            self.assertEqual(stackgen.dumps(render_doc(d, ["db", "ai"])), base)


class ItemOrderAndTypes(unittest.TestCase):
    def mixed(self):
        repo_spec = {"debian": {"url": "https://example.invalid/apt", "key_url": "https://example.invalid/k.asc",
                                "fingerprint": FPR, "suite": "bookworm", "components": ["main"]},
                     "suse": {"url": "https://example.invalid/rpm", "key_url": "https://example.invalid/k.asc",
                              "fingerprint": FPR},
                     "fedora": {"url": "https://example.invalid/rpm", "key_url": "https://example.invalid/k.asc",
                                "fingerprint": FPR}}
        return doc_with({"slug": "x", "items": [
            other("r", "role", ansible_role="some-role"),
            script("s"),
            build("b"),
            other("a", "artifact", version_pin="1.0", artifacts={"any": {"url": "https://e.invalid/a", "sha256": SHA}}),
            pkg("p", repo_ref="vendor repo"),
            other("vendor repo", "repo", spec=repo_spec),
        ]})

    def test_default_type_order(self):
        got = render_doc(self.mixed(), ["x"], "debian")
        self.assertEqual([s["type"] for s in got["solari_stack_steps"]],
                         ["pkg", "repo", "pkg", "artifact", "build", "script", "role"])
        repo = got["solari_stack_repos"][0]
        self.assertEqual((repo["suite"], repo["components"], repo["fingerprint"]), ("bookworm", ["main"], FPR))
        self.assertEqual(got["solari_stack_packages"], ["git", "make", "p"])

    def test_explicit_item_order(self):
        d = self.mixed()
        d["stacks"][1]["items"][1]["item_order"] = 650  # script after role
        d["stacks"][1]["items"][2]["item_order"] = 50   # build before repo
        got = render_doc(d, ["x"], "suse")
        self.assertEqual([s["type"] for s in got["solari_stack_steps"]],
                         ["pkg", "build", "repo", "pkg", "artifact", "role", "script"])

    def test_repo_ref_must_be_applied_first(self):
        d = self.mixed()
        d["stacks"][1]["items"][4]["item_order"] = 50  # pkg before its repo
        with self.assertRaisesRegex(StackGenError, "repo_ref 'vendor repo' is not a repo applied before it"):
            render_doc(d, ["x"], "suse")

    def test_repo_bad_fingerprint_and_missing_suite(self):
        d = self.mixed()
        d["stacks"][1]["items"][5]["spec"]["suse"]["fingerprint"] = "DEADBEEF"
        with self.assertRaisesRegex(StackGenError, "full 40/64-hex key fingerprint"):
            render_doc(d, ["x"], "suse")
        d = self.mixed()
        del d["stacks"][1]["items"][5]["spec"]["debian"]["suite"]
        with self.assertRaisesRegex(StackGenError, "debian repo needs suite"):
            render_doc(d, ["x"], "debian")

    def test_unsupported_role_family_is_error(self):
        d = doc_with({"slug": "x", "items": [other("r", "role", fedora=False, ansible_role="r")]})
        with self.assertRaisesRegex(StackGenError, "unmapped role for family fedora"):
            render_doc(d, ["x"], "fedora")

    def test_applicability_skips_and_reports(self):
        d = doc_with({"slug": "x", "items": [
            pkg("only-debian", applies_families=["debian"]),
            pkg("only-arm", applies_arches="arm64,armhf"),
        ]})
        got = render_doc(d, ["x"], "suse", "x86_64")
        self.assertEqual(got["solari_stack_packages"], ["git", "make"])
        self.assertEqual([s["item"] for s in got["solari_stack_skipped"]], ["only-arm", "only-debian"])
        got = render_doc(d, ["x"], "debian", "arm64")
        self.assertEqual(got["solari_stack_packages"], ["git", "make", "only-arm", "only-debian"])

    def test_pkg_rejects_same_sentinel_and_role_rejects_names(self):
        with self.assertRaisesRegex(StackGenError, "real package name"):
            render_doc(doc_with({"slug": "x", "items": [pkg("p", suse="=")]}), ["x"])
        bad = other("r", "role", ansible_role="r")
        bad["names"]["suse"] = "some-name"
        with self.assertRaisesRegex(StackGenError, r"must be '=' or null"):
            render_doc(doc_with({"slug": "x", "items": [bad]}), ["x"])


class Artifacts(unittest.TestCase):
    def art(self, artifacts, pin="2.0"):
        return doc_with({"slug": "x", "items": [other("tool", "artifact", version_pin=pin, artifacts=artifacts)]})

    def test_exact_arch_wins_over_any(self):
        d = self.art({"any": {"url": "u-any", "sha256": SHA}, "arm64": {"url": "u-arm", "sha256": SHA2}})
        self.assertEqual(render_doc(d, ["x"], arch="arm64")["solari_stack_artifacts"][0]["url"], "u-arm")
        a = render_doc(d, ["x"], arch="x86_64")["solari_stack_artifacts"][0]
        self.assertEqual((a["url"], a["artifact_arch"]), ("u-any", "any"))

    def test_missing_arch_artifact(self):
        with self.assertRaisesRegex(StackGenError, "no artifact for arch riscv64"):
            render_doc(self.art({"x86_64": {"url": "u", "sha256": SHA}}), ["x"], arch="riscv64")

    def test_missing_sha256(self):
        with self.assertRaisesRegex(StackGenError, "requires a sha256"):
            render_doc(self.art({"any": {"url": "u", "sha256": None}}), ["x"])

    def test_missing_pin(self):
        with self.assertRaisesRegex(StackGenError, "needs a version_pin"):
            render_doc(self.art({"any": {"url": "u", "sha256": SHA}}, pin=None), ["x"])

    def test_artifacts_on_pkg_rejected(self):
        with self.assertRaisesRegex(StackGenError, "per-arch artifacts on a pkg item"):
            render_doc(doc_with({"slug": "x", "items": [pkg("p", artifacts={"any": {"url": "u"}})]}), ["x"])


class Scripts(unittest.TestCase):
    def one(self, item):
        return render_doc(doc_with({"slug": "x", "items": [item]}), ["x"])

    def test_unpinned_remote_script_rejected(self):
        with self.assertRaisesRegex(StackGenError, "unpinned remote script"):
            self.one(script("s", body=None, url="https://get.example.invalid/install.sh"))

    def test_pinned_remote_script_ok(self):
        s = script("s", url="https://get.example.invalid/install.sh", sha256=SHA)
        del s["spec"]["body"]
        got = self.one(s)["solari_stack_scripts"][0]
        self.assertEqual((got["url"], got["sha256"], got["body"]), ("https://get.example.invalid/install.sh", SHA, None))
        self.assertEqual((got["interpreter"], got["run_as"]), ("/bin/sh", "root"))

    def test_body_and_url_together_rejected(self):
        with self.assertRaisesRegex(StackGenError, "exactly one of body"):
            self.one(script("s", url="https://x.invalid/i.sh", sha256=SHA))

    def test_guard_required(self):
        s = script("s")
        del s["spec"]["creates"]
        with self.assertRaisesRegex(StackGenError, "idempotency guard"):
            self.one(s)

    def test_version_check_needs_check_and_pin(self):
        with self.assertRaisesRegex(StackGenError, "version_check needs both"):
            self.one(script("s", check=["x", "--version"], version_check=r"(\d+)"))
        s = script("s", check=["x", "--version"], version_check=r"\d+")
        s["version_pin"] = "1"
        with self.assertRaisesRegex(StackGenError, "capture group"):
            self.one(s)

    def test_check_must_be_argv_list_not_shell_string(self):  # review finding 3
        for bad in ("test -x /opt/x && rm -rf /", [], [""], ["ok", 3], {"cmd": "x"}):
            with self.subTest(check=bad), self.assertRaisesRegex(StackGenError, "argv list"):
                self.one(script("s", check=bad))
        got = self.one(script("s", check=["/opt/x/bin/x", "--version"]))["solari_stack_scripts"][0]
        self.assertEqual(got["check"], ["/opt/x/bin/x", "--version"])

    def test_secret_env_rejected(self):
        with self.assertRaisesRegex(StackGenError, "looks like a secret"):
            self.one(script("s", env={"API_TOKEN": "x"}))
        got = self.one(script("s", env={"Z": "1", "A": "2"}))["solari_stack_scripts"][0]
        self.assertEqual(list(got["env"]), ["A", "Z"])

    def test_bad_run_as_and_unknown_key(self):
        with self.assertRaisesRegex(StackGenError, "run_as"):
            self.one(script("s", run_as="nobody"))
        with self.assertRaisesRegex(StackGenError, "unknown spec key"):
            self.one(script("s", sudo=True))


class Builds(unittest.TestCase):
    def test_build_deps_merged_into_packages_before_build(self):
        d = doc_with(
            {"slug": "toolchain", "items": [pkg("cmake"), pkg("libssl-dev", suse="libopenssl-devel")]},
            {"slug": "x", "depends_on": ["toolchain"],
             "items": [build("thing", build_deps=["git", "cmake", "libssl-dev"])]},
        )
        got = render_doc(d, ["x"], "suse")
        self.assertEqual(got["solari_stack_order"], ["core", "toolchain", "x"])
        self.assertEqual(got["solari_stack_steps"], [  # adjacent pkg steps merge
            {"type": "pkg", "packages": ["git", "make", "cmake", "libopenssl-devel"]},
            {"type": "build", "index": 0}])
        self.assertEqual(got["solari_stack_packages"], ["git", "make", "cmake", "libopenssl-devel"])
        b = got["solari_stack_builds"][0]
        self.assertEqual(b["build_deps"], ["git", "cmake", "libopenssl-devel"])
        self.assertEqual(b["source"], {"git": "https://example.invalid/x.git", "commit": COMMIT, "tag": None,
                                       "tarball": None, "sha256": None})

    def test_build_deps_never_resolve_from_unselected_stacks(self):  # review finding 4
        d = doc_with(
            {"slug": "toolchain", "items": [pkg("cmake")]},
            {"slug": "x", "items": [build("thing", build_deps=["cmake"])]},
        )
        with self.assertRaisesRegex(StackGenError, r"build dep 'cmake' is not a pkg item in the selected stacks"):
            render_doc(d, ["x"])

    def test_build_dep_applicability_enforced(self):  # review finding 4
        for kw, msg in (({"applies_families": ["debian"]}, "not applicable to family suse"),
                        ({"applies_arches": ["arm64"]}, "not applicable to arch x86_64")):
            d = doc_with({"slug": "x", "items": [pkg("ninja", **kw), build("t", build_deps=["ninja"])]})
            with self.subTest(kw=kw), self.assertRaisesRegex(StackGenError, msg):
                render_doc(d, ["x"])

    def test_build_dep_repo_must_precede_build(self):  # review finding 4
        repo = other("vendor repo", "repo", spec={"suse": {"url": "https://r.invalid", "key_url": "https://r.invalid/k",
                                                           "fingerprint": "A" * 40}})
        dep = pkg("vendor-lib", repo_ref="vendor repo")
        d = doc_with({"slug": "x", "items": [
            dict(repo, item_order=900), dict(dep, item_order=950), build("t", build_deps=["vendor-lib"])]})
        with self.assertRaisesRegex(StackGenError, "needs repo 'vendor repo', which is not applied before this build"):
            render_doc(d, ["x"])
        d = doc_with({"slug": "x", "items": [repo, dep, build("t", build_deps=["vendor-lib"])]})
        self.assertEqual([s["type"] for s in render_doc(d, ["x"])["solari_stack_steps"]],
                         ["pkg", "repo", "pkg", "build"])

    def test_build_dep_pkg_step_inserted_mid_stack(self):
        d = doc_with({"slug": "x", "items": [
            build("thing", build_deps=["ninja"]),
            other("r", "role", ansible_role="r"),
            pkg("ninja", debian="ninja-build", item_order=900),
        ]})
        got = render_doc(d, ["x"], "debian")
        self.assertEqual(got["solari_stack_steps"], [
            {"type": "pkg", "packages": ["git", "make", "ninja-build"]},
            {"type": "build", "index": 0}, {"type": "role", "index": 0}])

    def test_unresolvable_build_dep(self):
        with self.assertRaisesRegex(StackGenError, "build dep 'libfoo' is not a pkg item"):
            render_doc(doc_with({"slug": "x", "items": [build("t", build_deps=["libfoo"])]}), ["x"])

    def test_unmapped_build_dep(self):
        d = doc_with({"slug": "x", "items": [pkg("only-deb", suse=None), build("t", build_deps=["only-deb"])]})
        d["stacks"][1]["items"][0]["names"]["suse"] = None
        with self.assertRaisesRegex(StackGenError, "unmapped"):
            render_doc(d, ["x"], "suse")

    def test_git_build_requires_full_commit_sha(self):  # review finding 2
        for ref in ("master", "feature/release", "release", "v1.0.0", COMMIT[:12], COMMIT.upper(), None):
            with self.subTest(ref=ref), self.assertRaisesRegex(StackGenError, "full commit SHA|safe path component"):
                render_doc(doc_with({"slug": "x", "items": [build("t", pin=ref)]}), ["x"])
        b = render_doc(doc_with({"slug": "x", "items": [
            build("t", source={"git": "https://e.invalid/x.git", "tag": "v1.0.0"})]}), ["x"])["solari_stack_builds"][0]
        self.assertEqual((b["source"]["commit"], b["source"]["tag"], b["version"]), (COMMIT, "v1.0.0", COMMIT))
        self.assertFalse(hasattr(stackgen, "UNPINNED_REFS"), "branch denylist heuristic must be gone")

    def test_version_pin_must_be_a_safe_path_component(self):  # review finding 1
        bad = ("../../../../etc/cron.d", "..", ".", "a/b", "/abs", "-rf", "", "x" * 65, "v1 .0", "v1\n")
        for pin in bad:
            d = doc_with({"slug": "x", "items": [other("tool", "artifact", version_pin=pin,
                                                       artifacts={"any": {"url": "https://e.invalid/a", "sha256": SHA}})]})
            with self.subTest(pin=pin), self.assertRaisesRegex(StackGenError, "safe path component"):
                render_doc(d, ["x"])
        for pin in ("1.2.3", "v1.20.0", "2024.10+git~r1", "1.0-rc1", COMMIT):
            d = doc_with({"slug": "x", "items": [other("tool", "artifact", version_pin=pin,
                                                       artifacts={"any": {"url": "https://e.invalid/a", "sha256": SHA}})]})
            self.assertEqual(render_doc(d, ["x"])["solari_stack_artifacts"][0]["version"], pin)

    def test_tarball_requires_sha256(self):
        d = doc_with({"slug": "x", "items": [build("t", source={"tarball": "https://e.invalid/t.tar.gz"})]})
        with self.assertRaisesRegex(StackGenError, "build tarball requires a sha256"):
            render_doc(d, ["x"])
        d["stacks"][1]["items"][0]["spec"]["source"]["sha256"] = SHA
        self.assertEqual(render_doc(d, ["x"])["solari_stack_builds"][0]["source"]["sha256"], SHA)

    def test_custom_needs_commands_and_others_refuse_them(self):
        with self.assertRaisesRegex(StackGenError, "custom build needs commands"):
            render_doc(doc_with({"slug": "x", "items": [build("t", system="custom")]}), ["x"])
        with self.assertRaisesRegex(StackGenError, "only valid for system custom"):
            render_doc(doc_with({"slug": "x", "items": [build("t", commands="make")]}), ["x"])
        with self.assertRaisesRegex(StackGenError, "build system must be"):
            render_doc(doc_with({"slug": "x", "items": [build("t", system="bazel")]}), ["x"])


class Cli(unittest.TestCase):
    def test_from_json_ok_and_error_exit_codes(self):
        out = io.StringIO()
        with redirect_stdout(out):
            rc = stackgen.main(["--from-json", FIXTURE, "--family", "suse", "--arch", "x86_64",
                                "--stack", "dev-tools"])
        self.assertEqual(rc, 0)
        self.assertEqual(json.loads(out.getvalue())["solari_stack_order"], ["core", "dev-tools"])
        err = io.StringIO()
        with redirect_stderr(err):
            rc = stackgen.main(["--from-json", FIXTURE, "--family", "suse", "--arch", "x86_64", "--stack", "zz"])
        self.assertEqual(rc, 1)
        self.assertIn("unknown stack slug 'zz'", err.getvalue())

    def test_output_file_and_byte_determinism(self):
        with tempfile.TemporaryDirectory() as td:
            paths = [os.path.join(td, f"{i}.json") for i in range(2)]
            for p in paths:
                self.assertEqual(stackgen.main(["--from-json", FIXTURE, "--family", "debian", "--arch", "arm64",
                                                "--stack", "dev-tools", "--output", p]), 0)
            with open(paths[0], "rb") as a, open(paths[1], "rb") as b:
                self.assertEqual(a.read(), b.read())

    def test_from_db_without_env_is_error(self):
        env = {k: v for k, v in os.environ.items() if not k.startswith("SOR_DB_")}
        old = os.environ.copy()
        os.environ.clear()
        os.environ.update(env)
        try:
            err = io.StringIO()
            with redirect_stderr(err):
                rc = stackgen.main(["--from-db", "--family", "suse", "--arch", "x86_64"])
        finally:
            os.environ.clear()
            os.environ.update(old)
        self.assertEqual(rc, 1)
        self.assertIn("SOR_DB_NAME", err.getvalue())


if __name__ == "__main__":
    unittest.main(verbosity=2)
