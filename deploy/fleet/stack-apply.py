#!/usr/bin/env python3
"""stack-apply — STACK_APPLY helper: apply SoR stacks to a running machine.

Invoked by solariServer's queue consumer for the operator-gated STACK_APPLY
verb with a literal argv (no shell):

  deploy/fleet/stack-apply.py --entity <id> --stacks <a,b> [--dry-run] --op <principal>

Flow:
  1. validate the arguments (provlib, the same rules as the C bridge); load the
     live entity (name must be a hostname label) and confirm every stack slug
     is a live prov_stacks row; refuse — without opening a run — while another
     apply run for the entity is queued/running (serialised by a named lock);
  2. probe the target's facts with ansible on the provisioning host
     (`<PROV_ROOT>/ansible`): ansible_os_family / ansible_architecture, mapped
     with the SAME tables as roles/stack-items/defaults/main.yml (unmapped →
     refuse);
  3. render the vars with `stackgen.py --from-db`, open the prov_runs row (mode
     apply, dry_run flag) directly as running, upload the vars to
     `<PROV_ROOT>/runs/apply-<run id>.stack.json` (0644);
  4. `ansible-playbook ... playbooks/site.yml --tags stack -e @<vars> --check
     --diff` first (failure → failed); with --dry-run the run then succeeds
     ("dry-run: check+diff only"); otherwise the same command runs for real →
     succeeded / failed.

Exit: 0 succeeded, 1 refusal/failure, 2 usage error. Playbook output is printed
to stdout (the C side redirects it to run/stack-apply-<entity>.log).
"""
from __future__ import annotations

import argparse
import json
import os
import re
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

import provlib  # noqa: E402  (path set above)
from provlib import ProvError  # noqa: E402

HOST_LABEL_RE = re.compile(r"^[a-z0-9]([a-z0-9-]{0,61}[a-z0-9])?$")
DOMAIN_RE = re.compile(r"^[a-z0-9]([a-z0-9-]{0,61}[a-z0-9])?(\.[a-z0-9]([a-z0-9-]{0,61}[a-z0-9])?)+$")
DEFAULT_DOMAIN = "akoria.net"

# Mirror of deploy/fleet/ansible/roles/stack-items/defaults/main.yml
# (solari_stack_family_map / solari_stack_arch_map). tests/deploy/
# test_prov_helpers.py fails if the two drift apart.
FAMILY_MAP = {"Debian": "debian", "Suse": "suse", "RedHat": "fedora"}
ARCH_MAP = {"x86_64": "x86_64", "aarch64": "arm64", "armv7l": "armhf", "armv6l": "armhf",
            "riscv64": "riscv64", "i386": "i386", "i686": "i386"}

STACKGEN_TIMEOUT = 300
FACT_TIMEOUT = 300
CHECK_TIMEOUT = 3600
APPLY_TIMEOUT = 7200


def log(msg: str) -> None:
    print(f"[stack-apply] {msg}", flush=True)


# ---------------------------------------------------------------------------
# SoR reads
# ---------------------------------------------------------------------------
def load_entity(cur, entity_id: int) -> dict:
    cur.execute("SELECT id, name FROM entities WHERE id=%s AND deleted_at IS NULL", (entity_id,))
    row = cur.fetchone()
    if not row:
        raise ProvError(f"entity {entity_id} does not exist or is deleted")
    if not HOST_LABEL_RE.match(row["name"] or ""):
        raise ProvError(f"entity {entity_id} name {row['name']!r} is not a valid hostname label")
    return row


def check_stacks(cur, slugs: list[str]) -> None:
    marks = ",".join(["%s"] * len(slugs))
    cur.execute(f"SELECT slug FROM prov_stacks WHERE deleted_at IS NULL AND slug IN ({marks})",
                tuple(slugs))
    live = {r["slug"] for r in cur.fetchall()}
    missing = [s for s in slugs if s not in live]
    if missing:
        raise ProvError(f"unknown or deleted stack(s): {', '.join(missing)}")


def active_apply_run(cur, entity_id: int) -> int | None:
    cur.execute("SELECT id FROM prov_runs WHERE entity_id=%s AND mode='apply'"
                " AND status IN ('queued','running') ORDER BY id LIMIT 1", (entity_id,))
    row = cur.fetchone()
    return int(row["id"]) if row else None


