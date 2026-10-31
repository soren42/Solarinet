#!/usr/bin/env python3
"""provlib — shared contract for the P3 provisioning helpers.

The three operator-gated ctl verbs (IMAGE_REFRESH, PROV_STAGE, STACK_APPLY)
are dispatched by solariServer's queue consumer, which execs ONE helper per
verb with a literal argv (no shell):

  deploy/fleet/image-refresh.py --image <id> --op <principal>
  deploy/fleet/prov-stage.py    --entity <id> --image <id> --stacks <a,b> --mac <mac> --arch <arch> --op <principal>
  deploy/fleet/stack-apply.py   --entity <id> --stacks <a,b> [--dry-run] --op <principal>

The C side has already validated every argument with the same rules as below;
the helpers validate again (defence in depth: an operator may run them by hand).

SoR connection — never from argv. In order:
  1. SOR_DB_* environment (SOR_DB_NAME required; SOR_DB_HOST, SOR_DB_PORT,
     SOR_DB_USER, SOR_DB_PASS, SOR_DB_SOCKET) — the stackgen convention, used
     by tests against a scratch DB;
  2. an INI file named by SOLARI_PROV_DB_CNF (default
     /etc/solarinet/prov-sor.cnf), section [sor], keys database/host/port/
     user/password/socket. Refused unless it is a regular file with no group
     or other permission bits (0600/0400).

prov_runs lifecycle is forward-only (029's trg_prov_runs_bu enforces it in the
DB; advance_run() refuses the same moves client-side for a clean message):
  queued -> running | failed | cancelled;  running -> succeeded | failed | cancelled.

Provisioning host (benzene) — the same env names and defaults as
fleet-lib.sh: BENZENE, BENZENE_IP, HTTP_PORT, HTTP_BASE, PROV_ROOT, HTTP_ROOT.
remote_run() ssh-es there with the argv shlex-quoted (the remote login shell
sees literal words, never interpolated operator input). Test seam:
SOLARI_PROV_LOCAL=1 runs the same argv locally instead, so tests point
PROV_ROOT/HTTP_ROOT at a temp dir and never touch a network.

Stdlib only; pymysql is imported lazily by connect_sor().
"""
from __future__ import annotations

import configparser
import contextlib
import json
import os
import re
import shlex
import stat
import subprocess
import sys
from typing import Any, Iterable

# The same rules the C validators in src/server/solariCtl.c enforce.
ID_RE = re.compile(r"^[1-9][0-9]{0,9}$")             # unsigned decimal, no sign/zero-pad, < 10^10
SLUG_RE = re.compile(r"^[a-z0-9][a-z0-9-]{0,47}$")   # 029 slug CHECK, bounded to VARCHAR(48)
PRINCIPAL_RE = re.compile(r"^[A-Za-z0-9][A-Za-z0-9._@+:-]{0,127}$")
MAC_RE = re.compile(r"^[0-9a-f]{2}(:[0-9a-f]{2}){5}$")     # lowercase, colon-separated
# prov_images.arch == hardware_models.arch (031) vocabulary.
ARCHES = ("x86_64", "arm64", "armhf", "riscv64", "i386")
MAX_STACKS = 32

DEFAULT_CNF = "/etc/solarinet/prov-sor.cnf"
PROVISIONER_SOURCE = "provisioner"

TERMINAL = frozenset({"succeeded", "failed", "cancelled"})
# mode -> (entity required, image required); mirrors 031 ck_prov_runs_shape.
RUN_SHAPES = {"reimage": (True, True), "apply": (True, False), "refresh": (False, True)}
_FORWARD = {
    "queued": frozenset({"running", "failed", "cancelled"}),
    "running": frozenset({"succeeded", "failed", "cancelled"}),
}


class ProvError(Exception):
    """A refusal or failure the helper reports and exits non-zero on."""


