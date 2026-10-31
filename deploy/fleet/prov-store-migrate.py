#!/usr/bin/env python3
"""prov-store-migrate — move the netboot installers/ and images/ trees from
benzene's local disk into the NAS image store, then link them back.

Run ON the provisioning host (benzene), by hand, once per host; stdlib only
(no provlib: it is not installed there). Nothing is changed unless --apply is
given:

  prov-store-migrate.py                     dry run: report what would happen
  prov-store-migrate.py --apply             copy, verify, swap in the links
  prov-store-migrate.py --purge             dry run of the purge below
  prov-store-migrate.py --purge --apply     delete the kept originals, after
                                            re-verifying each file against the store
  prov-store-migrate.py --selftest-only     only check the store's filesystem semantics

For each of installers/ and images/ under HTTP_ROOT:

  * already a link into the store       -> nothing to do;
  * missing                              -> create <store>/<sub> and the link;
  * a real directory                     -> merge-copy it into <store>/<sub>:
      every regular file is copied to a ".<name>.prov-migrate.part" temp name,
      flushed, read BACK from the store and its sha256 compared with the
      source's before it is renamed into place. A destination that already
      has the same size + sha256 is skipped, so an interrupted run is simply
      re-run (resumable). A destination with different content is a CONFLICT:
      nothing is swapped and the run fails. Relative symlinks that stay inside
      the tree (fetch-images' <arch> -> .<arch>.v.XXXXXXXX) are recreated;
      absolute or escaping links and special files are refused.
    After the copy every source file is re-checked (size + mtime unchanged
    since it was hashed: nobody wrote to it meanwhile) and the destination
    tree re-listed. Only then is the original renamed to
    <HTTP_ROOT's parent>/.<sub>.pre-store.<UTC stamp> (beside the web root,
    so not served; KEPT, deleted only by --purge --apply) and an
    absolute symlink HTTP_ROOT/<sub> -> <store>/<sub> renamed into its place.
    Between those two renames (microseconds) a client fetching a kernel gets a
    404; run it with no netboot install in progress.

Preflight (all modes): <store> must be a mounted <fstype> filesystem
(findmnt, after touching the path to trigger the automount), else exit 1
"provisioning image store not mounted" — the whole point is never to copy
onto the local disk under an unmounted mountpoint. With --apply, a self-test
in a scratch dir in the store first proves the operations fetch-images.sh and
this script rely on: create/read a symlink (CIFS needs mfsymlinks), rename a
new symlink OVER an existing one (`mv -T`), rename a directory, chmod, flock,
mkdtemp. While copying, every fetch-images lock file found in the tree is
held (non-blocking: a refresh in progress aborts the run).

--fstype none skips the mount check and is accepted only with
SOLARI_PROV_LOCAL=1 (tests). Defaults come from the same environment names
as the helpers: PROV_ROOT (/srv/solari-provision), HTTP_ROOT
(PROV_ROOT/netboot/http), PROV_STORE (PROV_ROOT/store), PROV_STORE_FSTYPE
(cifs).

A dry run reads only: it compares by size (no hashing), writes nothing, and
does not run the self-test.

Exit: 0 ok (dry run: nothing blocks an --apply), 1 failure / conflict /
store not mounted, 2 usage error.
"""
from __future__ import annotations

import argparse
import fcntl
import hashlib
import os
import re
import shutil
import stat
import subprocess
import sys
import tempfile
import time

SUBS = ("installers", "images")
PATH_RE = re.compile(r"^/[A-Za-z0-9._/-]+$")
FSTYPE_RE = re.compile(r"^[a-z0-9.]{1,16}$")
PART = ".prov-migrate.part"
CHUNK = 4 << 20


class MigrateError(Exception):
    pass


def log(msg: str) -> None:
    print(f"[store-migrate] {msg}", flush=True)


def sha256_file(path: str, *, drop_cache: bool = False) -> str:
    h = hashlib.sha256()
    with open(path, "rb") as fh:
        if drop_cache and hasattr(os, "posix_fadvise"):
            try:   # read what the NAS holds, not our own write-back cache
                os.posix_fadvise(fh.fileno(), 0, 0, os.POSIX_FADV_DONTNEED)
            except OSError:
                pass
        for chunk in iter(lambda: fh.read(CHUNK), b""):
            h.update(chunk)
    return h.hexdigest()


