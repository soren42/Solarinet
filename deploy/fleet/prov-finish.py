#!/usr/bin/env python3
"""prov-finish — complete PROV_STAGE reimage runs after the target installs.

PROV_STAGE (prov-stage.py) leaves a reimage run `running` ("staged; awaiting
netboot install") with its record kept operator-side on the provisioning host:

  PROV_ROOT/runs/reimage-<run>.json        target fqdn, cfgkey (MAC), family,
                                           arch, admin user, stack order
  PROV_ROOT/runs/reimage-<run>.stack.json  the rendered stack vars (0600)

and the installer it staged (fleet-provision.sh --run-id) writes the run id
to the new system as the Ansible local fact
/etc/ansible/facts.d/solari_prov.fact and authorizes the provisioning host's
control key for the admin user. This helper — run by solari-prov-finish.timer
every few minutes, or by hand — drives each such run to a terminal status:

  1. per run, a non-blocking named lock (a hand run and the timer never act
     on the same run at once);
  2. not yet installed and older than SOLARI_PROV_INSTALL_TIMEOUT seconds
     (default 21600 = 6 h) -> the per-MAC boot profile is DISARMED (moved out
     of the web root) and the run fails "install not detected ...; re-stage to
     retry". No reinstall loop and no silently armed profile;
  3. otherwise probe the target over SSH from the provisioning host, as the
     admin user, and `cat` the marker. Until the marker has been seen the
     probe uses a throw-away known_hosts (accept-new); unreachable, or another
     run id (the old OS still running, an earlier install) -> still waiting.
     The host key that first answers with THIS run's marker is pinned in
     runs/reimage-<run>.known_hosts (trust on first marker) and every later
     probe and step, on this and later ticks, requires it
     (StrictHostKeyChecking=yes);
  4. marker == this run -> the install succeeded: un-stage the per-MAC
     profile and installer configs (only while profile.env still names THIS
     run, under the per-MAC SoR lock PROV_STAGE also takes), make sure python3
     exists (zypper / apt-get via the raw module), check the installed OS
     family/arch against the record, then the stack check pass and the apply
     pass -> succeeded "installed; stacks applied". Before EACH of those
     steps the run is re-read under a row lock and must still be `running`:
     a PROV_STAGE that superseded it meanwhile (status cancelled) stops it
     before the next target-side effect, and so does a profile.env that
     names another run ("foreign": a newer staging owns the MAC — the run is
     cancelled if the stager has not already done so). A step already in
     flight when the cancel lands runs to completion; no later step starts;
  5. a failure after the install is retried on later ticks (the run stays
     running, detail "installed; ... (attempt n of N)") up to
     SOLARI_PROV_APPLY_ATTEMPTS (default 3), then the run fails. The attempt
     number is recorded before the attempt starts, so a crash mid-attempt
     still uses it up. The profile is already gone by then, so a failed apply
     never reinstalls the machine. Waiting out the per-MAC lock is not an
     attempt: a lock timeout defers the run to the next tick with the attempt
     count it had.
  6. every invocation then sweeps the provisioning host for per-MAC boot
     profiles that are still armed although the run their profile.env names
     is terminal (a PROV_STAGE whose own clean-up could not run, a run failed
     by hand) and disarms them under the per-MAC lock.

Each timer tick (no --run) first publishes the NAS image-store status for the
dashboard's Images screen (provlib.store_status -> <ctl dir>/prov-store.json;
best effort, never fails the tick).

Runs staged before this mechanism (no run record) are failed with a message
to re-stage. Nothing here talks to the target except over SSH with the
existing operator/control keys; no new credentials, no HTTP.

  deploy/fleet/prov-finish.py [--run <id>]

Exit: 0 (each run's outcome is in prov_runs), 1 SoR unreachable / internal
error, 2 usage error. Progress and playbook output go to stdout (journal).
Environment: the provlib SoR + provisioning-host variables (as for the ctl
helpers), SOLARI_PROV_INSTALL_TIMEOUT, SOLARI_PROV_APPLY_ATTEMPTS,
SOLARI_PROV_MAC_LOCK_WAIT (seconds to wait for the per-MAC lock, default 30).
"""
from __future__ import annotations

import argparse
import importlib.util
import json
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