# ---------------------------------------------------------------------------
# Facts
# ---------------------------------------------------------------------------
def _find_facts(obj) -> dict | None:
    """Depth-first search for an ansible_facts dict carrying the two keys."""
    if isinstance(obj, dict):
        facts = obj.get("ansible_facts")
        if isinstance(facts, dict) and "ansible_os_family" in facts:
            return facts
        for v in obj.values():
            found = _find_facts(v)
            if found is not None:
                return found
    elif isinstance(obj, list):
        for v in obj:
            found = _find_facts(v)
            if found is not None:
                return found
    return None


def parse_facts(text: str) -> tuple[str, str]:
    """Extract (os_family, architecture) from ansible ad-hoc setup output.

    Accepts the built-in `host | SUCCESS => {...}` form (possibly surrounded
    by [WARNING] lines) and, defensively, a whole-document JSON callback dump.
    """
    facts = None
    try:
        facts = _find_facts(json.loads(text))
    except ValueError:
        pass
    if facts is None:
        dec = json.JSONDecoder()
        for m in re.finditer(r"=>\s*\{", text):
            try:
                obj, _ = dec.raw_decode(text, m.end() - 1)
            except ValueError:
                continue
            facts = _find_facts(obj if "ansible_facts" in obj else {"ansible_facts": obj})
            if facts is not None:
                break
    if facts is None:
        raise ProvError("fact probe returned no ansible_os_family/ansible_architecture")
    fam, arch = facts.get("ansible_os_family"), facts.get("ansible_architecture")
    if not isinstance(fam, str) or not isinstance(arch, str):
        raise ProvError("fact probe returned malformed facts")
    return fam, arch


def map_facts(os_family: str, architecture: str) -> tuple[str, str]:
    fam = FAMILY_MAP.get(os_family)
    arch = ARCH_MAP.get(architecture)
    if fam is None:
        raise ProvError(f"unsupported OS family {os_family!r} (no stack-items mapping)")
    if arch is None:
        raise ProvError(f"unsupported architecture {architecture!r} (no stack-items mapping)")
    return fam, arch


def _out(proc: subprocess.CompletedProcess) -> str:
    return (proc.stdout or b"").decode("utf-8", "replace")


def _err_tail(proc: subprocess.CompletedProcess) -> str:
    lines = ((proc.stderr or b"").decode("utf-8", "replace").strip().splitlines()
             or _out(proc).strip().splitlines())
    return lines[-1][:160] if lines else f"exit {proc.returncode}"


def _login(user: str | None) -> list[str]:
    return ["-u", user] if user else []


def probe_facts(ansible_dir: str, fqdn: str, *, user: str | None = None,
                extra_env: tuple[str, ...] = (), on_spawn=None) -> tuple[str, str]:
    # ANSIBLE_LOAD_CALLBACK_PLUGINS=0 + result_format=json pin ad-hoc output to
    # the built-in `host | SUCCESS => {...}` form whatever ansible.cfg says
    # (it sets callback_result_format=yaml; the `json` callback left
    # ansible-core for ansible.posix, so it is not assumed). user/extra_env
    # (KEY=VALUE) let prov-finish.py log in as the fresh host's admin with a
    # pinned known_hosts file; STACK_APPLY passes neither.
    argv = ["env", "-C", ansible_dir, *extra_env, "ANSIBLE_LOAD_CALLBACK_PLUGINS=0",
            "ANSIBLE_CALLBACK_RESULT_FORMAT=json",
            "ansible", "all", "-i", f"{fqdn},", *_login(user), "-m", "ansible.builtin.setup",
            "-a", "filter=ansible_os_family,ansible_architecture"]
    proc = provlib.remote_run(argv, check=False, timeout=FACT_TIMEOUT, on_spawn=on_spawn)
    if proc.returncode != 0:
        raise ProvError(f"fact probe of {fqdn} failed: {_err_tail(proc)}")
    return parse_facts(_out(proc))


# ---------------------------------------------------------------------------
# Render + run
# ---------------------------------------------------------------------------
def render_stacks(family: str, arch: str, slugs: list[str], out_path: str) -> list[str]:
    argv = [sys.executable, os.path.join(HERE, "stackgen.py"), "--from-db",
            "--family", family, "--arch", arch]
    for s in slugs:
        argv += ["--stack", s]
    argv += ["--output", out_path]
    try:
        proc = subprocess.run(argv, env=provlib.stackgen_env(), capture_output=True,
                              text=True, timeout=STACKGEN_TIMEOUT)
    except subprocess.TimeoutExpired:
        raise ProvError("stackgen timed out") from None
    if proc.returncode != 0:
        msg = (proc.stderr.strip().splitlines() or [f"exit {proc.returncode}"])[-1]
        raise ProvError(f"stack render failed: {msg}")
    with open(out_path, encoding="utf-8") as fh:
        order = json.load(fh).get("solari_stack_order")
    if not isinstance(order, list) or not order or not all(
            isinstance(s, str) and provlib.SLUG_RE.match(s) for s in order):
        raise ProvError("stack render has no usable solari_stack_order")
    return order


