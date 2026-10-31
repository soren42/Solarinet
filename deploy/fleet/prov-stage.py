#!/usr/bin/env python3
"""prov-stage — PROV_STAGE helper: stage a reimage + first-boot stacks.

Invoked by solariServer's queue consumer for the operator-gated PROV_STAGE
verb with a literal argv (no shell):

  deploy/fleet/prov-stage.py --entity <id> --image <id> --stacks <a,b> --op <principal>

Flow:
  1. validate the arguments (provlib, the same rules as the C bridge) and pass
     the F-07 gate BEFORE touching the SoR or the network;
  2. load from the SoR: the live entity (its name must be a hostname label),
     its MAC (live interface with a MAC: management first, then physical, then
     lowest id), the live image (boot_method 'ipxe' only in P3; sha256 and
     fetched_at must be set, i.e. IMAGE_REFRESH has verified it) and every
     requested stack (live prov_stacks rows);
  3. render the stack vars with `stackgen.py --from-db` for the image's
     family/arch (solari_stack_order = the dependency-closed run order);
  4. open a prov_runs row (mode reimage) and cancel any older queued/running
     reimage run for the same entity ("superseded by run <id>");
  5. stage through the EXISTING flow — fleet-provision.sh with --release and
     --stack-vars (path overridable via SOLARI_FLEET_PROVISION for tests);
  6. on success the run stays `running` with detail "staged; awaiting netboot
     install" (the install itself completes on the machine's next PXE boot);
     any failure after the run is open marks it `failed`.

Exit: 0 staged, 1 refusal/failure, 2 usage error. Progress goes to stdout /
stderr (the C side redirects both to run/prov-stage-<entity>.log). Secrets are
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
MAC_RE = re.compile(r"^[0-9a-f]{2}(:[0-9a-f]{2}){5}$")
# release_name reaches fleet-provision.sh argv and on-disk paths: one token.
RELEASE_RE = re.compile(r"^[A-Za-z0-9][A-Za-z0-9._-]{0,47}$")
DISTRO_RE = re.compile(r"^[a-z0-9][a-z0-9-]{0,31}$")

F07_DENIED = "[fleet][error] F-07: unsigned provisioning is disabled."
STACKGEN_TIMEOUT = 300
PROVISION_TIMEOUT = 3600


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


def load_mac(cur, entity_id: int) -> str:
    cur.execute(
        "SELECT mac FROM interfaces WHERE entity_id=%s AND deleted_at IS NULL AND mac IS NOT NULL"
        " ORDER BY is_management DESC, (if_kind='physical') DESC, id ASC LIMIT 1",
        (entity_id,))
    row = cur.fetchone()
    if not row:
        raise ProvError(f"entity {entity_id} has no live interface with a MAC address")
    mac = (row["mac"] or "").strip().lower()
    if not MAC_RE.match(mac):
        raise ProvError(f"entity {entity_id} MAC {row['mac']!r} is malformed")
    return mac


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


def stage(mac: str, image: dict, hostname: str, vars_path: str, op: str) -> None:
    script = os.environ.get("SOLARI_FLEET_PROVISION") or os.path.join(HERE, "fleet-provision.sh")
    argv = [script, "--target", mac, "--distro", image["distro"], "--arch", image["arch"],
            "--release", image["release_name"], "--hostname", hostname,
            "--stack-vars", vars_path, "--op", op]
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
    ap.add_argument("--op", required=True)
    a = ap.parse_args(argv)
    try:
        return (provlib.parse_id(a.entity, "entity"), provlib.parse_id(a.image, "image"),
                provlib.parse_stacks(a.stacks), provlib.parse_principal(a.op))
    except ProvError as exc:
        ap.error(str(exc))   # exit 2


def main(argv: list[str] | None = None) -> int:
    entity_id, image_id, slugs, op = parse_args(argv)
    if not f07_gate():
        return 1

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
                    mac = load_mac(cur, entity_id)
                    image = load_image(cur, image_id)
                    check_stacks(cur, slugs)
                conn.commit()
                log(f"entity {entity_id} {entity['name']} mac {mac}; image {image_id} "
                    f"{image['distro']} {image['release_name']} {image['arch']}")
                vars_path = os.path.join(tmp, "stack.json")
                order = render_stacks(image["family"], image["arch"], slugs, vars_path)
                log("stack order: " + ",".join(order))
                run_id = provlib.open_run(conn, mode="reimage", entity_id=entity_id,
                                          image_id=image_id, stack_order=order,
                                          requested_by=op)
                log(f"opened prov_runs {run_id}")
                supersede(conn, entity_id, run_id)
                provlib.advance_run(conn, run_id, "running", "staging netboot install")
                stage(mac, image, entity["name"], vars_path, op)
                set_running_detail(conn, run_id, "staged; awaiting netboot install")
            except ProvError as exc:
                if run_id is not None:
                    _fail(conn, run_id, str(exc))
                return provlib.die(str(exc))
            except Exception as exc:  # noqa: BLE001 — never leave the run open
                if run_id is not None:
                    _fail(conn, run_id, f"internal error: {type(exc).__name__}")
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


if __name__ == "__main__":
    sys.exit(main())
