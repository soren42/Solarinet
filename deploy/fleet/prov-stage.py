#!/usr/bin/env python3
"""prov-stage — PROV_STAGE helper: stage a reimage + first-boot stacks.

Invoked by solariServer's queue consumer for the operator-gated PROV_STAGE
verb with a literal argv (no shell):

  deploy/fleet/prov-stage.py --entity <id> --image <id> --stacks <a,b> \
                             --mac <mac> --arch <arch> --op <principal>

Flow:
  1. validate the arguments (provlib, the same rules as the C bridge) and pass
     the F-07 gate BEFORE touching the SoR or the network;
  2. load from the SoR: the live entity (its name must be a hostname label),
     the live image (boot_method 'ipxe' only; sha256 and fetched_at must be
     set, i.e. IMAGE_REFRESH has verified it) and every requested stack (live
     prov_stacks rows). --mac is the interface the operator picked: it must be
     a live interface of THIS entity. Arch fails closed: hardware_models.arch
     (031) wins when set and must equal --arch; either way the target arch
     must equal the image arch (--arch is the dashboard's resolution, which
     may come from the agent — the helper cannot read the monitoring DB);
  3. render the stack vars with `stackgen.py --from-db` for the image's
     family/arch (solari_stack_order = the dependency-closed run order);
  4. open a prov_runs row (mode reimage) directly as `running` (one commit,
     no queued window) and cancel any older queued/running reimage run for the
     same entity ("superseded by run <id>");
  5. keep the run's record operator-side on the provisioning host, outside
     the web root: PROV_ROOT/runs/reimage-<run>.json (target, image, admin
     user) and PROV_ROOT/runs/reimage-<run>.stack.json (the vars) — stack vars
     are never published over HTTP;
  6. stage through the EXISTING flow — fleet-provision.sh with --release and
     --run-id (path overridable via SOLARI_FLEET_PROVISION for tests), under
     the per-MAC SoR lock; the installer writes the run id to the target as an
     Ansible local fact and authorizes the provisioning host's control key.
     fleet-provision publishes the per-MAC boot profile LAST (atomic rename),
     after every installer file and profile.env, so a staging that fails
     part-way never leaves a destructive profile armed for a half-written
     install;
  7. on success the run stays `running` with detail "staged; awaiting netboot
     install". prov-finish.py (solari-prov-finish.timer) completes it: once
     the freshly installed host answers with this run id it un-stages the
     per-MAC profile, applies the stacks over SSH and marks the run
     succeeded/failed. Any failure here after the run is open marks it
     `failed` and then, under the per-MAC lock, disarms this MAC's staged
     install if profile.env names this run or a run that is no longer active
     (e.g. the one just superseded) — a failed PROV_STAGE leaves nothing armed.
     prov-finish sweeps the same condition on every tick in case this
     clean-up itself could not run.

Exit: 0 staged, 1 refusal/failure, 2 usage error. Progress goes to stdout /
stderr (the C side redirects both to the helper log). Secrets are
never printed: the SoR password travels only in the child's environment.
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
USER_RE = re.compile(r"^[a-z_][a-z0-9_-]{0,31}$")
DEFAULT_DOMAIN = "akoria.net"
DEFAULT_ADMIN_USER = "solari-admin"
# release_name reaches fleet-provision.sh argv and on-disk paths: one token.
RELEASE_RE = re.compile(r"^[A-Za-z0-9][A-Za-z0-9._-]{0,47}$")
DISTRO_RE = re.compile(r"^[a-z0-9][a-z0-9-]{0,31}$")

F07_DENIED = "[fleet][error] F-07: unsigned provisioning is disabled."
STACKGEN_TIMEOUT = 300
PROVISION_TIMEOUT = 3600
MAC_LOCK_WAIT = 120       # s; prov-finish holds the MAC lock only while un-staging


def log(msg: str) -> None:
    print(f"[prov-stage] {msg}", flush=True)


def f07_gate() -> bool:
    """Same fail-closed gate as deploy/fleet/provisioning-guard.sh."""
    if os.environ.get("SOLARI_ALLOW_UNVERIFIED_PROVISIONING", "0") != "1":
        print(F07_DENIED, file=sys.stderr)
        print("Use SOLARI_ALLOW_UNVERIFIED_PROVISIONING=1 only on an isolated lab network.",
              file=sys.stderr, flush=True)
        return False
    print("[fleet][warn] F-07 override active: unsigned provisioning is unsafe; "
          "use only on an isolated lab network.", file=sys.stderr, flush=True)
    return True


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


def check_mac(cur, entity_id: int, mac: str) -> None:
    """The operator-picked MAC must be a live interface of this entity."""
    cur.execute("SELECT mac FROM interfaces WHERE entity_id=%s AND deleted_at IS NULL"
                " AND mac IS NOT NULL", (entity_id,))
    live = {(r["mac"] or "").strip().lower().replace("-", ":") for r in cur.fetchall()}
    if mac not in live:
        raise ProvError(f"MAC {mac} is not a live interface of entity {entity_id}")


def load_model_arch(cur, entity_id: int) -> str | None:
    """hardware_models.arch of the entity's live hardware unit; None = unknown."""
    cur.execute(
        "SELECT hm.arch FROM entities e"
        " JOIN hardware_units hu ON hu.id = e.hardware_unit_id AND hu.deleted_at IS NULL"
        " JOIN hardware_models hm ON hm.id = hu.model_id AND hm.deleted_at IS NULL"
        " WHERE e.id=%s", (entity_id,))
    row = cur.fetchone()
    return (row or {}).get("arch") or None