import provlib  # noqa: E402  (path set above)
from provlib import MacBusy, ProvError  # noqa: E402

_spec = importlib.util.spec_from_file_location("stack_apply", os.path.join(HERE, "stack-apply.py"))
stack_apply = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(stack_apply)

MARKER = "/etc/ansible/facts.d/solari_prov.fact"
MARKER_RE = re.compile(r'\{\s*"run_id"\s*:\s*([1-9][0-9]{0,9})\s*\}')
CFGKEY_RE = provlib.CFGKEY_RE
FQDN_RE = re.compile(r"^[a-z0-9]([a-z0-9-]{0,61}[a-z0-9])?(\.[a-z0-9]([a-z0-9-]{0,61}[a-z0-9])?)+$")
USER_RE = re.compile(r"^[a-z_][a-z0-9_-]{0,31}$")
FAMILY_RE = re.compile(r"^[a-z][a-z0-9-]{0,31}$")

DETAIL_STAGING = "staging netboot install"
DETAIL_APPLYING = "installed; applying stacks"
# Every installed-state detail carries the number of the attempt last STARTED;
# it is written before any work, so a crash mid-attempt still consumes it.
ATTEMPT_RE = re.compile(r"^installed; .*\(attempt ([0-9]+) of [0-9]+\)")
DEFAULT_MAC_LOCK_WAIT = 30
DEFAULT_INSTALL_TIMEOUT = 21600
DEFAULT_APPLY_ATTEMPTS = 3
STORE_STATUS_TIMEOUT = 60
PROBE_TIMEOUT = 120
BOOTSTRAP_TIMEOUT = 900

# python3 for Ansible modules on a fresh install (raw needs none). AutoYaST /
# preseed / autoinstall already try; this covers a failed or offline attempt.
_PYTHON_SH = ("command -v python3 >/dev/null 2>&1 && exit 0; "
              "if command -v zypper >/dev/null 2>&1; then "
              "zypper --non-interactive install --no-recommends python3; "
              "elif command -v apt-get >/dev/null 2>&1; then "
              "DEBIAN_FRONTEND=noninteractive apt-get install -y python3; "
              "else echo 'no zypper/apt-get' >&2; exit 1; fi")


def log(msg: str) -> None:
    print(f"[prov-finish] {msg}", flush=True)


class Retry(Exception):
    """A post-install step failed; the run may be retried on a later tick."""


class Superseded(Exception):
    """The run is no longer running (cancelled by a newer PROV_STAGE): stop."""


# ---------------------------------------------------------------------------
# SoR
# ---------------------------------------------------------------------------
def pending_runs(conn, only: int | None) -> list[dict]:
    sql = ("SELECT id, entity_id, detail, TIMESTAMPDIFF(SECOND, started_at, NOW()) AS age"
           " FROM prov_runs WHERE mode='reimage' AND status='running' AND dry_run=0")
    args: tuple = ()
    if only is not None:
        sql += " AND id=%s"
        args = (only,)
    with conn.cursor() as cur:
        cur.execute(sql + " ORDER BY id", args)
        rows = list(cur.fetchall())
    conn.commit()
    return rows


def set_detail(conn, run_id: int, detail: str) -> None:
    with conn.cursor() as cur:
        cur.execute("UPDATE prov_runs SET detail=%s WHERE id=%s AND status='running'",
                    (detail[:255], run_id))
    conn.commit()


def step(conn, run_id: int, detail: str) -> None:
    """Gate before a target-side effect: the run must still be `running`.

    Reads the status under a row lock (prov-stage's supersede and every other
    status move take the same lock) and records `detail` in that transaction;
    raises Superseded when the run was cancelled or finished meanwhile."""
    with conn.cursor() as cur:
        cur.execute("SELECT status FROM prov_runs WHERE id=%s FOR UPDATE", (run_id,))
        row = cur.fetchone()
        if not row or row["status"] != "running":
            conn.rollback()
            raise Superseded(f"run is {row['status'] if row else 'gone'}")
        cur.execute("UPDATE prov_runs SET detail=%s WHERE id=%s", (detail[:255], run_id))
    conn.commit()