# ---------------------------------------------------------------------------
# Argument validation
# ---------------------------------------------------------------------------
def parse_id(value: str, what: str) -> int:
    if not isinstance(value, str) or not ID_RE.match(value):
        raise ProvError(f"{what} must be a positive decimal integer")
    n = int(value)
    if n > 4294967295:
        raise ProvError(f"{what} is out of range")
    return n


def parse_stacks(value: str) -> list[str]:
    """Comma-separated slugs → ordered, de-duplicated list. Empty is refused."""
    if not isinstance(value, str) or value == "":
        raise ProvError("stacks must name at least one stack slug")
    out: list[str] = []
    for slug in value.split(","):
        if not SLUG_RE.match(slug):
            raise ProvError(f"invalid stack slug {slug!r}")
        if slug not in out:
            out.append(slug)
    if len(out) > MAX_STACKS:
        raise ProvError(f"at most {MAX_STACKS} stacks per run")
    return out


def parse_mac(value: str) -> str:
    """The deploy target's MAC exactly as the C bridge passes it (lowercase a:b:..)."""
    if not isinstance(value, str) or not MAC_RE.match(value):
        raise ProvError("mac must be a lowercase colon-separated MAC address")
    return value


def parse_arch(value: str) -> str:
    if value not in ARCHES:
        raise ProvError(f"arch must be one of {', '.join(ARCHES)}")
    return value


def parse_principal(value: str) -> str:
    if not isinstance(value, str) or not PRINCIPAL_RE.match(value):
        raise ProvError("op must be a non-empty operator principal")
    return value


# ---------------------------------------------------------------------------
# SoR connection
# ---------------------------------------------------------------------------
def _cnf_params(path: str) -> dict[str, Any]:
    try:
        st = os.stat(path, follow_symlinks=False)
    except FileNotFoundError:
        raise ProvError(f"no SoR credentials: SOR_DB_NAME unset and {path} missing") from None
    if not stat.S_ISREG(st.st_mode):
        raise ProvError(f"{path} is not a regular file")
    if st.st_mode & 0o077:
        raise ProvError(f"{path} must not be group/other accessible (chmod 600)")
    cp = configparser.ConfigParser(interpolation=None)
    with open(path, encoding="utf-8") as fh:
        cp.read_file(fh)
    if not cp.has_section("sor"):
        raise ProvError(f"{path} has no [sor] section")
    sec = cp["sor"]
    return {"database": sec.get("database", ""), "host": sec.get("host", "127.0.0.1"),
            "port": sec.get("port", "3306"), "user": sec.get("user", ""),
            "password": sec.get("password", ""), "socket": sec.get("socket", "")}


def sor_params(env: dict[str, str] | None = None) -> dict[str, Any]:
    env = os.environ if env is None else env
    if env.get("SOR_DB_NAME"):
        p = {"database": env["SOR_DB_NAME"], "host": env.get("SOR_DB_HOST", "127.0.0.1"),
             "port": env.get("SOR_DB_PORT", "3306"), "user": env.get("SOR_DB_USER", ""),
             "password": env.get("SOR_DB_PASS", ""), "socket": env.get("SOR_DB_SOCKET", "")}
    else:
        p = _cnf_params(env.get("SOLARI_PROV_DB_CNF", DEFAULT_CNF))
    if not p["database"]:
        raise ProvError("SoR database name is empty")
    try:
        p["port"] = int(p["port"])
    except (TypeError, ValueError):
        raise ProvError("SoR port must be an integer") from None
    return p


def connect_sor(env: dict[str, str] | None = None):
    """Autocommit-off pymysql connection with DictCursor rows."""
    p = sor_params(env)
    try:
        import pymysql  # noqa: PLC0415 — optional dependency
        import pymysql.cursors  # noqa: PLC0415
    except ImportError:
        raise ProvError("the pymysql module is required") from None
    kwargs: dict[str, Any] = {"database": p["database"], "user": p["user"],
                              "password": p["password"], "charset": "utf8mb4",
                              "autocommit": False,
                              "cursorclass": pymysql.cursors.DictCursor}
    if p["socket"]:
        kwargs["unix_socket"] = p["socket"]
    else:
        kwargs["host"], kwargs["port"] = p["host"], p["port"]
    try:
        return pymysql.connect(**kwargs)
    except pymysql.MySQLError as exc:
        # The exception text never contains the password; the name is not secret.
        raise ProvError(f"cannot connect to SoR database {p['database']!r}: {exc}") from None


