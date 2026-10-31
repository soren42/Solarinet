#!/usr/bin/env python3
"""stackgen — render SolariNet provisioning stacks into Ansible vars.

Reads the software POOL and the stacks that reference it (the SoR tables
prov_software / prov_software_artifacts / prov_software_requires /
prov_software_conflicts / prov_stacks / prov_stack_members / prov_stack_deps
from migration 029, or the same shape as JSON) and renders, for ONE target
family + arch, the deterministic JSON vars consumed by the `stack-items`
Ansible role (via common-stack):

  solari_stack_family / solari_stack_arch   target the vars were rendered for
  solari_stack_order       dependency-closed stack slugs, locked (Core) first
  solari_stack_repos       third-party repos      (item_type repo)
  solari_stack_packages    distro package names   (item_type pkg)
  solari_stack_artifacts   pinned downloads       (item_type artifact)
  solari_stack_builds      source builds          (item_type build)
  solari_stack_scripts     install scripts        (item_type script)
  solari_stack_roles       Ansible roles          (item_type role)
  solari_stack_steps       the execution sequence over the lists above
  solari_stack_skipped     member occurrences not applicable to this family/arch
  solari_stack_shared      member occurrences already installed earlier (once only)
  solari_stack_implicit    entries installed only because another entry requires them

Rules this generator owns (the schema cannot express them):
  * closure = requested stacks + every locked stack + transitive depends_on;
  * order = topological; every unlocked stack implicitly depends on every
    locked stack; ties broken by (locked first, sort_order, slug);
  * within a stack: member item_order when set, else repo 100 -> pkg 200 ->
    artifact 300 -> build 400 -> script 500 -> role 600;
  * install once: a pool entry is installed at its FIRST position in run
    order; every later occurrence is reported in solari_stack_shared;
  * requirements (A requires B) resolve against the POOL: B is installed
    before A. If no selected stack lists B it is installed implicitly just
    before A (reported in solari_stack_implicit); if a selected stack lists B
    LATER, a pkg is pulled forward and anything else is a hard error. B must
    apply to the target family/arch;
  * HARD errors: unknown slug, stack dependency or requirement cycle, unknown
    member / requirement / conflict target, conflicting entries both in the
    installed set (from stacks or via requirements), requirement not
    applicable to the target or installed after its dependent, unmapped
    package (NULL family name on an applicable entry), artifact without a
    sha256 for the arch, unpinned remote script, build without a pinned ref /
    tarball sha256, script/build without an idempotency guard, repo fields
    that could inject config lines, control characters in a logical_name,
    secret-looking env names (a heuristic only, see _secret_env_name).

Entry ids (`id` in the typed lists) are derived from the pool logical_name
alone, so a repo file, key, artifact dir or build dir keeps the same host
path whichever stack happens to install the entry first.

Input sources:
  --from-json FILE   {"software": [...], "stacks": [...]}
                     (see tests/deploy/fixtures/stacks_seed.json)
  --from-db          SoR over pymysql, read-only transaction. Connection from
                     the environment only (never argv): SOR_DB_NAME (required),
                     SOR_DB_HOST (127.0.0.1), SOR_DB_PORT (3306), SOR_DB_USER,
                     SOR_DB_PASS, SOR_DB_SOCKET (optional unix socket).

Exit: 0 ok, 1 generator error (message on stderr), 2 usage error.
Stdlib only; pymysql is imported lazily for --from-db.
"""
from __future__ import annotations

import argparse
import heapq
import json
import os
import re
import sys
from typing import Any

FAMILIES = ("suse", "debian", "fedora")
ARCHES = ("x86_64", "arm64", "armhf", "riscv64", "i386")
ARTIFACT_ARCHES = ("any",) + ARCHES
ITEM_TYPES = ("pkg", "repo", "artifact", "script", "build", "role")
TYPE_RANK = {t: i for i, t in enumerate(("repo", "pkg", "artifact", "build", "script", "role"))}
TYPE_DEFAULT_ORDER = {t: (r + 1) * 100 for t, r in TYPE_RANK.items()}
SAME = "="
BUILD_SYSTEMS = ("cmake", "make", "autotools", "meson", "custom")
RUN_AS = ("root", "admin")
# version_pin becomes a directory name on hosts: one safe path component
# (no '/', never '.', '..' or a leading '-'). Mirrors ck_prov_software_pin_fmt.
PIN_RE = re.compile(r"^[A-Za-z0-9][A-Za-z0-9._+~-]{0,63}$")
# Git sources are pinned by full commit id only (refs and tags can move).
COMMIT_RE = re.compile(r"^[0-9a-f]{40}([0-9a-f]{24})?$")
# Env names that look like they carry a secret (see _secret_env_name).
SECRET_ENV_TERMS = frozenset((
    "password", "passwd", "pass", "secret", "token", "key", "apikey", "credential", "credentials",
    "jwt", "bearer", "auth", "private", "session", "cookie", "signature", "dsn"))
