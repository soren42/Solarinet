#!/usr/bin/env python3
"""image-refresh.py — the IMAGE_REFRESH helper: re-fetch one netboot image.

    image-refresh.py --image <id> --op <principal>

Exec'd (no shell) by solariServer's queue consumer for the operator-gated
IMAGE_REFRESH verb; safe to run by hand. Exit 0 ok, 1 failure/refusal, 2 usage.

Result: every refresh of an existing image is a prov_runs row (031 shape:
mode='refresh', image_id set, entity_id NULL, stack_slugs []), requested_by =
--op, opened running (provlib.open_run) -> succeeded | failed. The detail says what was
installed or why it failed (and whether the old image was kept). Only one
refresh per image runs at a time (SoR GET_LOCK held for the whole run); any
older refresh run of the image still queued/running is therefore orphaned and
is cancelled ("superseded by run <id>"), as prov-stage.py does for reimages.
Every exit — refusal, failure, exception, SIGTERM — leaves the run terminal.

1. Validate (provlib), connect to the SoR, load the LIVE prov_images row
   (unknown id -> refused before any run is opened). Only boot_method='ipxe'
   rows are refreshed here (a failed run otherwise): Pi-native netboot and
   write-to-media images are not served by the iPXE menu. The NAS image
   store must be mounted on benzene with HTTP_ROOT/installers linked into it
   (provlib.require_store), else the run fails before anything is fetched.
2. Build the fetch plan from source_url / checksum_url (no hardcoded lists):
     debian    linux + initrd.gz from source_url; checksum_url is the
               installer's SHA256SUMS (paths ./netboot/debian-installer/<arch>/…).
     ubuntu    the netboot tarball NAMED IN the release SHA256SUMS
               (ubuntu-<point>-netboot-<arch>.tar.gz, newest point release of
               release_name); linux + initrd are extracted from it.
     opensuse  linux + initrd from source_url (…/boot/<arch>/loader/); checksum
               source = checksum_url, or when NULL the repo's CHECKSUMS file
               (source_url minus boot/<arch>/loader/, + CHECKSUMS).
   No checksum source, or no entry for a file -> refuse. Never fetch unverified.
   Checksum sources must be https (http/file only under the SOLARI_PROV_LOCAL
   test seam).
3. Download the checksum file here (urllib, timeout, size cap) and parse the
   expected sha256 for every file.
4. Run deploy/fleet/fetch-images.sh, which downloads on benzene into a staging
   dir, verifies each sha256, extracts, writes SHA256SUMS.solari + solari-image.json
   and atomically swaps it into installers/<distro>/<release>/<arch>. The
   previous version dir is KEPT on benzene until steps 5 and 6 succeed.
5. UPDATE prov_images SET version, sha256, fetched_at=NOW(). sha256 is the
   sha256 of that image's SHA256SUMS.solari manifest (one CHAR(64) for a
   multi-file image). version = the upstream point release where the names
   carry it (Ubuntu), else `fetched-YYYYMMDD` (UTC) — Debian's SHA256SUMS and
   openSUSE's CHECKSUMS carry no build id, and no extra request is made for one.
6. Regenerate boot.ipxe from every live, fetched iPXE row (netbootlib) and
   write it to HTTP_ROOT/ipxe/boot.ipxe on benzene (provlib.remote_write).
7. Commit: `fetch-images.sh --prune <new>` drops the previous version dir. A
   prune failure is only a warning (the new image is consistent and serving).

Atomicity (F4): if step 5 or 6 fails (or the helper is interrupted after the
swap), the swap is undone — the SoR row is restored to its previous version/
sha256/fetched_at if step 5 had committed, then `fetch-images.sh --rollback`
re-points the symlink at the previous, intact version dir (or removes it when
there was none) and deletes the new one. boot.ipxe is written atomically and
last, so a failed step 6 never changed it: the OLD image keeps serving with a
menu and SoR row that match it. The run fails with "...; old image kept"
(or "rollback FAILED: ..." when the undo itself could not complete).

Test seam (SOLARI_PROV_LOCAL=1 only): SOLARI_PROV_URL_REWRITE holds
whitespace-separated `PREFIX|REPLACEMENT` pairs applied to every download URL
(e.g. the real upstream prefix -> a file:// fixture tree), so the SoR rows keep
their real https URLs while tests stay offline.
"""
from __future__ import annotations