def gated(conn, run_id: int, detail: str, launch):
    """step() whose check and the launch of a target command are one operation.

    The run's row lock is held from the status check until `launch(on_spawn)`
    has started its child process (remote_run calls on_spawn, which commits).
    A cancel (prov-stage supersede, advance_run) therefore either lands first
    and this raises Superseded with nothing launched, or waits for the lock
    and lands after the step is in flight — that step then completes and the
    next gate stops the run."""
    with conn.cursor() as cur:
        cur.execute("SELECT status FROM prov_runs WHERE id=%s FOR UPDATE", (run_id,))
        row = cur.fetchone()
        if not row or row["status"] != "running":
            conn.rollback()
            raise Superseded(f"run is {row['status'] if row else 'gone'}")
        cur.execute("UPDATE prov_runs SET detail=%s WHERE id=%s", (detail[:255], run_id))
    committed = []

    def on_spawn() -> None:
        conn.commit()
        committed.append(True)
    try:
        return launch(on_spawn)
    finally:
        if not committed:       # never launched: release the row lock
            conn.rollback()


def finish(conn, run_id: int, status: str, detail: str) -> None:
    try:
        conn.rollback()
        provlib.advance_run(conn, run_id, status, detail)
        log(f"run {run_id} {status}: {detail}")
    except ProvError as exc:     # cancelled/superseded meanwhile: nothing to do
        log(f"warning: run {run_id} not moved to {status}: {exc}")


def try_lock(conn, name: str) -> bool:
    with conn.cursor() as cur:
        cur.execute("SELECT GET_LOCK(%s, 0) AS got", (name,))
        got = (cur.fetchone() or {}).get("got")
    return got == 1


# ---------------------------------------------------------------------------
# Provisioning host
# ---------------------------------------------------------------------------
class Ctx:
    def __init__(self, env=None):
        self.prov_root, self.http_root = provlib.prov_paths(env)
        self.runs = f"{self.prov_root}/runs"
        self.mac_lock_wait = DEFAULT_MAC_LOCK_WAIT
        self.ansible_dir = f"{self.prov_root}/ansible"


def load_record(ctx: Ctx, row: dict) -> dict | None:
    """The run record, None when absent (legacy run); ProvError on transport error."""
    path = f"{ctx.runs}/reimage-{row['id']}.json"
    proc = provlib.remote_run(["sh", "-c", 'test -f "$1" || exit 3; cat "$1"', "sh", path],
                              check=False, timeout=60)
    if proc.returncode == 3:
        return None
    if proc.returncode != 0:
        raise ProvError(f"cannot read {path}: {stack_apply._err_tail(proc)}")
    try:
        rec = json.loads(proc.stdout.decode("utf-8"))
    except ValueError:
        rec = None
    ok = (isinstance(rec, dict) and rec.get("run_id") == row["id"]
          and rec.get("entity_id") == row["entity_id"]
          and isinstance(rec.get("fqdn"), str) and FQDN_RE.match(rec["fqdn"])
          and isinstance(rec.get("cfgkey"), str) and CFGKEY_RE.match(rec["cfgkey"])
          and isinstance(rec.get("admin_user"), str) and USER_RE.match(rec["admin_user"])
          and isinstance(rec.get("family"), str) and FAMILY_RE.match(rec["family"])
          and rec.get("arch") in provlib.ARCHES)
    if not ok:
        raise Retry("run record is malformed or does not match the run")
    return rec


def unstage(conn, ctx: Ctx, rec: dict, run_id: int, how: str) -> str:
    # The MAC lock keeps a concurrent PROV_STAGE from publishing between the
    # profile.env ownership check and the removals (provlib.mac_lock).
    with provlib.mac_lock(conn, rec["cfgkey"], ctx.mac_lock_wait):
        return provlib.unstage_profile(run_id, rec["cfgkey"], how,
                                       (ctx.prov_root, ctx.http_root))