# Long terms that are unambiguous even inside a run-together name (DBPASSWORD).
SECRET_ENV_SUBSTRINGS = ("password", "passwd", "credential", "apikey")
ENV_NAME_RE = re.compile(r"^[A-Za-z_][A-Za-z0-9_]*$")
SHA256_RE = re.compile(r"^[0-9a-f]{64}$")
FPR_RE = re.compile(r"^[0-9A-F]{40}([0-9A-F]{24})?$")
SLUG_RE = re.compile(r"^[a-z0-9][a-z0-9-]*$")
# C0/C1 controls and the Unicode line/paragraph separators: anything that can
# end a line in a config file the role writes. Mirrored by DB CHECKs.
CONTROL_RE = re.compile(r"[\x00-\x1f\x7f-\x9f  ]")
# Repo URLs are one whitespace-free token on an APT line / in a repo file.
REPO_URL_RE = re.compile(r"[^\s\x00-\x1f\x7f-\x9f  ]+")
# APT suite / component: a plain token, never a leading '-'.
REPO_TOKEN_RE = re.compile(r"[A-Za-z0-9._/][A-Za-z0-9._/-]*")
# Build prefix: absolute, no '.'/'..' components, nothing a shell or find
# -path pattern would interpret. Never '/' itself.
PREFIX_RE = re.compile(r"(/(?!\.\.?(?:/|$))[A-Za-z0-9._+-]+)+")

SCRIPT_KEYS = {"body", "url", "sha256", "interpreter", "run_as", "env", "creates", "check", "version_check"}
BUILD_KEYS = {"source", "system", "configure_args", "prefix", "build", "install", "env",
              "creates", "check", "version_check"}
ARTIFACT_KEYS = {"extract", "creates"}
REPO_FAMILY_KEYS = {"url", "key_url", "fingerprint", "suite", "components"}


class StackGenError(Exception):
    """A definition problem that must stop the render (exit 1)."""


# ---------------------------------------------------------------------------
# Normalisation / structural validation of the definition document
# ---------------------------------------------------------------------------
def _ident(name: str) -> str:
    return re.sub(r"[^a-z0-9]+", "-", name.lower()).strip("-")


def _set_list(value: Any, allowed: tuple[str, ...], what: str) -> list[str] | None:
    """NULL/empty = all. Accepts a list or a MariaDB SET string."""
    if value is None or value == "" or value == []:
        return None
    if isinstance(value, str):
        value = value.split(",")
    vals = list(value)
    bad = [v for v in vals if v not in allowed]
    if bad:
        raise StackGenError(f"{what}: unknown value(s) {bad}")
    return [v for v in allowed if v in vals]


def _name_list(value: Any, where: str, what: str) -> list[str]:
    if value is None:
        return []
    if not isinstance(value, list) or not all(isinstance(v, str) and v for v in value):
        raise StackGenError(f"{where}: {what} must be a list of logical names")
    return sorted(set(value))


def _load_software(raw: Any, seen_ids: dict[str, str]) -> dict:
    if not isinstance(raw, dict):
        raise StackGenError("pool entry must be an object")
    name = raw.get("logical_name")
    if not isinstance(name, str) or not name:
        raise StackGenError("pool entry without logical_name")
    where = f"pool entry {name!r}"
    if CONTROL_RE.search(name):
        raise StackGenError(f"{where}: logical_name contains a control character")
    ident = _ident(name)
    if seen_ids.get(ident) == name:
        raise StackGenError(f"duplicate pool entry {name!r}")
    if not ident or ident in seen_ids:
        raise StackGenError(f"{where}: logical_name does not yield a unique identifier"
                            + (f" (clashes with {seen_ids[ident]!r})" if ident in seen_ids else ""))
    seen_ids[ident] = name
    itype = raw.get("item_type", "pkg")
    if itype not in ITEM_TYPES:
        raise StackGenError(f"{where}: unknown item_type {itype!r}")
    names = raw.get("names") or {}
    for fam in names:
        if fam not in FAMILIES:
            raise StackGenError(f"{where}: unknown family {fam!r} in names")
    for fam in FAMILIES:
        val = names.get(fam)
        if val is None:
            continue
        if itype == "pkg" and (val == SAME or not val):
            raise StackGenError(f"{where}: pkg {fam} name must be a real package name, not {val!r}")
        if itype != "pkg" and val != SAME:
            raise StackGenError(f"{where}: {itype} {fam} name must be '=' or null, not {val!r}")
    role = raw.get("ansible_role")
    if (itype == "role") != (role is not None):
        raise StackGenError(f"{where}: ansible_role is required for role entries and only for them")
    if "repo_ref" in raw:
        raise StackGenError(f"{where}: repo_ref is gone; express it as a requires edge to the repo entry")
    artifacts = raw.get("artifacts") or {}
    if artifacts and itype != "artifact":
        raise StackGenError(f"{where}: per-arch artifacts on a {itype} entry")
    for arch in artifacts:
        if arch not in ARTIFACT_ARCHES:
            raise StackGenError(f"{where}: unknown artifact arch {arch!r}")
    spec = raw.get("spec")
    if spec is not None and not isinstance(spec, dict):
        raise StackGenError(f"{where}: spec must be an object")
    if itype in ("pkg", "role") and spec is not None:
        raise StackGenError(f"{where}: {itype} entries take no spec")
    if itype in ("repo", "script", "build") and spec is None:
        raise StackGenError(f"{where}: {itype} entries require a spec")
    pin = raw.get("version_pin")
    if pin is not None and (not isinstance(pin, str) or not PIN_RE.fullmatch(pin)):
        raise StackGenError(f"{where}: version_pin {pin!r} must be one safe path component "
                            "([A-Za-z0-9][A-Za-z0-9._+~-]*, max 64)")
    return {
        "logical_name": name,
        "ident": ident,
        "item_type": itype,
        "names": {f: names.get(f) for f in FAMILIES},
        "applies_families": _set_list(raw.get("applies_families"), FAMILIES, f"{where} applies_families"),
        "applies_arches": _set_list(raw.get("applies_arches"), ARCHES, f"{where} applies_arches"),
        "ansible_role": role,
        "version_pin": pin,
        "spec": spec,
        "artifacts": artifacts,
        "requires": _name_list(raw.get("requires"), where, "requires"),
        "conflicts_with": _name_list(raw.get("conflicts_with"), where, "conflicts_with"),
    }