import argparse
import datetime as dt
import os
import re
import signal
import subprocess
import sys
import urllib.parse
import urllib.request

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

import netbootlib  # noqa: E402
import provlib  # noqa: E402
from provlib import ProvError  # noqa: E402

FETCH_SH = os.path.join(HERE, "fetch-images.sh")
CHECKSUM_MAX = 8 * 1024 * 1024
CHECKSUM_TIMEOUT = 60
FETCH_TIMEOUT = 4 * 3600
MENU_LOCK = "solari_prov_boot_menu"
REFRESH_LOCK = "solari-prov-refresh-{}"

_CK_LINE = re.compile(r"([0-9a-fA-F]{64}) [ *](\S.*)")
_UBU_TARBALL = re.compile(r"ubuntu-([0-9]+\.[0-9]+(?:\.[0-9]+)*)-netboot-([a-z0-9]+)\.tar\.gz")
_MANIFEST_LINE = re.compile(r"manifest-sha256 ([0-9a-f]{64})")
_STAGED_LINE = re.compile(r"staged (\.[A-Za-z0-9_]+\.v\.[A-Za-z0-9]{8})")
_PREVIOUS_LINE = re.compile(r"previous (-|\.[A-Za-z0-9_]+\.(?:v\.[A-Za-z0-9]{8}|legacy\.[0-9]{1,10}))")

COLUMNS = ("id, distro, family, release_name, release_label, arch, boot_method, source_url,"
           " checksum_url, version, sha256, fetched_at, deleted_at")


def log(msg: str) -> None:
    print(f"[image-refresh] {msg}", flush=True)


def seam() -> bool:
    return os.environ.get("SOLARI_PROV_LOCAL") == "1"


# ---------------------------------------------------------------------------
# URLs
# ---------------------------------------------------------------------------
def require_scheme(url: str, what: str) -> None:
    scheme = urllib.parse.urlsplit(url).scheme
    allowed = ("https", "http") if seam() else ("https",)
    if scheme not in allowed:
        raise ProvError(f"{what} must be an https URL (got {url!r})")


def rewrite(url: str) -> str:
    """Download URL after the test-seam rewrite (identity outside the seam)."""
    if not seam():
        return url
    for pair in os.environ.get("SOLARI_PROV_URL_REWRITE", "").split():
        prefix, sep, repl = pair.partition("|")
        if sep and prefix and url.startswith(prefix):
            return repl + url[len(prefix):]
    return url


def dir_of(url: str) -> str:
    return url[: url.rfind("/") + 1]


def fetch_text(url: str) -> str:
    real = rewrite(url)
    scheme = urllib.parse.urlsplit(real).scheme
    if scheme not in (("https", "http", "file") if seam() else ("https",)):
        raise ProvError(f"refusing to download {real!r}")
    try:
        with urllib.request.urlopen(real, timeout=CHECKSUM_TIMEOUT) as resp:  # noqa: S310 — scheme checked
            final = resp.geturl()
            if not seam() and urllib.parse.urlsplit(final).scheme != "https":
                raise ProvError(f"checksum download was redirected off https: {final!r}")
            data = resp.read(CHECKSUM_MAX + 1)
    except ProvError:
        raise
    except Exception as exc:  # urllib raises a zoo of types
        raise ProvError(f"cannot download checksum file {url}: {exc}") from None
    if len(data) > CHECKSUM_MAX:
        raise ProvError(f"checksum file {url} is larger than {CHECKSUM_MAX} bytes")
    return data.decode("utf-8", "replace")