def check_arch(entity_id: int, model_arch: str | None, asserted: str, image: dict) -> str:
    """Fail closed: the target arch must be known and equal the image arch."""
    if model_arch is not None and model_arch != asserted:
        raise ProvError(f"entity {entity_id} hardware model arch is {model_arch} but the "
                        f"request says {asserted}; refusing")
    target = model_arch or asserted
    if target != image["arch"]:
        raise ProvError(f"image {image['id']} is {image['arch']} but entity {entity_id} "
                        f"is {target}; refusing")
    return target


def load_image(cur, image_id: int) -> dict:
    cur.execute(
        "SELECT id, distro, family, release_name, arch, boot_method, sha256, fetched_at"
        " FROM prov_images WHERE id=%s AND deleted_at IS NULL", (image_id,))
    img = cur.fetchone()
    if not img:
        raise ProvError(f"image {image_id} does not exist or is deleted")
    if img["boot_method"] != "ipxe":
        raise ProvError(f"image {image_id} boots via {img['boot_method']}; "
                        "PROV_STAGE supports only ipxe images in this release")
    if not img["sha256"] or img["fetched_at"] is None:
        raise ProvError(f"image {image_id} not fetched/verified — run IMAGE_REFRESH")
    if not DISTRO_RE.match(img["distro"] or "") or not RELEASE_RE.match(img["release_name"] or ""):
        raise ProvError(f"image {image_id} has an unusable distro/release handle")
    return img


def check_stacks(cur, slugs: list[str]) -> None:
    marks = ",".join(["%s"] * len(slugs))
    cur.execute(f"SELECT slug FROM prov_stacks WHERE deleted_at IS NULL AND slug IN ({marks})",
                tuple(slugs))
    live = {r["slug"] for r in cur.fetchall()}
    missing = [s for s in slugs if s not in live]
    if missing:
        raise ProvError(f"unknown or deleted stack(s): {', '.join(missing)}")


# ---------------------------------------------------------------------------
# Render + stage
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


def supersede(conn, entity_id: int, new_run: int) -> None:
    with conn.cursor() as cur:
        cur.execute("SELECT id FROM prov_runs WHERE entity_id=%s AND mode='reimage'"
                    " AND status IN ('queued','running') AND id<>%s ORDER BY id",
                    (entity_id, new_run))
        old = [int(r["id"]) for r in cur.fetchall()]
    conn.commit()
    for rid in old:
        try:
            provlib.advance_run(conn, rid, "cancelled", f"superseded by run {new_run}")
            log(f"superseded older reimage run {rid}")
        except ProvError as exc:   # finished meanwhile: nothing to supersede
            log(f"warning: could not supersede run {rid}: {exc}")