def human(n: int) -> str:
    for unit in ("B", "KiB", "MiB", "GiB", "TiB"):
        if n < 1024 or unit == "TiB":
            return f"{n:.1f} {unit}" if unit != "B" else f"{n} B"
        n /= 1024
    return str(n)


# ---- preflight -------------------------------------------------------------
def require_mounted(store: str, fstype: str) -> None:
    try:
        os.listdir(store)          # trigger the systemd automount
    except OSError:
        pass
    if fstype == "none":
        if not os.path.isdir(store):
            raise MigrateError(f"provisioning image store not mounted: {store} is not a directory")
        return
    try:
        out = subprocess.run(["findmnt", "-n", "-o", "SOURCE", "-t", fstype, "-M", store],
                             capture_output=True, text=True, timeout=30).stdout.strip()
    except (OSError, subprocess.TimeoutExpired) as exc:
        raise MigrateError(f"cannot run findmnt: {exc}") from None
    if not out:
        raise MigrateError(f"provisioning image store not mounted: {store} is not a {fstype} "
                           "mount (check the NAS, /etc/fstab and solari-prov-store-mount.service); "
                           "refusing to copy onto the local disk underneath")
    log(f"store {store} mounted from {out.splitlines()[0]} ({fstype})")


def selftest(store: str) -> None:
    """Prove the store does what fetch-images.sh and the swap below need."""
    d = tempfile.mkdtemp(prefix=".prov-migrate-selftest.", dir=store)
    try:
        f = os.path.join(d, "f")
        with open(f, "wb") as fh:
            fh.write(b"solari\n")
        os.chmod(f, 0o644)
        with open(f, "rb") as fh:
            fcntl.flock(fh.fileno(), fcntl.LOCK_EX | fcntl.LOCK_NB)
            fcntl.flock(fh.fileno(), fcntl.LOCK_UN)
        os.mkdir(os.path.join(d, ".v1"))
        os.mkdir(os.path.join(d, ".v2"))
        link = os.path.join(d, "link")
        os.symlink(".v1", link)
        if not stat.S_ISLNK(os.lstat(link).st_mode) or os.readlink(link) != ".v1":
            raise MigrateError("symlinks do not survive on the store (CIFS needs mfsymlinks)")
        if not os.path.isdir(link):
            raise MigrateError("a symlink on the store does not resolve")
        os.symlink(".v2", link + ".new")
        os.replace(link + ".new", link)              # mv -T: rename over a symlink
        if os.readlink(link) != ".v2":
            raise MigrateError("renaming a symlink over another did not replace it")
        os.rename(os.path.join(d, ".v1"), os.path.join(d, ".v1.moved"))
        tempfile.mkdtemp(prefix=".x.", dir=d)
        with open(f, "rb") as fh:
            if fh.read() != b"solari\n":
                raise MigrateError("file content did not round-trip")
    except OSError as exc:
        raise MigrateError(f"store self-test failed: {exc}") from None
    finally:
        shutil.rmtree(d, ignore_errors=True)
    log("store self-test passed (symlink, rename-over-symlink, rename dir, chmod, flock, mkdtemp)")


# ---- planning --------------------------------------------------------------
class Plan:
    def __init__(self) -> None:
        self.dirs: list[str] = []                         # relative dirs to create
        self.copy: list[tuple[str, int]] = []             # (rel, size) to copy
        self.same: list[str] = []                         # already identical in the store
        self.links: list[tuple[str, str]] = []            # (rel, target) to recreate
        self.conflicts: list[str] = []
        self.refused: list[str] = []
        self.src_state: dict[str, tuple[int, int, str]] = {}   # rel -> (size, mtime_ns, sha)
        self.locks: list[str] = []


def _inside(rel: str, target: str) -> bool:
    joined = os.path.normpath(os.path.join(os.path.dirname(rel), target))
    return not (os.path.isabs(target) or joined == ".." or joined.startswith("../"))