def parse_checksums(text: str) -> dict[str, str]:
    """sha256sum-format lines ('<hex>  path' / '<hex> *path'); './' prefixes dropped."""
    out: dict[str, str] = {}
    for raw in text.splitlines():
        m = _CK_LINE.fullmatch(raw.rstrip("\r"))
        if not m:
            continue
        sha, path = m.group(1).lower(), m.group(2).strip()
        while path.startswith("./"):
            path = path[2:]
        if out.get(path, sha) != sha:
            raise ProvError(f"checksum file lists {path} twice with different sha256")
        out[path] = sha
    return out


# ---------------------------------------------------------------------------
# Plan
# ---------------------------------------------------------------------------
def checksum_source(row: dict) -> str:
    if row.get("checksum_url"):
        return row["checksum_url"]
    if row["distro"] == "opensuse":
        try:
            return netbootlib.suse_install_repo(row) + "CHECKSUMS"
        except ValueError as exc:
            raise ProvError(f"cannot derive the openSUSE CHECKSUMS location: {exc}") from None
    raise ProvError(f"image {row['id']} has no checksum_url and none can be derived for "
                    f"{row['distro']}; refusing to fetch unverified files")


def build_plan(row: dict, today: dt.date) -> dict:
    """{'files': [(name, url, sha)], 'extracts': [(tb, member, out)], 'version': str,
    'checksum_url': str}. Downloads only the checksum file."""
    try:
        netbootlib.image_dir(row)
        uarch = netbootlib.upstream_arch(row["distro"], row["arch"])
        kernel, initrd = netbootlib.boot_files(row)
    except ValueError as exc:
        raise ProvError(str(exc)) from None
    src = row.get("source_url") or ""
    if not netbootlib.UPSTREAM_DIR_RE.fullmatch(src):
        raise ProvError(f"image {row['id']} source_url must be a plain upstream directory URL ending in '/'")
    require_scheme(src, "source_url")
    ck_url = checksum_source(row)
    if not netbootlib.UPSTREAM_FILE_RE.fullmatch(ck_url):
        raise ProvError(f"image {row['id']} checksum_url is not a plain URL: {ck_url!r}")
    require_scheme(ck_url, "checksum source")
    log(f"checksum source {ck_url}")
    sums = parse_checksums(fetch_text(ck_url))
    if not sums:
        raise ProvError(f"no sha256 entries in {ck_url}")
    ck_dir = dir_of(ck_url)

    version = f"fetched-{today:%Y%m%d}"
    extracts: list[tuple[str, str, str]] = []
    if row["distro"] == "ubuntu":
        release = row["release_name"]
        cands = []
        for path in sums:
            m = _UBU_TARBALL.fullmatch(path)
            if m and m.group(2) == uarch and (m.group(1) == release or m.group(1).startswith(release + ".")):
                cands.append((tuple(int(p) for p in m.group(1).split(".")), m.group(1), path))
        if not cands:
            raise ProvError(f"{ck_url} lists no ubuntu-{release}[.N]-netboot-{uarch}.tar.gz; "
                            "refusing to fetch an unverified netboot tarball")
        _, version, tarball = max(cands)
        wanted = [(tarball, src + tarball)]
        extracts = [(tarball, f"{uarch}/linux", kernel), (tarball, f"{uarch}/initrd", initrd)]
    else:
        wanted = [(kernel, src + kernel), (initrd, src + initrd)]

    files = []
    for name, url in wanted:
        if not url.startswith(ck_dir):
            raise ProvError(f"{url} is not under the checksum file's directory {ck_dir}")
        rel = url[len(ck_dir):]
        sha = sums.get(rel)
        if not sha:
            raise ProvError(f"{ck_url} has no sha256 entry for {rel}; refusing to fetch it unverified")
        files.append((name, url, sha))
    return {"files": files, "extracts": extracts, "version": version, "checksum_url": ck_url}