def _requirement_cycle(software: dict[str, dict]) -> list[str] | None:
    state: dict[str, int] = {}          # 1 = on the DFS path, 2 = done
    path: list[str] = []

    def visit(n: str) -> list[str] | None:
        state[n] = 1
        path.append(n)
        for r in software[n]["requires"]:
            if state.get(r) == 1:
                cyc = path[path.index(r):] + [r]
                return list(reversed(cyc))   # requirement first, like install order
            if r not in state:
                found = visit(r)
                if found:
                    return found
        path.pop()
        state[n] = 2
        return None

    for n in sorted(software):
        if n not in state:
            found = visit(n)
            if found:
                return found
    return None


def load_definitions(doc: Any) -> dict[str, dict]:
    """Validate the {"software": [...], "stacks": [...]} document.

    Returns {"software": logical_name -> entry, "stacks": slug -> stack,
    "conflicts": sorted list of (a, b) name pairs with a < b}."""
    if (not isinstance(doc, dict) or not isinstance(doc.get("software"), list)
            or not isinstance(doc.get("stacks"), list)):
        raise StackGenError('definitions must be an object with "software" and "stacks" lists')
    software: dict[str, dict] = {}
    seen_ids: dict[str, str] = {}
    for raw in doc["software"]:
        entry = _load_software(raw, seen_ids)
        software[entry["logical_name"]] = entry

    conflicts: set[tuple[str, str]] = set()
    for name, e in software.items():
        for r in e["requires"]:
            if r == name:
                raise StackGenError(f"pool entry {name!r} requires itself")
            if r not in software:
                raise StackGenError(f"pool entry {name!r} requires unknown entry {r!r}")
        for c in e["conflicts_with"]:
            if c == name:
                raise StackGenError(f"pool entry {name!r} conflicts with itself")
            if c not in software:
                raise StackGenError(f"pool entry {name!r} conflicts with unknown entry {c!r}")
            conflicts.add((min(name, c), max(name, c)))
    for a, b in sorted(conflicts):
        if b in software[a]["requires"] or a in software[b]["requires"]:
            raise StackGenError(f"pool entries {a!r} and {b!r} both conflict and require each other")
    cycle = _requirement_cycle(software)
    if cycle:
        raise StackGenError("requirement cycle: " + " -> ".join(repr(n) for n in cycle))

    stacks: dict[str, dict] = {}
    for raw in doc["stacks"]:
        slug = raw.get("slug")
        if not isinstance(slug, str) or not SLUG_RE.fullmatch(slug):
            raise StackGenError(f"invalid stack slug {slug!r}")
        if slug in stacks:
            raise StackGenError(f"duplicate stack slug {slug!r}")
        members = []
        seen: set[str] = set()
        for m in raw.get("members") or []:
            ref = m.get("software") if isinstance(m, dict) else None
            if not isinstance(ref, str) or not ref:
                raise StackGenError(f"stack {slug!r}: member without a software reference")
            if ref not in software:
                raise StackGenError(f"stack {slug!r}: member {ref!r} is not in the software pool")
            if ref in seen:
                raise StackGenError(f"stack {slug!r}: duplicate member {ref!r}")
            seen.add(ref)
            item_order = m.get("item_order")
            members.append({"software": ref,
                            "item_order": int(item_order) if item_order is not None else None,
                            "sort_order": int(m.get("sort_order", 100))})
        stacks[slug] = {
            "slug": slug,
            "locked": bool(raw.get("locked", False)),
            "sort_order": int(raw.get("sort_order", 100)),
            "depends_on": sorted(set(raw.get("depends_on") or [])),
            "members": members,
        }
    for s in stacks.values():
        for dep in s["depends_on"]:
            if dep == s["slug"]:
                raise StackGenError(f"stack {dep!r} depends on itself")
            if dep not in stacks:
                raise StackGenError(f"stack {s['slug']!r} depends on unknown stack {dep!r}")
    return {"software": software, "stacks": stacks, "conflicts": sorted(conflicts)}


# ---------------------------------------------------------------------------
# Stack closure + order
# ---------------------------------------------------------------------------
def _stack_key(s: dict) -> tuple:
    return (0 if s["locked"] else 1, s["sort_order"], s["slug"])