def sweep_armed(conn, ctx: Ctx) -> int:
    """Disarm armed per-MAC profiles whose run is terminal; returns how many."""
    paths = (ctx.prov_root, ctx.http_root)
    armed = provlib.armed_profiles(paths)
    if not armed:
        return 0
    marks = ",".join(["%s"] * len(armed))
    with conn.cursor() as cur:
        cur.execute(f"SELECT id FROM prov_runs WHERE id IN ({marks})"
                    " AND status IN ('succeeded','failed','cancelled')",
                    tuple(run for _cfg, run in armed))
        done = {int(r["id"]) for r in cur.fetchall()}
    conn.commit()
    n = 0
    for cfg, run in armed:
        if run not in done:
            continue
        try:   # re-checked under the MAC lock: a staging may have replaced it
            owner = provlib.disarm_stale_profile(conn, cfg, paths, wait=ctx.mac_lock_wait)
        except MacBusy:
            log(f"sweep: MAC {cfg} busy; next tick")
            continue
        if owner is not None:
            log(f"sweep: disarmed the {cfg} boot profile of terminal run {owner}")
            n += 1
    return n


def ssh_env(kh: str, *, pinned: bool) -> tuple[str, ...]:
    # host_key_checking=True drops ansible.cfg's StrictHostKeyChecking=no.
    mode = "yes" if pinned else "accept-new"
    return ("ANSIBLE_HOST_KEY_CHECKING=True",
            f"ANSIBLE_SSH_COMMON_ARGS=-o UserKnownHostsFile={kh} -o StrictHostKeyChecking={mode}")


def raw(ctx: Ctx, rec: dict, cmd: str, env: tuple[str, ...], *, become: bool,
        timeout: int, on_spawn=None):
    argv = ["env", "-C", ctx.ansible_dir, *env, f"ANSIBLE_BECOME={'True' if become else 'False'}",
            "ANSIBLE_LOAD_CALLBACK_PLUGINS=0", "ansible", "all", "-i", f"{rec['fqdn']},",
            "-u", rec["admin_user"], "-m", "ansible.builtin.raw", "-a", cmd]
    return provlib.remote_run(argv, check=False, timeout=timeout, on_spawn=on_spawn)


def read_marker(ctx: Ctx, rec: dict, kh: str) -> int | None:
    """The run id the host's marker carries; pins its host key on first match.

    Trust on first marker: until the installed system answers with this run's
    marker the host key is unknown (the old OS or the installer may still be
    on that name), so each probe uses a throw-away known_hosts with
    accept-new. The key that first answers with the marker is moved into the
    run's pinned known_hosts, and from then on every probe and step uses
    StrictHostKeyChecking=yes against it — a different host appearing later
    is unreachable, never re-trusted."""
    if provlib.remote_run(["test", "-s", kh], check=False, timeout=60).returncode == 0:
        proc = raw(ctx, rec, f"cat {MARKER}", ssh_env(kh, pinned=True), become=False,
                   timeout=PROBE_TIMEOUT)
        m = MARKER_RE.search(stack_apply._out(proc)) if proc.returncode == 0 else None
        return int(m.group(1)) if m else None
    probe = kh + ".probe"
    provlib.remote_run(["rm", "-f", probe], timeout=60)
    proc = raw(ctx, rec, f"cat {MARKER}", ssh_env(probe, pinned=False), become=False,
               timeout=PROBE_TIMEOUT)
    m = MARKER_RE.search(stack_apply._out(proc)) if proc.returncode == 0 else None
    run = int(m.group(1)) if m else None
    if run == rec["run_id"]:
        provlib.remote_run(["mv", "-f", probe, kh], timeout=60)
    else:
        provlib.remote_run(["rm", "-f", probe], check=False, timeout=60)
    return run