# ---------------------------------------------------------------------------
# SoR
# ---------------------------------------------------------------------------
def load_row(conn, image_id: int) -> dict:
    with conn.cursor() as cur:
        cur.execute(f"SELECT {COLUMNS} FROM prov_images WHERE id=%s AND deleted_at IS NULL", (image_id,))
        row = cur.fetchone()
    conn.rollback()
    if not row:
        raise ProvError(f"no live prov_images row with id {image_id}")
    return row


def require_ipxe(row: dict) -> None:
    image_id = row["id"]
    if row["boot_method"] != "ipxe":
        raise ProvError(f"image {image_id} ({row['distro']} {row['release_name']} {row['arch']}) boots via "
                        f"{row['boot_method']}; only iPXE images are fetched for the boot menu "
                        "(Pi netboot / media images are built by the pi/ tooling)")


def record(conn, row: dict, version: str, sha256: str) -> None:
    # Guard with a locking read, not UPDATE's rowcount: MariaDB counts CHANGED
    # rows, so re-recording identical values within a second would read as 0.
    try:
        with conn.cursor() as cur:
            cur.execute(
                "SELECT id FROM prov_images"
                " WHERE id=%s AND deleted_at IS NULL AND boot_method='ipxe'"
                " AND distro=%s AND release_name=%s AND arch=%s"
                " AND source_url <=> %s AND checksum_url <=> %s FOR UPDATE",
                (row["id"], row["distro"], row["release_name"], row["arch"],
                 row["source_url"], row["checksum_url"]))
            if cur.fetchone() is None:
                conn.rollback()
                raise ProvError(f"image {row['id']} was edited or deleted during the refresh; "
                                "the SoR row was not updated — re-run IMAGE_REFRESH")
            cur.execute("UPDATE prov_images SET version=%s, sha256=%s, fetched_at=NOW() WHERE id=%s",
                        (version, sha256, row["id"]))
        conn.commit()
    except ProvError:
        raise
    except Exception as exc:  # noqa: BLE001 — pymysql errors carry no secrets
        _rollback(conn)
        raise ProvError(f"SoR update failed: {exc}") from None


def restore(conn, row: dict, digest: str) -> None:
    """Undo record(): put back the version/sha256/fetched_at load_row() saw."""
    try:
        conn.rollback()
        with conn.cursor() as cur:
            cur.execute("SELECT id FROM prov_images WHERE id=%s AND sha256=%s FOR UPDATE",
                        (row["id"], digest))
            if cur.fetchone() is None:
                conn.rollback()
                raise ProvError(f"image {row['id']} changed again; SoR row not restored")
            cur.execute("UPDATE prov_images SET version=%s, sha256=%s, fetched_at=%s WHERE id=%s",
                        (row["version"], row["sha256"], row["fetched_at"], row["id"]))
        conn.commit()
    except ProvError:
        raise
    except Exception as exc:  # noqa: BLE001
        _rollback(conn)
        raise ProvError(f"SoR restore failed: {exc}") from None


def _rollback(conn) -> None:
    try:
        conn.rollback()
    except Exception:  # noqa: BLE001
        pass