def resolve_order(stacks: dict[str, dict], selected: list[str]) -> list[str]:
    for slug in selected:
        if slug not in stacks:
            raise StackGenError(f"unknown stack slug {slug!r}")
    locked = sorted(s for s, d in stacks.items() if d["locked"])
    closure: set[str] = set()
    todo = list(selected) + locked
    while todo:
        slug = todo.pop()
        if slug in closure:
            continue
        closure.add(slug)
        todo.extend(stacks[slug]["depends_on"])

    deps = {s: set(stacks[s]["depends_on"]) | (set() if stacks[s]["locked"] else set(locked))
            for s in closure}
    indeg = {s: len(deps[s]) for s in closure}
    users: dict[str, list[str]] = {s: [] for s in closure}
    for s, ds in deps.items():
        for d in ds:
            users[d].append(s)
    heap = [(_stack_key(stacks[s]), s) for s in closure if indeg[s] == 0]
    heapq.heapify(heap)
    order: list[str] = []
    while heap:
        _, s = heapq.heappop(heap)
        order.append(s)
        for u in users[s]:
            indeg[u] -= 1
            if indeg[u] == 0:
                heapq.heappush(heap, (_stack_key(stacks[u]), u))
    if len(order) != len(closure):
        raise StackGenError("stack dependency cycle: " + " -> ".join(_find_cycle(deps, closure - set(order))))
    return order


def _find_cycle(deps: dict[str, set[str]], remaining: set[str]) -> list[str]:
    start = min(remaining)
    path: list[str] = []
    pos: dict[str, int] = {}
    node = start
    while node not in pos:
        pos[node] = len(path)
        path.append(node)
        node = min(d for d in deps[node] if d in remaining)
    # path goes dependent -> dependency; report in apply order (dependency first)
    cycle = path[pos[node]:] + [node]
    return list(reversed(cycle))


# ---------------------------------------------------------------------------
# Per-type validation + rendering
# ---------------------------------------------------------------------------
def _member_key(member: dict, entry: dict) -> tuple:
    itype = entry["item_type"]
    pos = member["item_order"] if member["item_order"] is not None else TYPE_DEFAULT_ORDER[itype]
    return (pos, TYPE_RANK[itype], member["sort_order"], entry["logical_name"])


def _stack_members(defs: dict, slug: str) -> list[tuple[dict, dict]]:
    """(member, entry) pairs of one stack in install order."""
    sw = defs["software"]
    pairs = [(m, sw[m["software"]]) for m in defs["stacks"][slug]["members"]]
    return sorted(pairs, key=lambda p: _member_key(*p))


def _check_keys(spec: dict, allowed: set[str], where: str) -> None:
    extra = sorted(set(spec) - allowed)
    if extra:
        raise StackGenError(f"{where}: unknown spec key(s) {extra}")


def _check_sha(value: Any, where: str, what: str) -> str:
    if not isinstance(value, str) or not SHA256_RE.fullmatch(value):
        raise StackGenError(f"{where}: {what} requires a sha256 (64 lowercase hex), got {value!r}")
    return value


def _guard(spec: dict, it: dict, where: str) -> dict:
    creates, check, vcheck = spec.get("creates"), spec.get("check"), spec.get("version_check")
    if not creates and not check:
        raise StackGenError(f"{where}: needs an idempotency guard (creates and/or check)")
    if creates is not None and (not isinstance(creates, str) or not creates.startswith("/")):
        raise StackGenError(f"{where}: creates must be an absolute path")
    if check is not None:
        if (not isinstance(check, list) or not check or not all(isinstance(a, str) for a in check)
                or not check[0]):
            raise StackGenError(f"{where}: check must be an argv list of strings (no shell), "
                                f"e.g. [\"git\", \"-C\", \"/opt/x\", \"rev-parse\", \"HEAD\"]; got {check!r}")
        check = list(check)
    if vcheck is not None:
        if not check or not it["version_pin"]:
            raise StackGenError(f"{where}: version_check needs both check and version_pin")
        try:
            if not isinstance(vcheck, str) or re.compile(vcheck).groups != 1:
                raise StackGenError(f"{where}: version_check needs exactly one capture group")
        except re.error as exc:
            raise StackGenError(f"{where}: version_check is not a valid regex: {exc}") from None
    return {"creates": creates, "check": check, "version_check": vcheck}


def _secret_env_name(name: str) -> bool:
    """HEURISTIC: does an env NAME look like it carries a secret?

    Case-insensitive, on token boundaries (split on _ / -): API_TOKEN,
    db-password and AWS_SECRET_ACCESS_KEY match; MONKEY, BYPASS and
    TOKENIZER_PARALLELISM do not. A few long, unambiguous terms also match
    inside a run-together name (DBPASSWORD). This only catches the obvious
    mistake: specs are stored in the SoR and rendered into Ansible vars, so a
    secret VALUE must never be put in a spec whatever its variable is called."""
    lowered = name.lower()
    if any(t in SECRET_ENV_TERMS for t in re.split(r"[_/-]+", lowered)):
        return True
    return any(s in lowered for s in SECRET_ENV_SUBSTRINGS)


def _env(spec: dict, where: str) -> dict:
    env = spec.get("env") or {}
    if not isinstance(env, dict):
        raise StackGenError(f"{where}: env must be an object")
    for k, v in env.items():
        if not ENV_NAME_RE.fullmatch(k) or not isinstance(v, str):
            raise StackGenError(f"{where}: env {k!r} must be NAME=string")
        if _secret_env_name(k):
            raise StackGenError(f"{where}: env {k!r} looks like a secret; secrets never live in stack specs")
    return dict(sorted(env.items()))