def set_running_detail(conn, run_id: int, detail: str) -> None:
    """Update only the detail of a still-running run (status unchanged)."""
    with conn.cursor() as cur:
        cur.execute("UPDATE prov_runs SET detail=%s WHERE id=%s AND status='running'",
                    (detail[:255], run_id))
    conn.commit()


def publish_run(run_id: int, record: dict, vars_path: str) -> None:
    """Keep the run's record + vars operator-side on the provisioning host."""
    runs = provlib.remote_env()["PROV_ROOT"].rstrip("/") + "/runs"
    with open(vars_path, "rb") as fh:
        provlib.remote_write(f"{runs}/reimage-{run_id}.stack.json", fh.read(), "0600")
    provlib.remote_write(f"{runs}/reimage-{run_id}.json",
                         (json.dumps(record, sort_keys=True) + "\n").encode(), "0600")
    log(f"run record kept at {runs}/reimage-{run_id}.json (+ .stack.json)")


def stage(mac: str, image: dict, hostname: str, domain: str, run_id: int, op: str) -> None:
    # The installers live on the NAS image store: say so plainly in the run
    # detail (fleet-provision.sh refuses too, but only as "exit 1").
    provlib.require_store(("installers",))
    script = os.environ.get("SOLARI_FLEET_PROVISION") or os.path.join(HERE, "fleet-provision.sh")
    argv = [script, "--target", mac, "--distro", image["distro"], "--arch", image["arch"],
            "--release", image["release_name"], "--hostname", hostname, "--domain", domain,
            "--run-id", str(run_id), "--op", op]
    log("staging via " + " ".join(argv))
    sys.stdout.flush()
    sys.stderr.flush()
    try:
        rc = subprocess.run(argv, timeout=PROVISION_TIMEOUT).returncode
    except subprocess.TimeoutExpired:
        raise ProvError("fleet-provision timed out") from None
    except OSError as exc:
        raise ProvError(f"cannot run fleet-provision: {exc.strerror}") from None
    if rc != 0:
        raise ProvError(f"fleet-provision failed (exit {rc})")


# ---------------------------------------------------------------------------
def parse_args(argv: list[str] | None):
    ap = argparse.ArgumentParser(description="Stage a reimage of one machine with stacks (PROV_STAGE).")
    ap.add_argument("--entity", required=True)
    ap.add_argument("--image", required=True)
    ap.add_argument("--stacks", required=True)
    ap.add_argument("--mac", required=True)
    ap.add_argument("--arch", required=True)
    ap.add_argument("--op", required=True)
    a = ap.parse_args(argv)
    try:
        return (provlib.parse_id(a.entity, "entity"), provlib.parse_id(a.image, "image"),
                provlib.parse_stacks(a.stacks), provlib.parse_mac(a.mac),
                provlib.parse_arch(a.arch), provlib.parse_principal(a.op))
    except ProvError as exc:
        ap.error(str(exc))   # exit 2