def playbook(ansible_dir: str, fqdn: str, vars_path: str, check: bool, *,
             user: str | None = None, extra_env: tuple[str, ...] = (), on_spawn=None) -> bool:
    # Pin the built-in callback (ansible.cfg's choice too) so a stale cfg on
    # the control node naming the removed `yaml` callback cannot abort the run.
    argv = ["env", "-C", ansible_dir, *extra_env, "ANSIBLE_STDOUT_CALLBACK=ansible.builtin.default",
            "ansible-playbook", "-i", f"{fqdn},", *_login(user),
            "playbooks/site.yml", "--tags", "stack", "-e", f"@{vars_path}"]
    if check:
        argv += ["--check", "--diff"]
    log(("check pass: " if check else "apply pass: ")
        + " ".join(argv[argv.index("ansible-playbook") + 1:]))
    proc = provlib.remote_run(argv, check=False, on_spawn=on_spawn,
                              timeout=CHECK_TIMEOUT if check else APPLY_TIMEOUT)
    sys.stdout.write(_out(proc))
    err = (proc.stderr or b"").decode("utf-8", "replace")
    if err:
        sys.stdout.write(err)
    sys.stdout.flush()
    log(f"{'check' if check else 'apply'} pass exit {proc.returncode}")
    return proc.returncode == 0


# ---------------------------------------------------------------------------
def parse_args(argv: list[str] | None):
    ap = argparse.ArgumentParser(description="Apply SoR stacks to a running machine (STACK_APPLY).")
    ap.add_argument("--entity", required=True)
    ap.add_argument("--stacks", required=True)
    ap.add_argument("--dry-run", action="store_true")
    ap.add_argument("--op", required=True)
    a = ap.parse_args(argv)
    try:
        return (provlib.parse_id(a.entity, "entity"), provlib.parse_stacks(a.stacks),
                a.dry_run, provlib.parse_principal(a.op))
    except ProvError as exc:
        ap.error(str(exc))   # exit 2


def _lock(conn, name: str) -> None:
    with conn.cursor() as cur:
        cur.execute("SELECT GET_LOCK(%s, 10) AS got", (name,))
        got = (cur.fetchone() or {}).get("got")
    if got != 1:
        raise ProvError("another stack apply for this entity is in progress")


def _unlock(conn, name: str) -> None:
    try:
        with conn.cursor() as cur:
            cur.execute("SELECT RELEASE_LOCK(%s)", (name,))
            cur.fetchall()
    except Exception:  # noqa: BLE001 — the lock also dies with the session
        pass