def regenerate_menu(conn) -> int:
    """Render boot.ipxe from every live fetched iPXE row and write it on benzene."""
    http_base = provlib.remote_env()["HTTP_BASE"]
    http_root = provlib.remote_env()["HTTP_ROOT"]
    with conn.cursor() as cur:
        cur.execute("SELECT GET_LOCK(%s, 120) AS got", (MENU_LOCK,))
        if not (cur.fetchone() or {}).get("got"):
            raise ProvError("timed out waiting for the boot-menu lock")
        try:
            cur.execute(f"SELECT {COLUMNS} FROM prov_images WHERE deleted_at IS NULL"
                        " AND boot_method='ipxe' AND sha256 IS NOT NULL AND fetched_at IS NOT NULL"
                        " ORDER BY id")
            rows = list(cur.fetchall())
            conn.rollback()
            try:
                text = netbootlib.render_boot_menu(rows, http_base=http_base)
            except ValueError as exc:
                raise ProvError(f"boot menu not regenerated: {exc}") from None
            provlib.remote_write(f"{http_root}/ipxe/boot.ipxe", text.encode("utf-8"), "0644")
        finally:
            # Best effort: the menu may already be written, and the lock dies
            # with the session anyway — a failed release must not undo a swap.
            try:
                cur.execute("SELECT RELEASE_LOCK(%s)", (MENU_LOCK,))
                cur.fetchall()
                conn.rollback()
            except Exception as exc:  # noqa: BLE001
                log(f"warning: could not release the boot-menu lock: {exc}")
    return len(rows)


# ---------------------------------------------------------------------------
# Fetch
# ---------------------------------------------------------------------------
def fetch_op(row: dict, *op: str) -> None:
    """fetch-images.sh --prune / --rollback for this image's dir."""
    argv = ["bash", FETCH_SH, "--dest", netbootlib.image_dir(row), *op]
    try:
        proc = subprocess.run(argv, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True,
                              timeout=600)
    except (OSError, subprocess.TimeoutExpired) as exc:
        raise ProvError(f"fetch-images.sh {op[0]}: {exc}") from None
    sys.stdout.write(proc.stdout)
    sys.stderr.write(proc.stderr)
    if proc.returncode != 0:
        tail = " | ".join(proc.stderr.strip().splitlines()[-2:])
        raise ProvError(f"fetch-images.sh {op[0]} failed (exit {proc.returncode}): {tail}")


def run_fetch(row: dict, plan: dict, swap: dict) -> str:
    """Fetch + verify + swap. Fills swap['staged'/'previous'/'swapped'] as
    fetch-images.sh reports them, so a caller can roll back even on failure."""
    argv = ["bash", FETCH_SH, "--dest", netbootlib.image_dir(row)]
    for name, url, sha in plan["files"]:
        argv += ["--file", name, rewrite(url), sha]
    for tb, member, out in plan["extracts"]:
        argv += ["--extract", tb, member, out]
    meta = {"distro": row["distro"], "release_name": row["release_name"],
            "release_label": row.get("release_label") or "", "arch": row["arch"],
            "version": plan["version"], "source_url": row["source_url"]}
    for k, v in meta.items():
        argv += ["--meta", k, v]
    try:
        proc = subprocess.Popen(argv, stdout=subprocess.PIPE, text=True, start_new_session=True)
    except OSError as exc:
        raise ProvError(f"cannot run fetch-images.sh: {exc}") from None
    out: dict = {}
    assert proc.stdout is not None

    def drain() -> None:
        for line in proc.stdout:
            line = line.rstrip("\n")
            print(line, flush=True)
            text = line.strip()
            if (m := _MANIFEST_LINE.fullmatch(text)):
                out["digest"] = m.group(1)
            elif (m := _STAGED_LINE.fullmatch(text)):
                swap["staged"] = m.group(1)
            elif (m := _PREVIOUS_LINE.fullmatch(text)):
                swap["previous"] = m.group(1)
            elif text == "swapped":
                swap["swapped"] = True

    def kill() -> None:
        try:
            os.killpg(proc.pid, signal.SIGKILL)
        except OSError:
            pass
        drain()                       # learn whether the swap happened before the kill
        proc.wait()

    try:
        drain()
        rc = proc.wait(timeout=FETCH_TIMEOUT)
    except subprocess.TimeoutExpired:
        kill()
        raise ProvError("fetch-images.sh timed out") from None
    except BaseException:
        kill()
        raise
    digest = out.get("digest")
    if rc != 0:
        if swap.get("swapped"):
            raise ProvError(f"fetch-images.sh failed after the swap (exit {rc})")
        raise ProvError(f"fetch-images.sh failed (exit {rc}); the previous image (if any) is untouched")
    if not (swap.get("swapped") and swap.get("staged") and swap.get("previous")):
        raise ProvError("fetch-images.sh did not report the staged/previous version dirs")
    if not digest:
        raise ProvError("fetch-images.sh reported no manifest sha256")
    return digest