def _check_repo_family(fam: Any, family: str, where: str) -> None:
    """Every field of a repo definition ends up in an APT line, a repo file or
    an argv: no control characters anywhere (they would add config lines),
    URLs are one whitespace-free token, suite/components are plain tokens.
    Applied to every family in the spec, not only the rendered one; mirrored
    by ck_prov_software_repo."""
    if not isinstance(fam, dict):
        raise StackGenError(f"{where}: repo {family} definition must be an object")
    _check_keys(fam, REPO_FAMILY_KEYS, f"{where} ({family})")
    for k in ("url", "key_url", "fingerprint"):
        if not isinstance(fam.get(k), str) or not fam[k]:
            raise StackGenError(f"{where}: repo {family} definition needs {k}")
    for k in ("url", "key_url"):
        if not REPO_URL_RE.fullmatch(fam[k]):
            raise StackGenError(f"{where}: repo {family} {k} must be one URL without whitespace or "
                                f"control characters, got {fam[k]!r}")
    if CONTROL_RE.search(fam["fingerprint"]):
        raise StackGenError(f"{where}: repo {family} fingerprint contains a control character")
    if "suite" in fam and not (isinstance(fam["suite"], str) and REPO_TOKEN_RE.fullmatch(fam["suite"])):
        raise StackGenError(f"{where}: repo {family} suite must be a token "
                            f"([A-Za-z0-9._/-], no leading '-'), got {fam['suite']!r}")
    if "components" in fam:
        comps = fam["components"]
        if (not isinstance(comps, list) or not comps
                or not all(isinstance(c, str) and REPO_TOKEN_RE.fullmatch(c) for c in comps)):
            raise StackGenError(f"{where}: repo {family} components must be a non-empty list of tokens "
                                f"([A-Za-z0-9._/-], no leading '-'), got {comps!r}")


def _render_repo(it: dict, family: str, where: str) -> dict:
    spec = it["spec"]
    _check_keys(spec, set(FAMILIES), where)
    for f in FAMILIES:
        if f in spec:
            _check_repo_family(spec[f], f, where)
    fam = spec.get(family)
    if fam is None:
        raise StackGenError(f"{where}: repo has no {family} definition")
    fpr = re.sub(r"\s+", "", fam["fingerprint"]).upper()
    if not FPR_RE.fullmatch(fpr):
        raise StackGenError(f"{where}: repo fingerprint must be a full 40/64-hex key fingerprint")
    out = {"url": fam["url"], "key_url": fam["key_url"], "fingerprint": fpr, "suite": None, "components": None}
    if family == "debian":
        if "suite" not in fam or "components" not in fam:
            raise StackGenError(f"{where}: debian repo needs suite and components")
        out["suite"], out["components"] = fam["suite"], list(fam["components"])
    return out


def _render_artifact(it: dict, arch: str, where: str) -> dict:
    spec = it["spec"] or {}
    _check_keys(spec, ARTIFACT_KEYS, where)
    art = it["artifacts"].get(arch) or it["artifacts"].get("any")
    if not art:
        raise StackGenError(f"{where}: no artifact for arch {arch} (and no 'any')")
    chosen = arch if arch in it["artifacts"] else "any"
    if not art.get("url"):
        raise StackGenError(f"{where}: artifact for {chosen} has no url")
    return {"url": art["url"], "sha256": _check_sha(art.get("sha256"), where, f"artifact ({chosen})"),
            "artifact_arch": chosen, "extract": bool(spec.get("extract", False)),
            "creates": spec.get("creates")}


def _render_script(it: dict, where: str) -> dict:
    spec = it["spec"]
    _check_keys(spec, SCRIPT_KEYS, where)
    body, url = spec.get("body"), spec.get("url")
    if (body is None) == (url is None):
        raise StackGenError(f"{where}: script needs exactly one of body (inline) or url (remote)")
    if body is not None and (not isinstance(body, str) or not body.strip()):
        raise StackGenError(f"{where}: script body is empty")
    sha = None
    if url is not None:
        if not spec.get("sha256"):
            raise StackGenError(f"{where}: unpinned remote script (url without sha256) is refused")
        sha = _check_sha(spec["sha256"], where, "remote script")
    elif spec.get("sha256") is not None:
        raise StackGenError(f"{where}: sha256 is only meaningful with a remote url")
    interp = spec.get("interpreter", "/bin/sh")
    if not isinstance(interp, str) or not interp.startswith("/") or " " in interp:
        raise StackGenError(f"{where}: interpreter must be an absolute path")
    run_as = spec.get("run_as", "root")
    if run_as not in RUN_AS:
        raise StackGenError(f"{where}: run_as must be one of {RUN_AS}")
    out = {"body": body, "url": url, "sha256": sha, "interpreter": interp, "run_as": run_as,
           "env": _env(spec, where)}
    out.update(_guard(spec, it, where))
    return out