def plan_tree(src: str, dst: str, *, hash_sources: bool) -> Plan:
    p = Plan()
    for top, dirnames, filenames in os.walk(src, followlinks=False):
        rtop = os.path.relpath(top, src)
        rtop = "" if rtop == "." else rtop
        for name in sorted(dirnames + filenames):
            rel = os.path.join(rtop, name)
            s = os.path.join(src, rel)
            d = os.path.join(dst, rel)
            st = os.lstat(s)
            if stat.S_ISLNK(st.st_mode):
                target = os.readlink(s)
                if not _inside(rel, target):
                    p.refused.append(f"{rel}: symlink to {target} leaves the tree")
                elif os.path.islink(d):
                    if os.readlink(d) != target:
                        p.conflicts.append(f"{rel}: store has link -> {os.readlink(d)}, source -> {target}")
                elif os.path.lexists(d):
                    p.conflicts.append(f"{rel}: store has a non-link where the source has a link")
                else:
                    p.links.append((rel, target))
                if name in dirnames:
                    dirnames.remove(name)                  # never descend through a link
            elif stat.S_ISDIR(st.st_mode):
                if os.path.lexists(d) and (os.path.islink(d) or not os.path.isdir(d)):
                    p.conflicts.append(f"{rel}/: store has a non-directory there")
                elif not os.path.isdir(d):
                    p.dirs.append(rel)
            elif stat.S_ISREG(st.st_mode):
                if name.startswith(".") and name.endswith(".lock"):
                    p.locks.append(s)
                if name.endswith(PART):
                    p.refused.append(f"{rel}: looks like a leftover migration temp file in the SOURCE")
                    continue
                sha = sha256_file(s) if hash_sources else ""
                p.src_state[rel] = (st.st_size, st.st_mtime_ns, sha)
                if os.path.islink(d) or (os.path.lexists(d) and not os.path.isfile(d)):
                    p.conflicts.append(f"{rel}: store has a non-file there")
                elif os.path.isfile(d):
                    if os.path.getsize(d) == st.st_size and (
                            not hash_sources or sha256_file(d) == sha):
                        p.same.append(rel)
                    else:
                        p.conflicts.append(f"{rel}: the store already has a DIFFERENT file "
                                           "(size or sha256 differs)")
                else:
                    p.copy.append((rel, st.st_size))
            else:
                p.refused.append(f"{rel}: special file (not a file, dir or symlink)")
    return p


# ---- doing -----------------------------------------------------------------
def copy_verified(src: str, dst: str, sha: str) -> None:
    part = os.path.join(os.path.dirname(dst), "." + os.path.basename(dst) + PART)
    h = hashlib.sha256()
    with open(src, "rb") as fi, open(part, "wb") as fo:
        for chunk in iter(lambda: fi.read(CHUNK), b""):
            h.update(chunk)
            fo.write(chunk)
        fo.flush()
        os.fsync(fo.fileno())
    if h.hexdigest() != sha:
        os.unlink(part)
        raise MigrateError(f"{src} changed while it was being copied")
    got = sha256_file(part, drop_cache=True)
    if got != sha:
        os.unlink(part)
        raise MigrateError(f"read-back sha256 mismatch for {dst}: expected {sha}, got {got}")
    st = os.stat(src)
    try:
        os.chmod(part, stat.S_IMODE(st.st_mode) & 0o755 | 0o444)
        os.utime(part, ns=(st.st_atime_ns, st.st_mtime_ns))
    except OSError:
        pass                      # CIFS file_mode/dir_mode decide; HTTP only needs 0644
    os.replace(part, dst)


def hold_locks(paths: list[str]) -> list:
    held = []
    for path in paths:
        fh = open(path, "rb")
        try:
            fcntl.flock(fh.fileno(), fcntl.LOCK_EX | fcntl.LOCK_NB)
        except OSError:
            fh.close()
            for h in held:
                h.close()
            raise MigrateError(f"{path} is locked: an image refresh is running; retry later") from None
        held.append(fh)
    return held