def main(argv: list[str] | None = None) -> int:
    entity_id, image_id, slugs, mac, asserted_arch, op = parse_args(argv)
    if not f07_gate():
        return 1
    domain = os.environ.get("SOLARI_PROV_DOMAIN", DEFAULT_DOMAIN)
    admin_user = os.environ.get("ADMIN_USER", DEFAULT_ADMIN_USER)
    if not DOMAIN_RE.match(domain):
        return provlib.die(f"SOLARI_PROV_DOMAIN {domain!r} is not a valid domain")
    if not USER_RE.match(admin_user):
        return provlib.die(f"ADMIN_USER {admin_user!r} is not a valid user name")

    try:
        conn = provlib.connect_sor()
    except ProvError as exc:
        return provlib.die(str(exc))
    run_id = None
    try:
        with tempfile.TemporaryDirectory(prefix="prov-stage-") as tmp:
            try:
                with conn.cursor() as cur:
                    entity = load_entity(cur, entity_id)
                    check_mac(cur, entity_id, mac)
                    image = load_image(cur, image_id)
                    model_arch = load_model_arch(cur, entity_id)
                    check_stacks(cur, slugs)
                conn.commit()
                arch = check_arch(entity_id, model_arch, asserted_arch, image)
                log(f"entity {entity_id} {entity['name']} mac {mac} arch {arch} "
                    f"({'hardware model' if model_arch else 'asserted'}); image {image_id} "
                    f"{image['distro']} {image['release_name']} {image['arch']}")
                vars_path = os.path.join(tmp, "stack.json")
                order = render_stacks(image["family"], image["arch"], slugs, vars_path)
                log("stack order: " + ",".join(order))
                run_id = provlib.open_run(conn, mode="reimage", entity_id=entity_id,
                                          image_id=image_id, stack_order=order,
                                          requested_by=op, detail="staging netboot install")
                log(f"opened prov_runs {run_id}")
                supersede(conn, entity_id, run_id)
                publish_run(run_id, {
                    "run_id": run_id, "entity_id": entity_id, "image_id": image_id,
                    "hostname": entity["name"], "fqdn": f"{entity['name']}.{domain}",
                    "mac": mac, "cfgkey": mac.replace(":", "-"),
                    "family": image["family"], "arch": image["arch"],
                    "distro": image["distro"], "release": image["release_name"],
                    "admin_user": admin_user, "stack_order": order,
                }, vars_path)
                with provlib.mac_lock(conn, mac, MAC_LOCK_WAIT):
                    stage(mac, image, entity["name"], domain, run_id, op)
                set_running_detail(conn, run_id, "staged; awaiting netboot install")
            except ProvError as exc:
                if run_id is not None:
                    _fail(conn, run_id, str(exc))
                    _disarm(conn, mac, run_id, lock_busy=isinstance(exc, provlib.MacBusy))
                return provlib.die(str(exc))
            except Exception as exc:  # noqa: BLE001 — never leave the run open
                if run_id is not None:
                    _fail(conn, run_id, f"internal error: {type(exc).__name__}")
                    _disarm(conn, mac, run_id, lock_busy=False)
                raise
    finally:
        conn.close()
    log(f"run {run_id} staged; the machine installs on its next network boot")
    return 0


def _fail(conn, run_id: int, detail: str) -> None:
    try:
        conn.rollback()
        provlib.advance_run(conn, run_id, "failed", detail)
    except Exception as exc:  # noqa: BLE001
        print(f"error: could not mark run {run_id} failed: {exc}", file=sys.stderr)


def _disarm(conn, mac: str, run_id: int, *, lock_busy: bool) -> None:
    """After a failed staging: leave no destructive boot profile armed for it.

    Disarms this MAC's staged install when profile.env names this run, or a
    run that is no longer active (the run this staging superseded, whose
    profile fleet-provision had not yet replaced). Best effort: prov-finish's
    sweep disarms the same condition on its next tick if this cannot."""
    cfgkey = mac.replace(":", "-")
    if lock_busy:   # never got the MAC lock, so fleet-provision never ran
        log(f"MAC {mac} busy; prov-finish will disarm any stale profile")
        return
    try:
        conn.rollback()
        owner = provlib.disarm_stale_profile(conn, cfgkey, provlib.prov_paths(),
                                             wait=MAC_LOCK_WAIT, own_run=run_id)
    except Exception as exc:  # noqa: BLE001 — prov-finish sweeps what this misses
        print(f"error: could not disarm the {cfgkey} boot profile after run {run_id} "
              f"failed: {exc}; prov-finish will retry", file=sys.stderr)
        return
    if owner is not None:
        log(f"disarmed the {cfgkey} boot profile of run {owner}")


if __name__ == "__main__":
    sys.exit(main())