def _render_build(it: dict, where: str) -> dict:
    spec = it["spec"]
    if "build_deps" in spec:
        raise StackGenError(f"{where}: spec.build_deps is refused; build dependencies are requires "
                            "edges to pkg entries in the pool")
    _check_keys(spec, BUILD_KEYS, where)
    src = spec.get("source")
    if not isinstance(src, dict):
        raise StackGenError(f"{where}: build needs a source object")
    pin = it["version_pin"]
    if "git" in src:
        _check_keys(src, {"git", "tag"}, where + " source")
        if not isinstance(src["git"], str) or not src["git"]:
            raise StackGenError(f"{where}: build git source needs a url")
        if not pin or not COMMIT_RE.fullmatch(pin):
            raise StackGenError(f"{where}: git build must be pinned by full commit SHA in version_pin "
                                f"(40/64 lowercase hex), got {pin!r}")
        tag = src.get("tag")
        if tag is not None and (not isinstance(tag, str) or not tag):
            raise StackGenError(f"{where}: source tag (display only) must be a non-empty string")
        source = {"git": src["git"], "commit": pin, "tag": tag, "tarball": None, "sha256": None}
    elif "tarball" in src:
        _check_keys(src, {"tarball", "sha256"}, where + " source")
        if not pin:
            raise StackGenError(f"{where}: build has no pinned version")
        if not isinstance(src["tarball"], str) or not src["tarball"]:
            raise StackGenError(f"{where}: build tarball source needs a url")
        source = {"git": None, "commit": None, "tag": None, "tarball": src["tarball"],
                  "sha256": _check_sha(src.get("sha256"), where, "build tarball")}
    else:
        raise StackGenError(f"{where}: build source must be {{git}} or {{tarball, sha256}}")
    system = spec.get("system")
    if system not in BUILD_SYSTEMS:
        raise StackGenError(f"{where}: build system must be one of {BUILD_SYSTEMS}")
    # No build ever runs a build-controlled command as root: `build` and
    # `install` (like every other system's compile and install) run as the
    # unprivileged admin user, the install staged under $DESTDIR; root only
    # copies <stage><prefix> into the prefix (stack-items stage_install.py).
    # Root-time install logic belongs in a `script` item.
    cmds = {k: spec.get(k) for k in ("build", "install")}
    if system == "custom":
        for k, v in cmds.items():
            if not isinstance(v, str) or not v.strip():
                raise StackGenError(f"{where}: custom build needs a non-empty {k} command "
                                    "(both run unprivileged; install writes under $DESTDIR$PREFIX)")
    elif any(v is not None for v in cmds.values()):
        raise StackGenError(f"{where}: build/install commands are only valid for system custom")
    args = spec.get("configure_args") or []
    if not isinstance(args, list) or not all(isinstance(a, str) for a in args):
        raise StackGenError(f"{where}: configure_args must be a list of strings")
    prefix = spec.get("prefix", "/usr/local")
    if not isinstance(prefix, str) or not PREFIX_RE.fullmatch(prefix):
        raise StackGenError(f"{where}: prefix must be an absolute path of plain components "
                            f"([A-Za-z0-9._+-], no '.'/'..', not '/'), got {prefix!r}")
    env = _env(spec, where)
    for k in ("DESTDIR", "PREFIX"):
        if k in env:
            raise StackGenError(f"{where}: env {k} is set by the stack-items role (prefix / install "
                                "stage); use spec.prefix instead")
    out = {"source": source, "system": system, "configure_args": list(args), "prefix": prefix,
           "build": cmds["build"], "install": cmds["install"], "env": env,
           "build_deps": []}   # filled by render() from the entry's pkg requirements
    out.update(_guard(spec, it, where))
    return out


def _not_applicable(it: dict, family: str, arch: str) -> str | None:
    if it["applies_families"] and family not in it["applies_families"]:
        return f"not for family {family}"
    if it["applies_arches"] and arch not in it["applies_arches"]:
        return f"not for arch {arch}"
    return None


def _origin(inst: dict) -> str:
    if inst["required_by"] is None:
        return f"stack {inst['stack']!r}"
    return f"{inst['via']}, required by {inst['required_by']!r} in stack {inst['stack']!r}"