def migrate_sub(sub: str, http_root: str, store: str, *, apply: bool) -> bool:
    """Returns True when the sub needs (or got) no further work; raises on failure."""
    src = os.path.join(http_root, sub)
    dst = os.path.join(store, sub)
    real_dst = os.path.join(os.path.realpath(store), sub)
    if os.path.islink(src):
        if os.path.realpath(src) == real_dst and os.path.isdir(real_dst):
            log(f"{sub}: already linked -> {dst}")
            return True
        raise MigrateError(f"{src} is a symlink to {os.readlink(src)}, not into the store; "
                           "fix it by hand")
    if not os.path.lexists(src):
        log(f"{sub}: no local tree; {'creating' if apply else 'would create'} {dst} + link")
        if apply:
            os.makedirs(dst, exist_ok=True)
            swap_in(src, dst, backup=None)
        return True
    if not os.path.isdir(src):
        raise MigrateError(f"{src} is not a directory")
    if os.path.lexists(dst) and (os.path.islink(dst) or not os.path.isdir(dst)):
        raise MigrateError(f"{dst} exists and is not a directory")

    plan = plan_tree(src, dst, hash_sources=apply)
    total = sum(n for _, n in plan.copy)
    log(f"{sub}: {len(plan.src_state)} file(s), {len(plan.copy)} to copy ({human(total)}), "
        f"{len(plan.same)} already in the store, {len(plan.links)} link(s), "
        f"{len(plan.dirs)} dir(s) to create")
    if not apply:
        for rel, n in plan.copy:
            log(f"  would copy {sub}/{rel} ({human(n)})")
    for msg in plan.conflicts:
        log(f"  CONFLICT {sub}/{msg}")
    for msg in plan.refused:
        log(f"  REFUSED {sub}/{msg}")
    if plan.conflicts or plan.refused:
        raise MigrateError(f"{sub}: {len(plan.conflicts)} conflict(s), {len(plan.refused)} refused "
                           "entr(y/ies); nothing swapped — resolve them by hand and re-run")
    free = shutil.disk_usage(store).free if os.path.isdir(store) else 0
    if total > free:
        raise MigrateError(f"{sub}: needs {human(total)} but the store has {human(free)} free")
    if not apply:
        log(f"{sub}: dry run — would copy, verify, keep the original as "
            f"{backup_dir(http_root)}/.{sub}.pre-store.<stamp> and link {src} -> {dst}")
        return False

    held = hold_locks(plan.locks)
    try:
        os.makedirs(dst, exist_ok=True)
        for rel in plan.dirs:
            os.makedirs(os.path.join(dst, rel), exist_ok=True)
        for i, (rel, n) in enumerate(plan.copy, 1):
            log(f"  [{i}/{len(plan.copy)}] copying {sub}/{rel} ({human(n)})")
            copy_verified(os.path.join(src, rel), os.path.join(dst, rel), plan.src_state[rel][2])
        for rel, target in plan.links:
            os.symlink(target, os.path.join(dst, rel))
        verify_unchanged(sub, src, dst, plan)
        stamp = time.strftime("%Y%m%dT%H%M%SZ", time.gmtime())
        swap_in(src, dst, backup=os.path.join(backup_dir(http_root), f".{sub}.pre-store.{stamp}"))
    finally:
        for fh in held:
            fh.close()
    return True


def backup_dir(http_root: str) -> str:
    """Originals are kept BESIDE the web root (same local filesystem, so the
    move is a rename) where nginx does not serve them."""
    return os.path.dirname(http_root)


def verify_unchanged(sub: str, src: str, dst: str, plan: Plan) -> None:
    again = plan_tree(src, dst, hash_sources=False)
    if set(again.src_state) != set(plan.src_state):
        raise MigrateError(f"{sub}: files were added or removed during the copy; nothing swapped, re-run")
    for rel, (size, mtime, _sha) in plan.src_state.items():
        size2, mtime2, _ = again.src_state[rel]
        if (size2, mtime2) != (size, mtime):
            raise MigrateError(f"{sub}/{rel} changed during the copy; nothing swapped, re-run")
    if again.copy or again.conflicts or again.refused or again.links or again.dirs:
        raise MigrateError(f"{sub}: the store copy is incomplete after copying; nothing swapped")
    log(f"{sub}: verified {len(plan.src_state)} file(s) in the store (sha256 read-back + unchanged source)")


def swap_in(src: str, dst: str, *, backup: str | None) -> None:
    tmp = f"{src}.prov-migrate-link.{os.getpid()}"
    if os.path.lexists(tmp):
        os.unlink(tmp)
    os.symlink(dst, tmp)
    if backup is not None:
        os.rename(src, backup)
    try:
        os.replace(tmp, src)
    except OSError:
        if backup is not None:
            os.rename(backup, src)
        os.unlink(tmp)
        raise
    if backup is not None:
        log(f"swapped {src} -> {dst}; original kept at {backup} (remove with --purge)")
    else:
        log(f"linked {src} -> {dst}")