def preflight(row: dict, version: str) -> None:
    """Render this row's menu entry with the candidate values BEFORE downloading."""
    probe = dict(row, version=version, sha256="0" * 64, fetched_at="pending")
    try:
        netbootlib.render_boot_menu([probe], http_base=provlib.remote_env()["HTTP_BASE"])
    except ValueError as exc:
        raise ProvError(f"image {row['id']} cannot be served by the boot menu: {exc}") from None


class Terminated(Exception):
    """SIGTERM from the queue consumer: unwind (roll back, close the run)."""


def _sigterm(signum, frame):  # noqa: ARG001
    raise Terminated("terminated by SIGTERM")


def _lock(conn, name: str) -> None:
    with conn.cursor() as cur:
        cur.execute("SELECT GET_LOCK(%s, 10) AS got", (name,))
        got = (cur.fetchone() or {}).get("got")
    conn.rollback()
    if got != 1:
        raise ProvError("another refresh of this image is running")


def _unlock(conn, name: str) -> None:
    try:
        with conn.cursor() as cur:
            cur.execute("SELECT RELEASE_LOCK(%s)", (name,))
            cur.fetchall()
        conn.rollback()
    except Exception:  # noqa: BLE001 — the lock also dies with the session
        pass


def supersede(conn, image_id: int, new_run: int) -> None:
    """Cancel older non-terminal refresh runs of this image: we hold the image
    lock, so whoever opened them is gone and they would stay 'running' forever."""
    with conn.cursor() as cur:
        cur.execute("SELECT id FROM prov_runs WHERE mode='refresh' AND image_id=%s AND id<>%s"
                    " AND status IN ('queued','running') ORDER BY id", (image_id, new_run))
        stale = [int(r["id"]) for r in cur.fetchall()]
    conn.rollback()
    for rid in stale:
        try:
            provlib.advance_run(conn, rid, "cancelled", f"superseded by run {new_run}")
            log(f"superseded older refresh run {rid}")
        except ProvError as exc:
            log(f"warning: could not supersede run {rid}: {exc}")


def finish(conn, run_id: int, status: str, detail: str) -> None:
    """Make the run terminal; on a broken session, retry on a fresh connection."""
    for attempt in (0, 1):
        try:
            if attempt:
                conn = provlib.connect_sor()
            else:
                conn.rollback()
            provlib.advance_run(conn, run_id, status, detail)
            return
        except Exception as exc:  # noqa: BLE001
            if attempt:
                print(f"error: could not mark run {run_id} {status}: {exc}", file=sys.stderr)
        finally:
            if attempt:
                try:
                    conn.close()
                except Exception:  # noqa: BLE001
                    pass


def undo(conn, row: dict, swap: dict, digest: str | None, recorded: bool) -> str:
    """Put the previous image back after a post-swap failure; returns the
    suffix for the run detail."""
    problems = []
    if recorded and digest:
        try:
            restore(conn, row, digest)
            log(f"restored image {row['id']} SoR row")
        except Exception as exc:  # noqa: BLE001
            problems.append(f"SoR restore: {exc}")
    if swap.get("staged") and swap.get("previous"):
        try:
            fetch_op(row, "--rollback", swap["previous"], swap["staged"])
            log(f"rolled back to {swap['previous']}")
        except Exception as exc:  # noqa: BLE001
            # Killed between staging and the swap report: if the swap never
            # happened the rollback is refused and there is nothing to undo.
            if swap.get("swapped"):
                problems.append(f"files: {exc}")
    elif swap.get("swapped"):
        problems.append("files: version dirs unknown")
    if problems:
        return "rollback FAILED: " + "; ".join(problems)
    return "old image kept" if swap.get("previous") not in (None, "-") else "no image installed"


