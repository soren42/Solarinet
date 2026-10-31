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
The helpers never leave a row queued: open_run() INSERTs it already `running`
(started_at set) in one commit, so a helper that dies right after opening
leaves a run its own recovery path sees (prov-finish for reimage, the next
refresh's supersede for refresh), never one stuck in `queued`.

Per-MAC boot profiles (PROV_STAGE): the per-MAC iPXE profile is what makes a
machine wipe itself on its next network boot. Every write to one MAC's files
holds mac_lock(); unstage_profile() removes (finish) or disarms one only while
PROV_ROOT/profiles/<cfgkey>/profile.env still names the given run, and
disarm_stale_profile() disarms a profile whose run is no longer active.

Provisioning host (benzene) — the same env names and defaults as
fleet-lib.sh: BENZENE, BENZENE_IP, HTTP_PORT, HTTP_BASE, PROV_ROOT, HTTP_ROOT,
PROV_STORE (default PROV_ROOT/store) and PROV_STORE_FSTYPE (default cifs).
remote_run() ssh-es there with the argv shlex-quoted (the remote login shell
sees literal words, never interpolated operator input). Test seam:
SOLARI_PROV_LOCAL=1 runs the same argv locally instead, so tests point
PROV_ROOT/HTTP_ROOT at a temp dir and never touch a network.

Image store (NAS): installers/ and images/ under HTTP_ROOT are links into
PROV_STORE, a CIFS share automounted on benzene. require_store() runs
prov-store-guard.sh there and raises ProvError ("provisioning image store not
mounted ...") unless the share is mounted and linked, so nothing big is ever
written to benzene's local disk under an unmounted mountpoint.
PROV_STORE_FSTYPE=none (skip the mount check) is refused outside the seam.
store_status() reports the same facts without failing; publish_store_status()
writes them to <status dir>/prov-store.json for GET /api/provisioning/store
(status dir: SOLARI_PROV_STATUS_DIR, else the ctl socket's directory, which
the dashboard already reads helper logs from).

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
import time
from typing import Any, Callable, Iterable

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


class MacBusy(ProvError):
    """mac_lock() timed out: another step holds the MAC. Transient — retry later."""


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
    """INSERT a `running` prov_runs row (started_at = now) and commit; returns its id.

    One statement, one commit: there is no queued window a crash could strand.
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
            " status, dry_run, detail, started_at, source_id, asserted_kind)"
            " VALUES (%s,%s,%s,%s,%s,'running',%s,%s,CURRENT_TIMESTAMP,%s,'human')",
            (mode, entity_id, image_id, json.dumps(slugs), requested_by,
             1 if dry_run else 0, _clip(detail), src))
        run_id = int(cur.lastrowid)
    conn.commit()
    return run_id


def run_status(conn, run_id: int) -> str | None:
    """Current status of one run (None = no such row); ends the read transaction."""
    with conn.cursor() as cur:
        cur.execute("SELECT status FROM prov_runs WHERE id=%s", (run_id,))
        row = cur.fetchone()
    conn.commit()
    return row["status"] if row else None


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
        raise MacBusy(f"another provisioning step holds MAC {mac}; try again shortly")
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
            "HTTP_ROOT": env.get("HTTP_ROOT", f"{prov_root}/netboot/http"),
            "PROV_STORE": env.get("PROV_STORE", f"{prov_root}/store"),
            "PROV_STORE_FSTYPE": env.get("PROV_STORE_FSTYPE", "cifs")}


# Active lock watch for remote_run, innermost last: (interval_s, still_held).
_WATCHES: list[tuple[float, Callable[[], bool]]] = []
def _watch_interval() -> float:
    raw_v = os.environ.get("SOLARI_PROV_LOCK_WATCH", "")
    return float(raw_v) if re.fullmatch(r"[0-9]{1,4}(\.[0-9]{1,3})?", raw_v) and float(raw_v) > 0 else 15.0


LOCK_WATCH_INTERVAL = _watch_interval()


def holds_lock(conn, name: str) -> bool:
    """True when this SoR session still owns named lock `name` (False on any error)."""
    try:
        with conn.cursor() as cur:
            cur.execute("SELECT IS_USED_LOCK(%s) = CONNECTION_ID() AS mine", (name,))
            row = cur.fetchone()
        conn.commit()
    except Exception:  # noqa: BLE001 — a dead session no longer owns anything
        return False
    mine = row.get("mine") if isinstance(row, dict) else (row[0] if row else None)
    return mine == 1


@contextlib.contextmanager
def lock_watch(conn, name: str, interval: float = LOCK_WATCH_INTERVAL):
    """While active, every remote_run re-checks `name` is still held each
    `interval` seconds and kills its child when it is not. The named locks
    are session-scoped: if the SoR session drops, the server frees the lock
    and a reaper may treat the run as orphaned, so the step must not go on."""
    _WATCHES.append((float(interval), lambda: holds_lock(conn, name)))
    try:
        yield
    finally:
        _WATCHES.pop()


def remote_run(argv: list[str], *, input: bytes | None = None, check: bool = True,
               timeout: float | None = None,
               on_spawn: Callable[[], None] | None = None) -> subprocess.CompletedProcess:
    """Run argv on the provisioning host (or locally under SOLARI_PROV_LOCAL=1).

    `on_spawn` runs once the child exists and before it is waited on; if it
    raises, the child is killed and the exception propagates. prov-finish
    uses it to commit its run-status gate only after the step is in flight.
    Under lock_watch() the child is killed if the watched lock is lost."""
    if not argv or not all(isinstance(a, str) for a in argv):
        raise ProvError("remote_run needs a non-empty list of strings")
    if os.environ.get("SOLARI_PROV_LOCAL") == "1":
        cmd = list(argv)
    else:
        cmd = ["ssh", "-o", "BatchMode=yes", remote_env()["BENZENE"], "--", shlex.join(argv)]
    watch = _WATCHES[-1] if _WATCHES else None
    deadline = time.monotonic() + timeout if timeout is not None else None
    with subprocess.Popen(cmd, stdin=subprocess.PIPE if input is not None else subprocess.DEVNULL,
                          stdout=subprocess.PIPE, stderr=subprocess.PIPE) as child:
        failure: BaseException | None = None
        try:
            if on_spawn is not None:
                on_spawn()
            while True:
                wait = watch[0] if watch else None
                if deadline is not None:
                    left = max(0.0, deadline - time.monotonic())
                    wait = left if wait is None else min(wait, left)
                try:
                    # retrying communicate() after a timeout loses no output
                    out, err = child.communicate(input, timeout=wait)
                    break
                except subprocess.TimeoutExpired:
                    if deadline is not None and time.monotonic() >= deadline:
                        failure = ProvError(f"timed out: {argv[0]}")
                        break
                    if watch and not watch[1]():
                        failure = ProvError(f"{argv[0]} aborted: provisioning lock lost "
                                            "(SoR session dropped)")
                        break
        except BaseException as exc:  # noqa: BLE001 — kill the child, then re-raise
            failure = exc
        if failure is not None:
            child.kill()
            child.communicate()
            if isinstance(failure, ProvError):
                raise failure from None
            raise failure
    proc = subprocess.CompletedProcess(cmd, child.returncode, out, err)
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


# ---------------------------------------------------------------------------
# Per-MAC boot profiles on the provisioning host
# ---------------------------------------------------------------------------
CFGKEY_RE = re.compile(r"^[0-9a-f]{2}(-[0-9a-f]{2}){5}$")
# PROV_ROOT / HTTP_ROOT reach ssh options (space-split by Ansible) and a remote shell.
PATH_RE = re.compile(r"^/[A-Za-z0-9._/-]+$")


def prov_paths(env: dict[str, str] | None = None) -> tuple[str, str]:
    """(PROV_ROOT, HTTP_ROOT) without a trailing slash; plain absolute paths only."""
    renv = remote_env(env)
    out = []
    for name in ("PROV_ROOT", "HTTP_ROOT"):
        path = renv[name].rstrip("/")
        if not PATH_RE.match(path) or "/../" in path + "/":
            raise ProvError(f"{name} {path!r} is not a plain absolute path")
        out.append(path)
    return out[0], out[1]


# ---- NAS image store ---------------------------------------------------------
STORE_SUBS = ("installers", "images")
FSTYPE_RE = re.compile(r"^[a-z0-9.]{1,16}$")
STORE_GUARD = os.path.join(os.path.dirname(os.path.abspath(__file__)), "prov-store-guard.sh")
STORE_STATUS_FILE = "prov-store.json"


def store_config(env: dict[str, str] | None = None) -> tuple[str, str]:
    """(PROV_STORE, PROV_STORE_FSTYPE), validated; "none" only under the seam."""
    env = os.environ if env is None else env
    renv = remote_env(env)
    store, fstype = renv["PROV_STORE"].rstrip("/"), renv["PROV_STORE_FSTYPE"]
    if not PATH_RE.match(store) or "/../" in store + "/":
        raise ProvError(f"PROV_STORE {store!r} is not a plain absolute path")
    if not FSTYPE_RE.match(fstype):
        raise ProvError(f"PROV_STORE_FSTYPE {fstype!r} is not a filesystem type name")
    if fstype == "none" and env.get("SOLARI_PROV_LOCAL") != "1":
        raise ProvError("PROV_STORE_FSTYPE=none is only allowed under SOLARI_PROV_LOCAL=1")
    return store, fstype


def _store_guard(mode: str, subs: Iterable[str], timeout: float) -> subprocess.CompletedProcess:
    store, fstype = store_config()
    http_root = prov_paths()[1]
    with open(STORE_GUARD, encoding="utf-8") as fh:
        script = fh.read() + '\nprov_store "$@"\n'
    return remote_run(["sh", "-c", script, "prov-store", mode, store, fstype, http_root, *subs],
                      check=False, timeout=timeout)


def require_store(subs: Iterable[str] = STORE_SUBS, timeout: float = 90) -> None:
    """Fail closed unless the image store is mounted and HTTP_ROOT/<sub> links into it."""
    proc = _store_guard("check", subs, timeout)
    if proc.returncode != 0:
        lines = [ln.removeprefix("[store][error] ") for ln in
                 (proc.stderr or b"").decode("utf-8", "replace").strip().splitlines()
                 if ln.startswith("[store]")]
        raise ProvError(lines[-1] if lines else
                        f"provisioning image store not mounted or unreachable ({_err_tail(proc)})")


def store_status(timeout: float = 60) -> dict[str, Any]:
    """The guard's status report as a dict (never raises for an unmounted store)."""
    store, fstype = store_config()
    why = "store check printed no status"
    try:
        proc = _store_guard("status", STORE_SUBS, timeout)
    except ProvError as exc:
        proc, why = None, str(exc)
    raw: dict[str, str] = {}
    if proc is not None and proc.returncode == 0:
        for line in proc.stdout.decode("utf-8", "replace").splitlines():
            key, sep, val = line.partition("=")
            if sep and re.fullmatch(r"[a-z_]{1,16}", key):
                raw[key] = val[:512]
    elif proc is not None:
        why = f"store check could not run on the provisioning host ({_err_tail(proc)})"

    def num(key: str) -> int | None:
        return int(raw[key]) * 1024 if raw.get(key, "").isdigit() else None
    return {"store": store, "fstype": fstype, "reachable": bool(raw),
            "mounted": raw.get("mounted") == "yes", "linked": raw.get("linked") == "yes",
            "ok": raw.get("ok") == "yes", "source": raw.get("source") or None,
            "size_bytes": num("size_kb"), "avail_bytes": num("avail_kb"),
            "reason": raw.get("reason") or (None if raw else why),
            "checked_at": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime())}


def status_dir(env: dict[str, str] | None = None) -> str:
    env = os.environ if env is None else env
    return env.get("SOLARI_PROV_STATUS_DIR") or os.path.dirname(
        env.get("SOLARI_CTL_SOCK") or "/run/solari/solariCtl.sock")


def publish_store_status(status: dict[str, Any]) -> str:
    """Atomically write the status for the dashboard; returns the path.

    0644: it carries no secret (path, mount flag, share name, free space) and
    the directory (the ctl socket's, 0751) is not listable by the web user."""
    path = os.path.join(status_dir(), STORE_STATUS_FILE)
    tmp = f"{path}.tmp.{os.getpid()}"
    fd = os.open(tmp, os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_NOFOLLOW, 0o644)
    try:
        with os.fdopen(fd, "w", encoding="utf-8") as fh:
            os.fchmod(fh.fileno(), 0o644)
            json.dump(status, fh, sort_keys=True)
            fh.write("\n")
        os.replace(tmp, path)
    except BaseException:
        with contextlib.suppress(OSError):
            os.unlink(tmp)
        raise
    return path


# Un-stage / disarm one per-MAC profile, but only while profile.env still
# names THIS run (a newer PROV_STAGE for the same MAC owns the files
# otherwise). Idempotent; prints one word: done|already|foreign. "disarm"
# moves the boot profile out of the web root (kept for the record).
_UNSTAGE_SH = r'''
set -eu
run=$1 cfg=$2 prov=$3 http=$4 how=$5
pdir="$prov/profiles/$cfg"
if [ -f "$pdir/finished-$run.env" ] || [ -f "$pdir/disarmed-$run.env" ]; then echo already; exit 0; fi
if [ ! -f "$pdir/profile.env" ] || ! grep -qx "run_id=$run" "$pdir/profile.env"; then echo foreign; exit 0; fi
if [ "$how" = disarm ] && [ -f "$http/ipxe/mac-$cfg.ipxe" ]; then
  mv -f "$http/ipxe/mac-$cfg.ipxe" "$pdir/disarmed-$run.ipxe"
fi
rm -f "$http/ipxe/mac-$cfg.ipxe" "$http/configs/$cfg.xml" "$http/configs/$cfg.preseed" \
      "$http/configs/$cfg.stack.json"
rm -rf "$http/configs/ubuntu/$cfg"
if [ "$how" = disarm ]; then mv -f "$pdir/profile.env" "$pdir/disarmed-$run.env"
else mv -f "$pdir/profile.env" "$pdir/finished-$run.env"; fi
echo done
'''

# Owner of one MAC's staged files: the run_id profile.env names (may be
# empty). Exit 3 = no profile.env.
_OWNER_SH = r'''
set -eu
f="$1/profiles/$2/profile.env"
[ -f "$f" ] || exit 3
sed -n 's/^run_id=//p' "$f" | head -n 1
'''

# Every armed per-MAC profile: "<cfgkey> <run_id>" per line.
_ARMED_SH = r'''
set -eu
prov=$1 http=$2
for f in "$prov"/profiles/*/profile.env; do
  [ -f "$f" ] || continue
  d=${f%/profile.env}; cfg=${d##*/}
  [ -f "$http/ipxe/mac-$cfg.ipxe" ] || continue
  printf '%s %s\n' "$cfg" "$(sed -n 's/^run_id=//p' "$f" | head -n 1)"
done
'''


def _err_tail(proc: subprocess.CompletedProcess) -> str:
    text = (proc.stderr or b"").decode("utf-8", "replace").strip().splitlines()
    return " | ".join(text[-3:]) or f"exit {proc.returncode}"


def unstage_profile(run_id: int, cfgkey: str, how: str, paths: tuple[str, str]) -> str:
    """Run _UNSTAGE_SH for one run; the caller holds mac_lock(). done|already|foreign."""
    if not CFGKEY_RE.match(cfgkey) or how not in ("finish", "disarm"):
        raise ProvError(f"refusing to un-stage {cfgkey!r} ({how!r})")
    proc = remote_run(["sh", "-c", _UNSTAGE_SH, "sh", str(run_id), cfgkey, paths[0], paths[1],
                       how], check=False, timeout=60)
    word = proc.stdout.decode("utf-8", "replace").strip()
    if proc.returncode != 0 or word not in ("done", "already", "foreign"):
        raise ProvError(f"un-staging {cfgkey} failed: {_err_tail(proc)}")
    return word


def profile_owner(cfgkey: str, paths: tuple[str, str]) -> int | None:
    """The run id one MAC's profile.env names; None when absent or empty."""
    if not CFGKEY_RE.match(cfgkey):
        raise ProvError(f"refusing cfgkey {cfgkey!r}")
    proc = remote_run(["sh", "-c", _OWNER_SH, "sh", paths[0], cfgkey], check=False, timeout=60)
    if proc.returncode == 3:
        return None
    if proc.returncode != 0:
        raise ProvError(f"cannot read the {cfgkey} profile owner: {_err_tail(proc)}")
    run = proc.stdout.decode("utf-8", "replace").strip()
    return int(run) if ID_RE.match(run) else None


def armed_profiles(paths: tuple[str, str]) -> list[tuple[str, int]]:
    """Every armed per-MAC profile whose profile.env names a run: (cfgkey, run id)."""
    proc = remote_run(["sh", "-c", _ARMED_SH, "sh", paths[0], paths[1]], check=False, timeout=60)
    if proc.returncode != 0:
        raise ProvError(f"cannot list armed boot profiles: {_err_tail(proc)}")
    out = []
    for line in proc.stdout.decode("utf-8", "replace").splitlines():
        cfg, _, run = line.strip().partition(" ")
        if CFGKEY_RE.match(cfg) and ID_RE.match(run):
            out.append((cfg, int(run)))
    return out


def disarm_stale_profile(conn, cfgkey: str, paths: tuple[str, str], *, wait: int,
                         own_run: int | None = None) -> int | None:
    """Disarm one MAC's staged install if nothing live owns it; returns the run disarmed.

    Under mac_lock(): reads which run profile.env names and disarms it when it
    is `own_run` (the caller's own staging just failed) or a run that is no
    longer queued/running in the SoR (failed, cancelled — e.g. superseded by a
    staging that then failed — or even succeeded). A live run's profile, an
    unknown run id, or a profile.env with no run id (FLEET_PROVISION) is left
    alone. Raises MacBusy when the lock is held."""
    with mac_lock(conn, cfgkey, wait):
        owner = profile_owner(cfgkey, paths)
        if owner is None:
            return None
        if owner != own_run and run_status(conn, owner) not in TERMINAL:
            return None
        unstage_profile(owner, cfgkey, "disarm", paths)
        return owner


def die(msg: str, code: int = 1) -> int:
    print(f"error: {msg}", file=sys.stderr)
    return code