def purge(http_root: str, store: str, *, apply: bool) -> None:
    for sub in SUBS:
        src = os.path.join(http_root, sub)
        real_dst = os.path.join(os.path.realpath(store), sub)
        bdir = backup_dir(http_root)
        backups = sorted(e for e in os.listdir(bdir) if e.startswith(f".{sub}.pre-store."))
        if not backups:
            continue
        if not (os.path.islink(src) and os.path.realpath(src) == real_dst):
            raise MigrateError(f"{src} is not linked into the store; refusing to purge its backups")
        for b in backups:
            bpath = os.path.join(bdir, b)
            plan = plan_tree(bpath, real_dst, hash_sources=True)
            if plan.copy or plan.conflicts or plan.refused or plan.links or plan.dirs:
                raise MigrateError(f"{b}: not every file is identical in the store "
                                   f"({len(plan.copy)} missing, {len(plan.conflicts)} differ); kept")
            if apply:
                shutil.rmtree(bpath)
                log(f"purged {b} ({len(plan.same)} file(s) verified identical in the store)")
            else:
                log(f"would purge {b} ({len(plan.same)} file(s) verified identical in the store)")


def main(argv: list[str] | None = None) -> int:
    env = os.environ
    prov_root = env.get("PROV_ROOT", "/srv/solari-provision")
    ap = argparse.ArgumentParser(description="Move netboot installers/ + images/ into the NAS image store.")
    ap.add_argument("--http-root", default=env.get("HTTP_ROOT", f"{prov_root}/netboot/http"))
    ap.add_argument("--store", default=env.get("PROV_STORE", f"{prov_root}/store"))
    ap.add_argument("--fstype", default=env.get("PROV_STORE_FSTYPE", "cifs"))
    ap.add_argument("--apply", action="store_true", help="actually copy and swap (default: dry run)")
    ap.add_argument("--purge", action="store_true",
                    help="delete kept .<sub>.pre-store.* originals after re-verifying them "
                         "(dry run unless --apply)")
    ap.add_argument("--selftest-only", action="store_true", help="only run the store self-test")
    a = ap.parse_args(argv)
    http_root, store = a.http_root.rstrip("/"), a.store.rstrip("/")
    for name, path in (("--http-root", http_root), ("--store", store)):
        if not PATH_RE.match(path) or "/../" in path + "/":
            ap.error(f"{name} {path!r} is not a plain absolute path")
    if not FSTYPE_RE.match(a.fstype):
        ap.error(f"--fstype {a.fstype!r} is not a filesystem type name")
    if a.fstype == "none" and env.get("SOLARI_PROV_LOCAL") != "1":
        ap.error("--fstype none is only allowed under SOLARI_PROV_LOCAL=1")
    if os.path.realpath(store).startswith(os.path.realpath(http_root) + "/"):
        ap.error("the store may not live inside the HTTP root")
    if not os.path.isdir(http_root):
        ap.error(f"--http-root {http_root} is not a directory")
    try:
        require_mounted(store, a.fstype)
        if a.selftest_only:
            selftest(store)
            return 0
        if a.purge:
            purge(http_root, store, apply=a.apply)
            return 0
        if a.apply:
            # beside the web root, not in it (nginx would list it)
            lock = open(os.path.join(os.path.dirname(http_root), ".prov-store-migrate.lock"), "a+b")
            try:
                fcntl.flock(lock.fileno(), fcntl.LOCK_EX | fcntl.LOCK_NB)
            except OSError:
                raise MigrateError("another prov-store-migrate is running") from None
            selftest(store)
        else:
            log("dry run (pass --apply to copy and swap); the store self-test runs with --apply")
        done = [migrate_sub(sub, http_root, store, apply=a.apply) for sub in SUBS]
        log("all done" if all(done) else "dry run complete; nothing blocks --apply")
        return 0
    except MigrateError as exc:
        print(f"[store-migrate][error] {exc}", file=sys.stderr, flush=True)
        return 1
    except OSError as exc:
        print(f"[store-migrate][error] {exc}", file=sys.stderr, flush=True)
        return 1


if __name__ == "__main__":
    sys.exit(main())