def refresh(conn, row: dict) -> str:
    """Steps 2-7; returns the success detail. On a post-swap failure the swap
    is undone and ProvError carries the reason + rollback outcome."""
    require_ipxe(row)
    provlib.require_store(("installers",))   # fail closed: never fetch onto the local disk
    log("image store mounted and linked")
    today = dt.datetime.now(dt.timezone.utc).date()
    plan = build_plan(row, today)
    preflight(row, plan["version"])
    for name, url, sha in plan["files"]:
        log(f"plan {name} <- {url} sha256={sha}")
    swap: dict = {}
    digest = None
    recorded = False
    try:
        digest = run_fetch(row, plan, swap)
        record(conn, row, plan["version"], digest)
        recorded = True
        log(f"recorded image {row['id']}: version={plan['version']} manifest sha256={digest}")
        n = regenerate_menu(conn)
        log(f"boot menu regenerated with {n} fetched image(s)")
    except BaseException as exc:
        if not (swap.get("swapped") or swap.get("staged")):
            raise
        reason = str(exc) or type(exc).__name__
        if not isinstance(exc, (ProvError, Terminated)):
            reason = f"internal error: {type(exc).__name__}"
        signal.signal(signal.SIGTERM, signal.SIG_IGN)   # finish the undo + close the run
        outcome = undo(conn, row, swap, digest, recorded)
        log(f"refresh failed after the swap ({reason}); {outcome}")
        raise ProvError(f"{reason}; {outcome}") from None
    detail = f"installed {plan['version']} (manifest {digest[:12]})"
    try:
        fetch_op(row, "--prune", swap["staged"])
    except ProvError as exc:
        log(f"warning: {exc}")
        detail += "; old version not pruned"
    return detail


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(prog="image-refresh.py",
                                 description="Re-fetch + verify one iPXE netboot image (IMAGE_REFRESH).")
    ap.add_argument("--image", required=True, help="prov_images.id")
    ap.add_argument("--op", required=True, help="operator principal")
    args = ap.parse_args(argv)
    try:
        image_id = provlib.parse_id(args.image, "image")
        op = provlib.parse_principal(args.op)
    except ProvError as exc:
        return provlib.die(str(exc), 2)

    signal.signal(signal.SIGTERM, _sigterm)
    conn = None
    run_id = None
    lock = REFRESH_LOCK.format(image_id)
    try:
        conn = provlib.connect_sor()
        row = load_row(conn, image_id)
        log(f"op={op} image={image_id} {row['distro']} {row['release_name']} {row['arch']}")
        _lock(conn, lock)
        run_id = provlib.open_run(conn, mode="refresh", entity_id=None, image_id=image_id,
                                  stack_order=[], requested_by=op, detail="fetching")
        log(f"opened prov_runs {run_id}")
        supersede(conn, image_id, run_id)
        detail = refresh(conn, row)
        finish(conn, run_id, "succeeded", detail)
        log(f"run {run_id} succeeded: {detail}")
    except (ProvError, Terminated) as exc:
        if run_id is not None:
            finish(conn, run_id, "failed", str(exc))
        return provlib.die(str(exc))
    except BaseException as exc:  # never leave the run open
        if run_id is not None:
            finish(conn, run_id, "failed", f"internal error: {type(exc).__name__}")
        raise
    finally:
        if conn is not None:
            _unlock(conn, lock)
            try:
                conn.close()
            except Exception:  # noqa: BLE001
                pass
    return 0


if __name__ == "__main__":
    sys.exit(main())