def stackgen_env(env: dict[str, str] | None = None) -> dict[str, str]:
    """Environment for a stackgen --from-db child: the same SoR the helper uses."""
    p = sor_params(env)
    out = dict(os.environ if env is None else env)
    out.update({"SOR_DB_NAME": p["database"], "SOR_DB_HOST": str(p["host"]),
                "SOR_DB_PORT": str(p["port"]), "SOR_DB_USER": p["user"],
                "SOR_DB_PASS": p["password"]})
    if p["socket"]:
        out["SOR_DB_SOCKET"] = p["socket"]
    else:
        out.pop("SOR_DB_SOCKET", None)
    return out


# ---------------------------------------------------------------------------
# prov_runs
# ---------------------------------------------------------------------------
def provisioner_source_id(cur) -> int:
    cur.execute("SELECT id FROM sources WHERE slug=%s AND deleted_at IS NULL",
                (PROVISIONER_SOURCE,))
    row = cur.fetchone()
    if not row:
        raise ProvError("SoR has no live 'provisioner' source row (migration 021)")
    return int(row["id"])


def open_run(conn, *, mode: str, entity_id: int | None, image_id: int | None,
             stack_order: Iterable[str], requested_by: str, dry_run: bool = False,
             detail: str | None = None) -> int:
    """INSERT a queued prov_runs row and commit; returns its id.

    Shapes (031 ck_prov_runs_shape): reimage = entity + image; apply = entity,
    no image; refresh = image, no entity (stack_order empty).
    """
    if mode not in RUN_SHAPES:
        raise ProvError(f"invalid run mode {mode!r}")
    need_entity, need_image = RUN_SHAPES[mode]
    if (entity_id is not None) != need_entity or (image_id is not None) != need_image:
        raise ProvError("reimage runs need an entity and an image; apply runs an entity and "
                        "no image; refresh runs an image and no entity")
    slugs = list(stack_order)
    with conn.cursor() as cur:
        src = provisioner_source_id(cur)
        cur.execute(
            "INSERT INTO prov_runs (mode, entity_id, image_id, stack_slugs, requested_by,"
            " status, dry_run, detail, source_id, asserted_kind)"
            " VALUES (%s,%s,%s,%s,%s,'queued',%s,%s,%s,'human')",
            (mode, entity_id, image_id, json.dumps(slugs), requested_by,
             1 if dry_run else 0, _clip(detail), src))
        run_id = int(cur.lastrowid)
    conn.commit()
    return run_id


@contextlib.contextmanager
def mac_lock(conn, mac: str, wait: int):
    """SoR named lock serialising every write to one MAC's netboot files.

    PROV_STAGE (fleet-provision publishing a profile) and prov-finish
    (un-staging / disarming one) both run against the same per-MAC files on
    the provisioning host; holding this across each keeps an older run's
    clean-up from removing a newer staging's files mid-publish."""
    name = "solari-prov-mac-" + parse_mac(mac.replace("-", ":"))
    with conn.cursor() as cur:
        cur.execute("SELECT GET_LOCK(%s, %s) AS got", (name, wait))
        row = cur.fetchone()
    got = row.get("got") if isinstance(row, dict) else (row[0] if row else None)
    if got != 1:
        raise ProvError(f"another provisioning step holds MAC {mac}; try again shortly")
    try:
        yield
    finally:
        try:
            with conn.cursor() as cur:
                cur.execute("SELECT RELEASE_LOCK(%s)", (name,))
                cur.fetchall()
        except Exception:  # noqa: BLE001 — the lock also dies with the session
            pass