# ---------------------------------------------------------------------------
# One run
# ---------------------------------------------------------------------------
def complete(conn, ctx: Ctx, rec: dict, run_id: int, kh: str, suffix: str) -> None:
    """Install detected: un-stage, bootstrap, verify, check + apply. Raises Retry.

    `suffix` (" (attempt n of N)") is in every progress detail, so a crash
    mid-way still counts this attempt. gated() re-checks the run is still
    running as it launches every target-side command (Superseded otherwise); a MAC
    lock timeout propagates as MacBusy (not an attempt)."""
    step(conn, run_id, DETAIL_APPLYING + suffix)
    try:
        word = unstage(conn, ctx, rec, run_id, "finish")
    except MacBusy:
        raise
    except ProvError as exc:
        raise Retry(str(exc)) from None
    if word == "foreign":       # a newer staging owns the MAC: this run is over
        log(f"run {run_id}: boot profile belongs to a newer staging (left in place)")
        raise Superseded("a newer staging owns the boot profile")
    log(f"run {run_id}: install detected on {rec['fqdn']}; boot profile "
        + ("removed" if word == "done" else "already removed"))
    pinned = ssh_env(kh, pinned=True)
    proc = gated(conn, run_id, "installed; bootstrapping python3" + suffix,
                 lambda spawned: raw(ctx, rec, _PYTHON_SH, pinned, become=True,
                                     timeout=BOOTSTRAP_TIMEOUT, on_spawn=spawned))
    if proc.returncode != 0:
        raise Retry(f"python3 bootstrap failed: {stack_apply._err_tail(proc)}")
    try:
        fam, arch = stack_apply.map_facts(*gated(
            conn, run_id, "installed; checking facts" + suffix,
            lambda spawned: stack_apply.probe_facts(
                ctx.ansible_dir, rec["fqdn"], user=rec["admin_user"], extra_env=pinned,
                on_spawn=spawned)))
    except ProvError as exc:
        raise Retry(str(exc)) from None
    if (fam, arch) != (rec["family"], rec["arch"]):
        raise Retry(f"installed system reports {fam}/{arch}, expected {rec['family']}/{rec['arch']}")
    vars_path = f"{ctx.runs}/reimage-{run_id}.stack.json"
    for check, what in ((True, "check"), (False, "apply")):
        if not gated(conn, run_id, f"installed; {what} pass" + suffix,
                     lambda spawned: stack_apply.playbook(
                         ctx.ansible_dir, rec["fqdn"], vars_path, check,
                         user=rec["admin_user"], extra_env=pinned, on_spawn=spawned)):
            raise Retry(f"stack {what} pass failed")


def process(conn, ctx: Ctx, row: dict, *, install_timeout: int, attempts: int) -> str:
    run_id = int(row["id"])
    detail = row["detail"] or ""
    installed = detail.startswith("installed;")
    timed_out = not installed and int(row["age"] or 0) > install_timeout
    if detail == DETAIL_STAGING and not timed_out:
        return "staging"        # prov-stage.py is still writing the record/profile
    try:
        rec = load_record(ctx, row)
    except Retry as exc:
        finish(conn, run_id, "failed", str(exc))
        return "failed"
    if rec is None:
        finish(conn, run_id, "failed",
               "staging never completed (no run record); re-stage to retry"
               if detail == DETAIL_STAGING else
               "staged before completion tracking (no run record); re-stage to retry")
        return "failed"
    if timed_out:
        word = unstage(conn, ctx, rec, run_id, "disarm")
        finish(conn, run_id, "failed",
               f"install not detected within {install_timeout / 3600:g} h; boot profile "
               + ("disarmed" if word in ("done", "already") else "left to the newer staging")
               + "; re-stage to retry")
        return "failed"
    m = ATTEMPT_RE.match(detail)
    started = int(m.group(1)) if m else 0
    if started >= attempts:     # the last attempt died without recording an outcome
        finish(conn, run_id, "failed", "installed; stacks not applied (attempt "
               f"{started} of {attempts} ended without a result)")
        return "failed"
    n = started + 1
    if installed:               # every tick of an installed run is an attempt: count it first
        set_detail(conn, run_id, f"installed; probing host (attempt {n} of {attempts})")
    kh = f"{ctx.runs}/reimage-{run_id}.known_hosts"
    marker = read_marker(ctx, rec, kh)
    if marker != run_id:
        if installed:           # was reachable before: count it as a failed attempt
            return _retry(conn, run_id, n, attempts, "installed host not reachable")
        return "waiting"
    try:
        complete(conn, ctx, rec, run_id, kh, f" (attempt {n} of {attempts})")
    except MacBusy as exc:      # not an attempt: put the count back, next tick
        set_detail(conn, run_id, f"installed; {exc}; deferred"
                   + (f" (attempt {started} of {attempts})" if started else ""))
        log(f"run {run_id}: {exc}; deferred to the next tick")
        return "busy"
    except Superseded as exc:
        finish(conn, run_id, "cancelled", f"superseded: {exc}; stacks not applied")
        return "superseded"
    except Retry as exc:
        return _retry(conn, run_id, n, attempts, str(exc))
    finish(conn, run_id, "succeeded", "installed; stacks applied")
    provlib.remote_run(["rm", "-f", kh], check=False, timeout=60)
    return "succeeded"