def main(argv: list[str] | None = None) -> int:
    entity_id, slugs, dry_run, op = parse_args(argv)
    domain = os.environ.get("SOLARI_PROV_DOMAIN", DEFAULT_DOMAIN)
    if not DOMAIN_RE.match(domain):
        return provlib.die(f"SOLARI_PROV_DOMAIN {domain!r} is not a valid domain")
    ansible_dir = provlib.remote_env()["PROV_ROOT"].rstrip("/") + "/ansible"
    prov_root = provlib.remote_env()["PROV_ROOT"].rstrip("/")

    try:
        conn = provlib.connect_sor()
    except ProvError as exc:
        return provlib.die(str(exc))
    lock_name = apply_lock_name(entity_id)
    run_id = None
    try:
        with tempfile.TemporaryDirectory(prefix="stack-apply-") as tmp:
            try:
                # Held for the whole run (released by the server if this process
                # dies), so a `running` apply row for this entity found while we
                # hold it belongs to a dead helper and is failed, not obeyed.
                _lock(conn, lock_name)
                reap_dead_applies(conn, entity_id)
                with conn.cursor() as cur:
                    entity = load_entity(cur, entity_id)
                    check_stacks(cur, slugs)
                conn.commit()
                fqdn = f"{entity['name']}.{domain}"
                log(f"entity {entity_id} {fqdn}; stacks {','.join(slugs)}"
                    + (" (dry-run)" if dry_run else ""))
                os_family, architecture = probe_facts(ansible_dir, fqdn)
                family, arch = map_facts(os_family, architecture)
                log(f"facts: {os_family}/{architecture} -> {family}/{arch}")
                vars_local = os.path.join(tmp, "stack.json")
                order = render_stacks(family, arch, slugs, vars_local)
                log("stack order: " + ",".join(order))

                # The entity lock (held since the start) serialises check-then-open,
                # so two concurrent applies cannot both pass the "no active run" test.
                with conn.cursor() as cur:
                    busy = active_apply_run(cur, entity_id)
                conn.commit()
                if busy is not None:
                    raise ProvError(f"apply run {busy} for entity {entity_id} is still active")
                run_id = provlib.open_run(conn, mode="apply", entity_id=entity_id,
                                          image_id=None, stack_order=order,
                                          requested_by=op, dry_run=dry_run,
                                          detail="check pass")
                log(f"opened prov_runs {run_id}")

                vars_remote = f"{prov_root}/runs/apply-{run_id}.stack.json"
                with open(vars_local, "rb") as fh:
                    provlib.remote_write(vars_remote, fh.read(), "0644")
                log(f"vars uploaded to {vars_remote}")

                if not playbook(ansible_dir, fqdn, vars_remote, check=True):
                    raise ProvError("check pass failed")
                if dry_run:
                    provlib.advance_run(conn, run_id, "succeeded", "dry-run: check+diff only")
                    log(f"run {run_id} succeeded (dry-run)")
                    return 0
                _set_running_detail(conn, run_id, "apply pass")
                if not playbook(ansible_dir, fqdn, vars_remote, check=False):
                    raise ProvError("apply pass failed")
                provlib.advance_run(conn, run_id, "succeeded", "applied")
                log(f"run {run_id} succeeded")
                return 0
            except ProvError as exc:
                if run_id is not None:
                    _fail(conn, run_id, str(exc))
                return provlib.die(str(exc))
            except Exception as exc:  # noqa: BLE001 — never leave the run open
                if run_id is not None:
                    _fail(conn, run_id, f"internal error: {type(exc).__name__}")
                raise
    finally:
        _unlock(conn, lock_name)
        conn.close()


def apply_lock_name(entity_id: int) -> str:
    return f"solari-prov-apply-{int(entity_id)}"


def reap_dead_applies(conn, entity_id: int) -> list[int]:
    """Fail this entity's open apply runs; the caller must hold its apply lock.

    Every live stack-apply holds apply_lock_name(entity) from before it opens
    its run until it exits, and the server drops the lock with a dead
    session, so any queued/running apply row seen under the lock is orphaned."""
    reaped = []
    with conn.cursor() as cur:
        cur.execute("SELECT id FROM prov_runs WHERE entity_id=%s AND mode='apply'"
                    " AND status IN ('queued','running') ORDER BY id", (entity_id,))
        ids = [int(r["id"]) for r in cur.fetchall()]
    conn.commit()
    for rid in ids:
        try:
            provlib.advance_run(conn, rid, "failed",
                                "helper exited without a result (apply lock released); "
                                "re-run the apply")
            reaped.append(rid)
            log(f"reaped orphaned apply run {rid} (entity {entity_id})")
        except ProvError:       # finished meanwhile
            conn.rollback()
    return reaped


def reap_orphaned_applies(conn) -> list[int]:
    """prov-finish tick: fail open apply runs whose helper is gone (lock free)."""
    with conn.cursor() as cur:
        cur.execute("SELECT DISTINCT entity_id FROM prov_runs WHERE mode='apply'"
                    " AND status IN ('queued','running')")
        entities = [int(r["entity_id"]) for r in cur.fetchall()]
    conn.commit()
    reaped: list[int] = []
    for eid in entities:
        name = apply_lock_name(eid)
        with conn.cursor() as cur:
            cur.execute("SELECT GET_LOCK(%s, 0) AS got", (name,))
            got = (cur.fetchone() or {}).get("got")
        if got != 1:
            continue            # a live stack-apply owns it
        try:
            reaped += reap_dead_applies(conn, eid)
        finally:
            _unlock(conn, name)
    return reaped


def _set_running_detail(conn, run_id: int, detail: str) -> None:
    with conn.cursor() as cur:
        cur.execute("UPDATE prov_runs SET detail=%s WHERE id=%s AND status='running'",
                    (detail[:255], run_id))
    conn.commit()


def _fail(conn, run_id: int, detail: str) -> None:
    try:
        conn.rollback()
        provlib.advance_run(conn, run_id, "failed", detail)
    except Exception as exc:  # noqa: BLE001
        print(f"error: could not mark run {run_id} failed: {exc}", file=sys.stderr)


if __name__ == "__main__":
    sys.exit(main())