def advance_run(conn, run_id: int, status: str, detail: str | None = None) -> None:
    """Move a run forward (never back, never out of a terminal state) and commit."""
    with conn.cursor() as cur:
        cur.execute("SELECT status FROM prov_runs WHERE id=%s FOR UPDATE", (run_id,))
        row = cur.fetchone()
        if not row:
            conn.rollback()
            raise ProvError(f"prov_runs {run_id} does not exist")
        cur_status = row["status"]
        if status not in _FORWARD.get(cur_status, frozenset()):
            conn.rollback()
            raise ProvError(f"prov_runs {run_id}: {cur_status} -> {status} is not a forward move")
        sets = ["status=%s", "detail=%s"]
        if status == "running":
            sets.append("started_at=CURRENT_TIMESTAMP")
        if status in TERMINAL:
            sets.append("finished_at=CURRENT_TIMESTAMP")
        cur.execute(f"UPDATE prov_runs SET {', '.join(sets)} WHERE id=%s",
                    (status, _clip(detail), run_id))
    conn.commit()


def _clip(detail: str | None) -> str | None:
    if detail is None:
        return None
    flat = " ".join(str(detail).split())
    return flat[:255]


# ---------------------------------------------------------------------------
# Provisioning host
# ---------------------------------------------------------------------------
def remote_env(env: dict[str, str] | None = None) -> dict[str, str]:
    env = os.environ if env is None else env
    benzene_ip = env.get("BENZENE_IP", "10.5.2.50")
    port = env.get("HTTP_PORT", "8080")
    prov_root = env.get("PROV_ROOT", "/srv/solari-provision")
    return {"BENZENE": env.get("BENZENE", "jason@benzene.akoria.net"),
            "HTTP_BASE": env.get("HTTP_BASE", f"http://{benzene_ip}:{port}"),
            "PROV_ROOT": prov_root,
            "HTTP_ROOT": env.get("HTTP_ROOT", f"{prov_root}/netboot/http")}


def remote_run(argv: list[str], *, input: bytes | None = None, check: bool = True,
               timeout: float | None = None) -> subprocess.CompletedProcess:
    """Run argv on the provisioning host (or locally under SOLARI_PROV_LOCAL=1)."""
    if not argv or not all(isinstance(a, str) for a in argv):
        raise ProvError("remote_run needs a non-empty list of strings")
    if os.environ.get("SOLARI_PROV_LOCAL") == "1":
        cmd = list(argv)
    else:
        cmd = ["ssh", "-o", "BatchMode=yes", remote_env()["BENZENE"], "--", shlex.join(argv)]
    try:
        proc = subprocess.run(cmd, input=input, capture_output=True, timeout=timeout)
    except subprocess.TimeoutExpired:
        raise ProvError(f"timed out: {argv[0]}") from None
    if check and proc.returncode != 0:
        tail = proc.stderr.decode("utf-8", "replace").strip().splitlines()[-3:]
        raise ProvError(f"{argv[0]} failed (exit {proc.returncode}): {' | '.join(tail)}")
    return proc


_WRITE_SH = ('umask 022; mkdir -p "$(dirname "$1")" && cat > "$1.tmp.$$" '
             '&& chmod "$2" "$1.tmp.$$" && mv -f "$1.tmp.$$" "$1"')


def remote_write(path: str, data: bytes, mode: str = "0644") -> None:
    """Atomically write bytes to an absolute path on the provisioning host."""
    if not path.startswith("/") or "\0" in path or "/../" in path or path.endswith("/.."):
        raise ProvError(f"refusing remote path {path!r}")
    if not re.fullmatch(r"0[0-7]{3}", mode):
        raise ProvError(f"invalid mode {mode!r}")
    remote_run(["sh", "-c", _WRITE_SH, "sh", path, mode], input=data)


def die(msg: str, code: int = 1) -> int:
    print(f"error: {msg}", file=sys.stderr)
    return code