def render(defs: dict, selected: list[str], family: str, arch: str) -> dict:
    if family not in FAMILIES:
        raise StackGenError(f"unknown family {family!r} (expected one of {FAMILIES})")
    if arch not in ARCHES:
        raise StackGenError(f"unknown arch {arch!r} (expected one of {ARCHES})")
    software = defs["software"]
    order = resolve_order(defs["stacks"], selected)
    occurrences = [(slug, entry) for slug in order for _, entry in _stack_members(defs, slug)]
    # First position of every entry the selected stacks list (requirement checks).
    first_pos: dict[str, tuple[int, str]] = {}
    for pos, (slug, entry) in enumerate(occurrences):
        first_pos.setdefault(entry["logical_name"], (pos, slug))

    lists: dict[str, list] = {"repos": [], "packages": [], "artifacts": [], "builds": [],
                              "scripts": [], "roles": []}
    steps: list[dict] = []
    skipped: list[dict] = []
    shared: list[dict] = []
    implicit: list[dict] = []
    installed: dict[str, dict] = {}     # logical_name -> {"stack", "required_by", "via"}
    in_progress: set[str] = set()

    def add_packages(names: list[str]) -> None:
        new = [n for n in names if n not in lists["packages"]]
        if not new:
            return
        lists["packages"].extend(new)
        if steps and steps[-1]["type"] == "pkg":
            steps[-1]["packages"].extend(new)
        else:
            steps.append({"type": "pkg", "packages": new})

    def add_entry(kind: str, entry: dict) -> None:
        steps.append({"type": kind[:-1], "index": len(lists[kind])})
        lists[kind].append(entry)

    def install(it: dict, slug: str, pos: int, required_by: str | None, via: str | None,
                where: str) -> None:
        name = it["logical_name"]
        if name in in_progress:   # load_definitions rejects cycles; belt and braces
            raise StackGenError(f"{where}: requirement cycle through {name!r}")
        in_progress.add(name)
        for req in sorted(it["requires"], key=lambda r: (TYPE_RANK[software[r]["item_type"]], r)):
            if req in installed:
                continue
            dep = software[req]
            rwhere = f"{where} requirement {req!r}"
            why = _not_applicable(dep, family, arch)
            if why:
                raise StackGenError(f"{rwhere}: {why}; a requirement must apply to {family}/{arch}")
            listed = first_pos.get(req)
            if listed is not None and listed[0] > pos and dep["item_type"] != "pkg":
                raise StackGenError(f"{rwhere}: {dep['item_type']} is installed later in run order "
                                    f"(stack {listed[1]!r}); it must come before {name!r} — fix "
                                    "item_order or the stack dependencies")
            if listed is None:
                install(dep, slug, pos, name, "implicit", f"stack {slug!r} item {req!r} (required by {name!r})")
                implicit.append({"item": req, "required_by": name, "stack": slug})
            else:
                # Listed later by a selected stack: a pkg is pulled forward (its
                # later occurrence is then reported as shared).
                install(dep, slug, pos, name, "pulled forward",
                        f"stack {slug!r} item {req!r} (pulled forward for {name!r})")
        in_progress.discard(name)

        itype = it["item_type"]
        fam_name = it["names"][family]
        if fam_name is None:
            kind = "package" if itype == "pkg" else itype
            raise StackGenError(f"{where}: unmapped {kind} for family {family} (null {family} name)")
        base = {"name": name, "id": it["ident"], "stack": slug, "version": it["version_pin"]}
        if itype == "pkg":
            add_packages([fam_name])
        elif itype == "repo":
            add_entry("repos", {**base, **_render_repo(it, family, where)})
        elif itype == "artifact":
            if not it["version_pin"]:
                raise StackGenError(f"{where}: artifact needs a version_pin")
            add_entry("artifacts", {**base, **_render_artifact(it, arch, where)})
        elif itype == "script":
            add_entry("scripts", {**base, **_render_script(it, where)})
        elif itype == "build":
            entry = {**base, **_render_build(it, where)}
            # The pkg requirements, all installed by now, in install order.
            entry["build_deps"] = sorted({software[r]["names"][family] for r in it["requires"]
                                          if software[r]["item_type"] == "pkg"},
                                         key=lists["packages"].index)
            add_entry("builds", entry)
        else:  # role
            role = it["ansible_role"]
            if role not in lists["roles"]:
                add_entry("roles", role)
        installed[name] = {"stack": slug, "required_by": required_by, "via": via}

    for pos, (slug, it) in enumerate(occurrences):
        name = it["logical_name"]
        why = _not_applicable(it, family, arch)
        if why:
            skipped.append({"stack": slug, "item": name, "reason": why})
            continue
        if name in installed:
            shared.append({"stack": slug, "item": name, "installed_in": installed[name]["stack"]})
            continue
        install(it, slug, pos, None, None, f"stack {slug!r} item {name!r}")

    for a, b in defs["conflicts"]:
        if a in installed and b in installed:
            raise StackGenError(f"conflicting entries {a!r} ({_origin(installed[a])}) and {b!r} "
                                f"({_origin(installed[b])}) cannot be installed together")

    out = {
        "solari_stack_family": family,
        "solari_stack_arch": arch,
        "solari_stack_order": order,
        "solari_stack_steps": steps,
        "solari_stack_skipped": skipped,
        "solari_stack_shared": shared,
        "solari_stack_implicit": implicit,
    }
    for kind, values in lists.items():
        out[f"solari_stack_{kind}"] = values
    return out


def dumps(rendered: dict) -> str:
    return json.dumps(rendered, sort_keys=True, indent=2, ensure_ascii=False) + "\n"


# ---------------------------------------------------------------------------
# Sources
# ---------------------------------------------------------------------------
def load_json_file(path: str) -> dict[str, dict]:
    try:
        with open(path, encoding="utf-8") as fh:
            doc = json.load(fh)
    except (OSError, ValueError) as exc:
        raise StackGenError(f"cannot read {path}: {exc}") from None
    return load_definitions(doc)