def _retry(conn, run_id: int, n: int, attempts: int, why: str) -> str:
    """Attempt `n` failed: fail the run on the last one, else leave it for a retry."""
    why = why[:160]             # keep the attempt count inside the 255-char detail
    if n >= attempts:
        finish(conn, run_id, "failed", f"installed; {why}; stacks not applied "
               f"(attempt {n} of {attempts})")
        return "failed"
    set_detail(conn, run_id, f"installed; {why}; retrying (attempt {n} of {attempts})")
    log(f"run {run_id}: {why}; will retry (attempt {n} of {attempts})")
    return "retry"


# ---------------------------------------------------------------------------
def _int_env(name: str, default: int, minimum: int) -> int:
    raw_v = os.environ.get(name, "")
    if raw_v == "":
        return default
    if not re.fullmatch(r"[0-9]{1,9}", raw_v) or int(raw_v) < minimum:
        raise ProvError(f"{name} must be an integer >= {minimum}")
    return int(raw_v)


def publish_store() -> None:
    """Refresh the image-store status the dashboard shows (best effort, every
    tick: a failure is logged and never fails the tick). Under the test seam
    it only runs with an explicit SOLARI_PROV_STATUS_DIR, so a test can never
    write into the live ctl directory."""
    if os.environ.get("SOLARI_PROV_LOCAL") == "1" and not os.environ.get("SOLARI_PROV_STATUS_DIR"):
        return
    try:
        status = provlib.store_status(timeout=STORE_STATUS_TIMEOUT)
        provlib.publish_store_status(status)
    except Exception as exc:  # noqa: BLE001 — informational only
        log(f"store status: error: {exc}")
        return
    if not status["ok"]:
        log(f"store status: NOT OK: {status['reason']}")


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description="Complete staged reimage runs after first boot.")
    ap.add_argument("--run", help="process only this prov_runs id")
    a = ap.parse_args(argv)
    try:
        only = provlib.parse_id(a.run, "run") if a.run is not None else None
    except ProvError as exc:
        ap.error(str(exc))
    if only is None:
        publish_store()
    try:
        install_timeout = _int_env("SOLARI_PROV_INSTALL_TIMEOUT", DEFAULT_INSTALL_TIMEOUT, 600)
        attempts = _int_env("SOLARI_PROV_APPLY_ATTEMPTS", DEFAULT_APPLY_ATTEMPTS, 1)
        ctx = Ctx()
        ctx.mac_lock_wait = _int_env("SOLARI_PROV_MAC_LOCK_WAIT", DEFAULT_MAC_LOCK_WAIT, 0)
        conn = provlib.connect_sor()
    except ProvError as exc:
        return provlib.die(str(exc))
    rc = 0
    try:
        for row in pending_runs(conn, only):
            name = f"solari-prov-finish-{row['id']}"
            if not try_lock(conn, name):
                log(f"run {row['id']}: busy (another prov-finish holds it)")
                continue
            try:
                # Re-read under the per-run lock: the list above may be stale
                # (finished by another prov-finish, superseded by PROV_STAGE).
                fresh = pending_runs(conn, int(row["id"]))
                if not fresh:
                    log(f"run {row['id']}: no longer running")
                    continue
                with provlib.lock_watch(conn, name):
                    outcome = process(conn, ctx, fresh[0], install_timeout=install_timeout,
                                      attempts=attempts)
                log(f"run {row['id']}: {outcome}")
            except MacBusy as exc:
                log(f"run {row['id']}: {exc}; next tick")
            except Exception as exc:  # noqa: BLE001 — leave the run, try the next tick
                log(f"run {row['id']}: error: {exc}")
                rc = 1
            finally:
                stack_apply._unlock(conn, name)
        try:
            sweep_armed(conn, ctx)
        except Exception as exc:  # noqa: BLE001 — retried every tick
            log(f"sweep: error: {exc}")
            rc = 1
        try:
            stack_apply.reap_orphaned_applies(conn)
        except Exception as exc:  # noqa: BLE001 — retried every tick
            log(f"apply reaper: error: {exc}")
            rc = 1
    finally:
        conn.close()
    return rc


if __name__ == "__main__":
    sys.exit(main())
