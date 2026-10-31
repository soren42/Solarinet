#!/usr/bin/env python3
"""stackgen — render SolariNet provisioning stacks into Ansible vars.

Reads stack definitions (the SoR prov_stacks / prov_stack_items /
prov_stack_artifacts / prov_stack_deps tables from migration 029, or the same
shape as JSON) and renders, for ONE target family + arch, the deterministic
JSON vars consumed by the `stack-items` Ansible role (via common-stack):

  solari_stack_family / solari_stack_arch   target the vars were rendered for
  solari_stack_order       dependency-closed stack slugs, locked (Core) first
  solari_stack_repos       third-party repos      (item_type repo)
  solari_stack_packages    distro package names   (pkg + resolved build deps)
  solari_stack_artifacts   pinned downloads       (item_type artifact)
  solari_stack_builds      source builds          (item_type build)
  solari_stack_scripts     install scripts        (item_type script)
  solari_stack_roles       Ansible roles          (item_type role)
  solari_stack_steps       the execution sequence over the lists above
  solari_stack_skipped     items not applicable to this family/arch

Rules this generator owns (the schema cannot express them):
  * closure = requested stacks + every locked stack + transitive depends_on;
  * order = topological; every unlocked stack implicitly depends on every
    locked stack; ties broken by (locked first, sort_order, slug);
  * within a stack: item_order when set, else repo 100 -> pkg 200 ->
    artifact 300 -> build 400 -> script 500 -> role 600;
  * HARD errors: unknown slug, dependency cycle, unmapped package (NULL family
    name on an applicable item), artifact without a sha256 for the arch,
    unpinned remote script, build without a pinned ref / tarball sha256,
    unresolvable build dependency, pkg whose repo_ref is not applied first,
    script/build without an idempotency guard, secret-looking env names.

Input sources:
  --from-json FILE   {"stacks": [...]} (see tests/deploy/fixtures/stacks_seed.json)
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
# Branch-like refs are not pins: they move. Tags and commit ids are accepted.
UNPINNED_REFS = frozenset({"head", "master", "main", "trunk", "develop", "dev", "latest", "stable"})
SECRET_ENV_RE = re.compile(r"PASS|SECRET|TOKEN|KEY|CREDENTIAL|AUTH|PRIVATE", re.IGNORECASE)
ENV_NAME_RE = re.compile(r"^[A-Za-z_][A-Za-z0-9_]*$")
SHA256_RE = re.compile(r"^[0-9a-f]{64}$")
FPR_RE = re.compile(r"^[0-9A-F]{40}([0-9A-F]{24})?$")
SLUG_RE = re.compile(r"^[a-z0-9][a-z0-9-]*$")

SCRIPT_KEYS = {"body", "url", "sha256", "interpreter", "run_as", "env", "creates", "check", "version_check"}
BUILD_KEYS = {"source", "system", "configure_args", "build_deps", "prefix", "commands", "env",
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


def load_definitions(doc: Any) -> dict[str, dict]:
    """Validate the {"stacks": [...]} document; return slug -> stack."""
    if not isinstance(doc, dict) or not isinstance(doc.get("stacks"), list):
        raise StackGenError('definitions must be an object with a "stacks" list')
    stacks: dict[str, dict] = {}
    for raw in doc["stacks"]:
        slug = raw.get("slug")
        if not isinstance(slug, str) or not SLUG_RE.match(slug):
            raise StackGenError(f"invalid stack slug {slug!r}")
        if slug in stacks:
            raise StackGenError(f"duplicate stack slug {slug!r}")
        items = []
        seen_items: set[str] = set()
        seen_ids: set[str] = set()
        for it in raw.get("items") or []:
            name = it.get("logical_name")
            where = f"stack {slug!r} item {name!r}"
            if not isinstance(name, str) or not name:
                raise StackGenError(f"stack {slug!r}: item without logical_name")
            if name in seen_items:
                raise StackGenError(f"{where}: duplicate logical_name")
            ident = _ident(name)
            if not ident or ident in seen_ids:
                raise StackGenError(f"{where}: logical_name does not yield a unique identifier")
            seen_items.add(name)
            seen_ids.add(ident)
            itype = it.get("item_type", "pkg")
            if itype not in ITEM_TYPES:
                raise StackGenError(f"{where}: unknown item_type {itype!r}")
            names = it.get("names") or {}
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
            role = it.get("ansible_role")
            if (itype == "role") != (role is not None):
                raise StackGenError(f"{where}: ansible_role is required for role items and only for them")
            if it.get("repo_ref") is not None and itype != "pkg":
                raise StackGenError(f"{where}: repo_ref is only valid on pkg items")
            artifacts = it.get("artifacts") or {}
            if artifacts and itype != "artifact":
                raise StackGenError(f"{where}: per-arch artifacts on a {itype} item")
            for arch in artifacts:
                if arch not in ARTIFACT_ARCHES:
                    raise StackGenError(f"{where}: unknown artifact arch {arch!r}")
            spec = it.get("spec")
            if spec is not None and not isinstance(spec, dict):
                raise StackGenError(f"{where}: spec must be an object")
            if itype in ("pkg", "role") and spec is not None:
                raise StackGenError(f"{where}: {itype} items take no spec")
            if itype in ("repo", "script", "build") and spec is None:
                raise StackGenError(f"{where}: {itype} items require a spec")
            item_order = it.get("item_order")
            items.append({
                "logical_name": name,
                "ident": ident,
                "item_type": itype,
                "names": {f: names.get(f) for f in FAMILIES},
                "applies_families": _set_list(it.get("applies_families"), FAMILIES, f"{where} applies_families"),
                "applies_arches": _set_list(it.get("applies_arches"), ARCHES, f"{where} applies_arches"),
                "ansible_role": role,
                "repo_ref": it.get("repo_ref"),
                "version_pin": it.get("version_pin"),
                "spec": spec,
                "artifacts": artifacts,
                "item_order": int(item_order) if item_order is not None else None,
                "sort_order": int(it.get("sort_order", 100)),
            })
        stacks[slug] = {
            "slug": slug,
            "locked": bool(raw.get("locked", False)),
            "sort_order": int(raw.get("sort_order", 100)),
            "depends_on": sorted(set(raw.get("depends_on") or [])),
            "items": items,
        }
    for s in stacks.values():
        for dep in s["depends_on"]:
            if dep == s["slug"]:
                raise StackGenError(f"stack {dep!r} depends on itself")
            if dep not in stacks:
                raise StackGenError(f"stack {s['slug']!r} depends on unknown stack {dep!r}")
    return stacks


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
def _item_key(it: dict) -> tuple:
    pos = it["item_order"] if it["item_order"] is not None else TYPE_DEFAULT_ORDER[it["item_type"]]
    return (pos, TYPE_RANK[it["item_type"]], it["sort_order"], it["logical_name"])


def _check_keys(spec: dict, allowed: set[str], where: str) -> None:
    extra = sorted(set(spec) - allowed)
    if extra:
        raise StackGenError(f"{where}: unknown spec key(s) {extra}")


def _check_sha(value: Any, where: str, what: str) -> str:
    if not isinstance(value, str) or not SHA256_RE.match(value):
        raise StackGenError(f"{where}: {what} requires a sha256 (64 lowercase hex), got {value!r}")
    return value


def _guard(spec: dict, it: dict, where: str) -> dict:
    creates, check, vcheck = spec.get("creates"), spec.get("check"), spec.get("version_check")
    if not creates and not check:
        raise StackGenError(f"{where}: needs an idempotency guard (creates and/or check)")
    if creates is not None and (not isinstance(creates, str) or not creates.startswith("/")):
        raise StackGenError(f"{where}: creates must be an absolute path")
    if check is not None and (not isinstance(check, str) or not check.strip()):
        raise StackGenError(f"{where}: check must be a non-empty command")
    if vcheck is not None:
        if not check or not it["version_pin"]:
            raise StackGenError(f"{where}: version_check needs both check and version_pin")
        try:
            if not isinstance(vcheck, str) or re.compile(vcheck).groups != 1:
                raise StackGenError(f"{where}: version_check needs exactly one capture group")
        except re.error as exc:
            raise StackGenError(f"{where}: version_check is not a valid regex: {exc}") from None
    return {"creates": creates, "check": check, "version_check": vcheck}


def _env(spec: dict, where: str) -> dict:
    env = spec.get("env") or {}
    if not isinstance(env, dict):
        raise StackGenError(f"{where}: env must be an object")
    for k, v in env.items():
        if not ENV_NAME_RE.match(k) or not isinstance(v, str):
            raise StackGenError(f"{where}: env {k!r} must be NAME=string")
        if SECRET_ENV_RE.search(k):
            raise StackGenError(f"{where}: env {k!r} looks like a secret; secrets never live in stack specs")
    return dict(sorted(env.items()))


def _render_repo(it: dict, family: str, where: str) -> dict:
    spec = it["spec"]
    _check_keys(spec, set(FAMILIES), where)
    fam = spec.get(family)
    if not isinstance(fam, dict):
        raise StackGenError(f"{where}: repo has no {family} definition")
    _check_keys(fam, REPO_FAMILY_KEYS, f"{where} ({family})")
    for k in ("url", "key_url", "fingerprint"):
        if not isinstance(fam.get(k), str) or not fam[k]:
            raise StackGenError(f"{where}: repo {family} definition needs {k}")
    fpr = re.sub(r"\s+", "", fam["fingerprint"]).upper()
    if not FPR_RE.match(fpr):
        raise StackGenError(f"{where}: repo fingerprint must be a full 40/64-hex key fingerprint")
    out = {"url": fam["url"], "key_url": fam["key_url"], "fingerprint": fpr, "suite": None, "components": None}
    if family == "debian":
        comps = fam.get("components")
        if not isinstance(fam.get("suite"), str) or not fam["suite"] or not isinstance(comps, list) or not comps:
            raise StackGenError(f"{where}: debian repo needs suite and components")
        out["suite"], out["components"] = fam["suite"], [str(c) for c in comps]
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
    _check_keys(spec, BUILD_KEYS, where)
    src = spec.get("source")
    if not isinstance(src, dict):
        raise StackGenError(f"{where}: build needs a source object")
    pin = it["version_pin"]
    if "git" in src:
        _check_keys(src, {"git"}, where + " source")
        if not pin or str(pin).strip().lower() in UNPINNED_REFS:
            raise StackGenError(f"{where}: build has no pinned ref (version_pin {pin!r} is not a tag/commit)")
        source = {"git": src["git"], "ref": pin, "tarball": None, "sha256": None}
    elif "tarball" in src:
        _check_keys(src, {"tarball", "sha256"}, where + " source")
        if not pin:
            raise StackGenError(f"{where}: build has no pinned version")
        source = {"git": None, "ref": None, "tarball": src["tarball"],
                  "sha256": _check_sha(src.get("sha256"), where, "build tarball")}
    else:
        raise StackGenError(f"{where}: build source must be {{git}} or {{tarball, sha256}}")
    system = spec.get("system")
    if system not in BUILD_SYSTEMS:
        raise StackGenError(f"{where}: build system must be one of {BUILD_SYSTEMS}")
    commands = spec.get("commands")
    if system == "custom" and (not isinstance(commands, str) or not commands.strip()):
        raise StackGenError(f"{where}: custom build needs commands")
    if system != "custom" and commands is not None:
        raise StackGenError(f"{where}: commands are only valid for system custom")
    args = spec.get("configure_args") or []
    deps = spec.get("build_deps") or []
    if not all(isinstance(a, str) for a in args) or not all(isinstance(d, str) for d in deps):
        raise StackGenError(f"{where}: configure_args and build_deps must be lists of strings")
    prefix = spec.get("prefix", "/usr/local")
    if not isinstance(prefix, str) or not prefix.startswith("/"):
        raise StackGenError(f"{where}: prefix must be an absolute path")
    out = {"source": source, "system": system, "configure_args": list(args), "prefix": prefix,
           "commands": commands, "env": _env(spec, where), "build_deps": list(deps)}
    out.update(_guard(spec, it, where))
    return out


def _pkg_index(stacks: dict[str, dict], order: list[str]) -> dict[str, dict]:
    """logical name -> pkg item; closure stacks (in order) win, then the rest by slug."""
    index: dict[str, dict] = {}
    for slug in order + sorted(set(stacks) - set(order)):
        for it in sorted(stacks[slug]["items"], key=_item_key):
            if it["item_type"] == "pkg":
                index.setdefault(it["logical_name"], {**it, "stack": slug})
    return index


def render(stacks: dict[str, dict], selected: list[str], family: str, arch: str) -> dict:
    if family not in FAMILIES:
        raise StackGenError(f"unknown family {family!r} (expected one of {FAMILIES})")
    if arch not in ARCHES:
        raise StackGenError(f"unknown arch {arch!r} (expected one of {ARCHES})")
    order = resolve_order(stacks, selected)
    pkg_index = _pkg_index(stacks, order)
    lists: dict[str, list] = {"repos": [], "packages": [], "artifacts": [], "builds": [],
                              "scripts": [], "roles": []}
    steps: list[dict] = []
    skipped: list[dict] = []
    applied_repos: set[str] = set()

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

    for slug in order:
        for it in sorted(stacks[slug]["items"], key=_item_key):
            where = f"stack {slug!r} item {it['logical_name']!r}"
            itype = it["item_type"]
            if it["applies_families"] and family not in it["applies_families"]:
                skipped.append({"stack": slug, "item": it["logical_name"], "reason": f"not for family {family}"})
                continue
            if it["applies_arches"] and arch not in it["applies_arches"]:
                skipped.append({"stack": slug, "item": it["logical_name"], "reason": f"not for arch {arch}"})
                continue
            fam_name = it["names"][family]
            if fam_name is None:
                kind = "package" if itype == "pkg" else itype
                raise StackGenError(f"{where}: unmapped {kind} for family {family} (null {family} name)")
            base = {"name": it["logical_name"], "id": f"{slug}--{it['ident']}", "stack": slug,
                    "version": it["version_pin"]}
            if itype == "pkg":
                ref = it["repo_ref"]
                if ref is not None and ref not in applied_repos:
                    raise StackGenError(f"{where}: repo_ref {ref!r} is not a repo applied before it")
                add_packages([fam_name])
            elif itype == "repo":
                add_entry("repos", {**base, **_render_repo(it, family, where)})
                applied_repos.add(it["logical_name"])
            elif itype == "artifact":
                if not it["version_pin"]:
                    raise StackGenError(f"{where}: artifact needs a version_pin")
                add_entry("artifacts", {**base, **_render_artifact(it, arch, where)})
            elif itype == "script":
                add_entry("scripts", {**base, **_render_script(it, where)})
            elif itype == "build":
                entry = {**base, **_render_build(it, where)}
                resolved = []
                for dep in entry["build_deps"]:
                    pkg = pkg_index.get(dep)
                    if pkg is None:
                        raise StackGenError(f"{where}: build dep {dep!r} is not a pkg item in any stack")
                    real = pkg["names"][family]
                    if real is None:
                        raise StackGenError(f"{where}: build dep {dep!r} unmapped for family {family}")
                    resolved.append(real)
                entry["build_deps"] = resolved
                add_packages(resolved)
                add_entry("builds", entry)
            else:  # role
                role = it["ansible_role"]
                if role not in lists["roles"]:
                    add_entry("roles", role)

    out = {
        "solari_stack_family": family,
        "solari_stack_arch": arch,
        "solari_stack_order": order,
        "solari_stack_steps": steps,
        "solari_stack_skipped": skipped,
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
    """Read live (non-deleted) rows into the --from-json document shape."""
    cur = conn.cursor()
    cur.execute("SELECT id, slug, locked, sort_order FROM prov_stacks WHERE deleted_at IS NULL")
    by_id = {r[0]: {"slug": r[1], "locked": bool(r[2]), "sort_order": int(r[3]),
                    "depends_on": [], "items": []} for r in cur.fetchall()}
    cur.execute("SELECT stack_id, depends_on_stack_id FROM prov_stack_deps WHERE deleted_at IS NULL")
    for sid, did in cur.fetchall():
        if sid in by_id and did in by_id:
            by_id[sid]["depends_on"].append(by_id[did]["slug"])
    cur.execute(
        "SELECT id, stack_id, logical_name, item_type, name_suse, name_debian, name_fedora,"
        " CAST(applies_families AS CHAR), CAST(applies_arches AS CHAR), ansible_role, repo_ref,"
        " version_pin, spec, item_order, sort_order FROM prov_stack_items WHERE deleted_at IS NULL")
    items: dict[int, dict] = {}
    for (iid, sid, name, itype, ns, nd, nf, fams, arches, role, repo_ref,
         pin, spec, item_order, sort_order) in cur.fetchall():
        if sid not in by_id:
            continue
        try:
            spec_obj = json.loads(spec) if spec is not None else None
        except ValueError as exc:
            raise StackGenError(f"item {name!r}: spec is not JSON: {exc}") from None
        item = {"logical_name": name, "item_type": itype,
                "names": {"suse": ns, "debian": nd, "fedora": nf},
                "applies_families": fams or None, "applies_arches": arches or None,
                "ansible_role": role, "repo_ref": repo_ref, "version_pin": pin, "spec": spec_obj,
                "item_order": item_order, "sort_order": sort_order, "artifacts": {}}
        items[iid] = item
        by_id[sid]["items"].append(item)
    cur.execute("SELECT item_id, arch, url, sha256 FROM prov_stack_artifacts WHERE deleted_at IS NULL")
    for iid, arch, url, sha in cur.fetchall():
        if iid in items:
            items[iid]["artifacts"][arch] = {"url": url, "sha256": sha}
    return {"stacks": sorted(by_id.values(), key=lambda s: s["slug"])}


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
        stacks = load_db_from_env() if args.from_db else load_json_file(args.from_json)
        text = dumps(render(stacks, args.stack, args.family, args.arch))
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