def fetch_db_document(conn) -> dict:
    """Read live (non-deleted) rows into the --from-json document shape.

    A live membership or requirement that points at a soft-deleted pool entry
    is an error (the stack would silently lose software); a conflict pair with
    a soft-deleted side is moot and ignored."""
    cur = conn.cursor()
    cur.execute(
        "SELECT id, logical_name, item_type, name_suse, name_debian, name_fedora,"
        " CAST(applies_families AS CHAR), CAST(applies_arches AS CHAR), ansible_role,"
        " version_pin, spec, deleted_at IS NULL FROM prov_software")
    live: dict[int, dict] = {}
    all_names: dict[int, str] = {}
    for (sid, name, itype, ns, nd, nf, fams, arches, role, pin, spec, is_live) in cur.fetchall():
        all_names[sid] = name
        if not is_live:
            continue
        try:
            spec_obj = json.loads(spec) if spec is not None else None
        except ValueError as exc:
            raise StackGenError(f"pool entry {name!r}: spec is not JSON: {exc}") from None
        live[sid] = {"logical_name": name, "item_type": itype,
                     "names": {"suse": ns, "debian": nd, "fedora": nf},
                     "applies_families": fams or None, "applies_arches": arches or None,
                     "ansible_role": role, "version_pin": pin, "spec": spec_obj,
                     "artifacts": {}, "requires": [], "conflicts_with": []}
    cur.execute("SELECT software_id, arch, url, sha256 FROM prov_software_artifacts WHERE deleted_at IS NULL")
    for sid, arch, url, sha in cur.fetchall():
        if sid in live:
            live[sid]["artifacts"][arch] = {"url": url, "sha256": sha}
    cur.execute("SELECT software_id, requires_software_id FROM prov_software_requires WHERE deleted_at IS NULL")
    for sid, rid in cur.fetchall():
        if sid not in live:
            continue
        if rid not in live:
            raise StackGenError(f"pool entry {live[sid]['logical_name']!r} requires "
                                f"{all_names.get(rid, rid)!r}, which is soft-deleted")
        live[sid]["requires"].append(live[rid]["logical_name"])
    cur.execute("SELECT software_lo_id, software_hi_id FROM prov_software_conflicts WHERE deleted_at IS NULL")
    for lo, hi in cur.fetchall():
        if lo in live and hi in live:
            live[lo]["conflicts_with"].append(live[hi]["logical_name"])

    cur.execute("SELECT id, slug, locked, sort_order FROM prov_stacks WHERE deleted_at IS NULL")
    by_id = {r[0]: {"slug": r[1], "locked": bool(r[2]), "sort_order": int(r[3]),
                    "depends_on": [], "members": []} for r in cur.fetchall()}
    cur.execute("SELECT stack_id, depends_on_stack_id FROM prov_stack_deps WHERE deleted_at IS NULL")
    for sid, did in cur.fetchall():
        if sid in by_id and did in by_id:
            by_id[sid]["depends_on"].append(by_id[did]["slug"])
    cur.execute("SELECT stack_id, software_id, item_order, sort_order FROM prov_stack_members"
                " WHERE deleted_at IS NULL")
    for stid, sid, item_order, sort_order in cur.fetchall():
        if stid not in by_id:
            continue
        if sid not in live:
            raise StackGenError(f"stack {by_id[stid]['slug']!r} references pool entry "
                                f"{all_names.get(sid, sid)!r}, which is soft-deleted")
        by_id[stid]["members"].append({"software": live[sid]["logical_name"],
                                       "item_order": item_order, "sort_order": sort_order})
    return {"software": sorted(live.values(), key=lambda e: e["logical_name"]),
            "stacks": sorted(by_id.values(), key=lambda s: s["slug"])}


def load_db_from_env() -> dict[str, dict]:
    name = os.environ.get("SOR_DB_NAME")
    if not name:
        raise StackGenError("--from-db needs SOR_DB_NAME in the environment")
    try:
        import pymysql  # noqa: PLC0415 — optional dependency, DB mode only
    except ImportError:
        raise StackGenError("--from-db needs the pymysql module") from None
    kwargs: dict[str, Any] = {"database": name, "charset": "utf8mb4",
                              "user": os.environ.get("SOR_DB_USER", ""),
                              "password": os.environ.get("SOR_DB_PASS", "")}
    if os.environ.get("SOR_DB_SOCKET"):
        kwargs["unix_socket"] = os.environ["SOR_DB_SOCKET"]
    else:
        kwargs["host"] = os.environ.get("SOR_DB_HOST", "127.0.0.1")
        kwargs["port"] = int(os.environ.get("SOR_DB_PORT", "3306"))
    try:
        conn = pymysql.connect(**kwargs)
    except pymysql.MySQLError as exc:
        raise StackGenError(f"cannot connect to SoR database {name!r}: {exc}") from None
    try:
        with conn.cursor() as cur:
            cur.execute("START TRANSACTION READ ONLY")
        doc = fetch_db_document(conn)
        conn.rollback()
    except pymysql.MySQLError as exc:
        raise StackGenError(f"SoR read failed: {exc}") from None
    finally:
        conn.close()
    return load_definitions(doc)


# ---------------------------------------------------------------------------
# CLI
# ---------------------------------------------------------------------------
def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description="Render SolariNet provisioning stacks to Ansible vars (JSON).")
    src = ap.add_mutually_exclusive_group(required=True)
    src.add_argument("--from-json", metavar="FILE", help="read definitions from a JSON document")
    src.add_argument("--from-db", action="store_true", help="read definitions from the SoR (env SOR_DB_*)")
    ap.add_argument("--family", required=True, choices=FAMILIES)
    ap.add_argument("--arch", required=True, choices=ARCHES)
    ap.add_argument("--stack", action="append", default=[], metavar="SLUG",
                    help="stack to include (repeatable); locked stacks are always included")
    ap.add_argument("--output", metavar="FILE", help="write vars here instead of stdout")
    args = ap.parse_args(argv)
    try:
        defs = load_db_from_env() if args.from_db else load_json_file(args.from_json)
        text = dumps(render(defs, args.stack, args.family, args.arch))
    except StackGenError as exc:
        print(f"stackgen: error: {exc}", file=sys.stderr)
        return 1
    if args.output:
        with open(args.output, "w", encoding="utf-8") as fh:
            fh.write(text)
    else:
        sys.stdout.write(text)
    return 0


if __name__ == "__main__":
    sys.exit(main())
