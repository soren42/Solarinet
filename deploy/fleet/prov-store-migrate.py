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
      re-run (resumable). A destination with different content is a CONFLICT. Relative symlinks that stay inside
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

Order (--apply): BOTH subtrees are planned first, and any conflict or refused
entry in either stops the run before anything is copied or swapped. Then both
are copied, both verified, and only then both swapped, so a failure while
copying or verifying swaps nothing. A failure during the swap phase itself
(rename errors) says exactly which subtree was swapped and which was not.

Preflight (all modes): <store> must be a mounted <fstype> filesystem
(findmnt, after touching the path to trigger the automount), else exit 1
"provisioning image store not mounted" — the whole point is never to copy
onto the local disk under an unmounted mountpoint. With --apply, a self-test
in a scratch dir in the store first proves the operations fetch-images.sh and
this script rely on: create/read a symlink (CIFS needs mfsymlinks), rename a
new symlink OVER an existing one (`mv -T`), rename a directory, chmod, flock,
mkdtemp.

Store maintenance lock (--apply and --purge --apply): .prov-store-migrate.lock
beside the web root is taken EXCLUSIVELY (non-blocking) before planning and
held through the swap. Every helper that writes to the store (fetch-images.sh,
fleet-provision.sh's store write, build-pi-image.sh; image-refresh.py and
prov-stage.py through them) takes it SHARED via prov-store-guard.sh, so a
refresh, stage or image build in progress makes the migrate refuse, and none
can start while it runs. Every fetch-images lock file found in the tree is
also held (non-blocking), for helpers from releases before the shared lock.

Hung NAS: the touch that triggers the automount and findmnt run in a child
process bounded by PROV_STORE_TIMEOUT seconds (default 20); a store that does
not answer is "not responding", exit 1, never a hang.

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
def store_timeout() -> int:
    raw = os.environ.get("PROV_STORE_TIMEOUT", "20")
    if not raw.isdigit() or not 1 <= int(raw) <= 600:
        raise MigrateError(f"PROV_STORE_TIMEOUT {raw!r} is not a number of seconds (1-600)")
    return int(raw)


def bounded(argv: list[str], timeout: int, what: str) -> tuple[int, str]:
    """Run argv in a child, never blocking past timeout (+5 s to reap a kill).

    A child stuck in the kernel on a hard-mounted hung share may not even die
    on SIGKILL; it is then abandoned rather than waited for."""
    try:
        proc = subprocess.Popen(argv, stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
                                stderr=subprocess.DEVNULL, text=True, start_new_session=True)
    except OSError as exc:
        raise MigrateError(f"cannot run {argv[0]} to {what}: {exc}") from None
    try:
        out, _ = proc.communicate(timeout=timeout)
    except subprocess.TimeoutExpired:
        proc.kill()
        try:
            proc.communicate(timeout=5)
        except subprocess.TimeoutExpired:
            pass
        raise MigrateError(f"provisioning image store not responding: {what} timed out after "
                           f"{timeout}s (NAS hung or unreachable); refusing") from None
    return proc.returncode, out


def require_mounted(store: str, fstype: str) -> None:
    timeout = store_timeout()
    # trigger the systemd automount; the exit status does not matter, a hang does
    bounded(["ls", "--", store + "/"], timeout, f"touching {store}")
    if fstype == "none":
        if not os.path.isdir(store):
            raise MigrateError(f"provisioning image store not mounted: {store} is not a directory")
        return
    _rc, out = bounded(["findmnt", "-n", "-o", "SOURCE", "-t", fstype, "-M", store], timeout,
                       f"findmnt on {store}")
    out = out.strip()
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


class Sub:
    """One subtree's preflight result: kind is linked | missing | tree."""

    def __init__(self, sub: str, http_root: str, store: str) -> None:
        self.sub = sub
        self.src = os.path.join(http_root, sub)
        self.dst = os.path.join(store, sub)
        self.kind = ""
        self.plan: Plan | None = None
        self.backup = ""


def prepare_sub(sub: str, http_root: str, store: str, *, apply: bool) -> Sub:
    """Classify + plan one subtree; reads only. Conflicts are collected, not raised."""
    s = Sub(sub, http_root, store)
    real_dst = os.path.join(os.path.realpath(store), sub)
    if os.path.islink(s.src):
        if os.path.realpath(s.src) == real_dst and os.path.isdir(real_dst):
            log(f"{sub}: already linked -> {s.dst}")
            s.kind = "linked"
            return s
        raise MigrateError(f"{s.src} is a symlink to {os.readlink(s.src)}, not into the store; "
                           "fix it by hand")
    if not os.path.lexists(s.src):
        log(f"{sub}: no local tree; {'creating' if apply else 'would create'} {s.dst} + link")
        s.kind = "missing"
        return s
    if not os.path.isdir(s.src):
        raise MigrateError(f"{s.src} is not a directory")
    if os.path.lexists(s.dst) and (os.path.islink(s.dst) or not os.path.isdir(s.dst)):
        raise MigrateError(f"{s.dst} exists and is not a directory")
    s.kind = "tree"
    s.plan = plan = plan_tree(s.src, s.dst, hash_sources=apply)
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
    return s


def preflight(http_root: str, store: str, *, apply: bool) -> list[Sub]:
    """Plan EVERY subtree before anything is copied or swapped; one conflict or
    refusal in any of them stops the whole run."""
    subs = [prepare_sub(sub, http_root, store, apply=apply) for sub in SUBS]
    bad = [(s.sub, len(s.plan.conflicts), len(s.plan.refused)) for s in subs
           if s.plan and (s.plan.conflicts or s.plan.refused)]
    if bad:
        detail = ", ".join(f"{sub}: {c} conflict(s), {r} refused" for sub, c, r in bad)
        raise MigrateError(f"{detail}; nothing copied, nothing swapped (neither "
                           f"{' nor '.join(SUBS)}) — resolve them by hand and re-run")
    total = sum(n for s in subs if s.plan for _, n in s.plan.copy)
    free = shutil.disk_usage(store).free if os.path.isdir(store) else 0
    if total > free:
        raise MigrateError(f"needs {human(total)} but the store has {human(free)} free; "
                           "nothing copied, nothing swapped")
    return subs


def outcome(subs: list[Sub], swapped: list[Sub]) -> str:
    todo = [s for s in subs if s.kind != "linked"]
    if not swapped:
        return ("nothing swapped: " + ", ".join(f"{s.src} is unchanged" for s in todo)
                + " (store copies kept; a re-run resumes)")
    rest = [s for s in todo if s not in swapped]
    done = ", ".join(f"{s.sub} -> {s.dst}" + (f" (original kept at {s.backup})" if s.backup else "")
                     for s in swapped)
    left = ", ".join(f"{s.sub} (still the local directory {s.src})" for s in rest) or "none"
    return f"PARTIAL: swapped {done}; NOT swapped: {left}; re-run to finish"


def apply_subs(subs: list[Sub], http_root: str) -> None:
    """Copy all, verify all, then swap all. Any failure names what was swapped."""
    todo = [s for s in subs if s.kind != "linked"]
    held = hold_locks([lk for s in todo if s.plan for lk in s.plan.locks])
    swapped: list[Sub] = []
    try:
        for s in todo:
            os.makedirs(s.dst, exist_ok=True)
            if s.plan is None:
                continue
            for rel in s.plan.dirs:
                os.makedirs(os.path.join(s.dst, rel), exist_ok=True)
            for i, (rel, n) in enumerate(s.plan.copy, 1):
                log(f"  [{i}/{len(s.plan.copy)}] copying {s.sub}/{rel} ({human(n)})")
                copy_verified(os.path.join(s.src, rel), os.path.join(s.dst, rel),
                              s.plan.src_state[rel][2])
            for rel, target in s.plan.links:
                os.symlink(target, os.path.join(s.dst, rel))
        for s in todo:
            if s.plan is not None:
                verify_unchanged(s.sub, s.src, s.dst, s.plan)
        stamp = time.strftime("%Y%m%dT%H%M%SZ", time.gmtime())
        for s in todo:
            if s.plan is not None:
                s.backup = os.path.join(backup_dir(http_root), f".{s.sub}.pre-store.{stamp}")
            swap_in(s.src, s.dst, backup=s.backup or None)
            swapped.append(s)
    except (MigrateError, OSError) as exc:
        raise MigrateError(f"{exc}; {outcome(subs, swapped)}") from None
    finally:
        for fh in held:
            fh.close()


def take_maintenance_lock(http_root: str) -> int:
    """The store maintenance lock, EXCLUSIVE, non-blocking; held (fd kept open)
    until the process exits. Beside the web root, not in it (nginx would list
    it). Opened read-only (+create, 0644) so a lock file created by another
    user (root's image build) still works."""
    path = os.path.join(os.path.dirname(http_root), ".prov-store-migrate.lock")
    fd = os.open(path, os.O_RDONLY | os.O_CREAT | os.O_NOFOLLOW | os.O_CLOEXEC, 0o644)
    try:
        fcntl.flock(fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
    except OSError:
        os.close(fd)
        raise MigrateError(f"the image store maintenance lock {path} is held: another prov-store-migrate, "
                           "or an image refresh / stage / image build, is running; retry when it "
                           "has finished") from None
    return fd


def backup_dir(http_root: str) -> str:
    """Originals are kept BESIDE the web root (same local filesystem, so the
    move is a rename) where nginx does not serve them."""
    return os.path.dirname(http_root)


def verify_unchanged(sub: str, src: str, dst: str, plan: Plan) -> None:
    again = plan_tree(src, dst, hash_sources=False)
    if set(again.src_state) != set(plan.src_state):
        raise MigrateError(f"{sub}: files were added or removed during the copy")
    for rel, (size, mtime, _sha) in plan.src_state.items():
        size2, mtime2, _ = again.src_state[rel]
        if (size2, mtime2) != (size, mtime):
            raise MigrateError(f"{sub}/{rel} changed during the copy")
    if again.copy or again.conflicts or again.refused or again.links or again.dirs:
        raise MigrateError(f"{sub}: the store copy is incomplete after copying")
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
    if os.path.abspath(store).startswith(os.path.abspath(http_root) + "/"):
        ap.error("the store may not live inside the HTTP root")
    if not os.path.isdir(http_root):
        ap.error(f"--http-root {http_root} is not a directory")
    lock_fd = None
    try:
        require_mounted(store, a.fstype)    # bounded: before anything else touches the store
        if os.path.realpath(store).startswith(os.path.realpath(http_root) + "/"):
            ap.error("the store may not live inside the HTTP root")
        if a.selftest_only:
            selftest(store)
            return 0
        if a.apply:
            lock_fd = take_maintenance_lock(http_root)   # held through the swap, until return
        if a.purge:
            purge(http_root, store, apply=a.apply)
            return 0
        if a.apply:
            selftest(store)
        else:
            log("dry run (pass --apply to copy and swap); the store self-test runs with --apply")
        subs = preflight(http_root, store, apply=a.apply)
        if not a.apply:
            for s in subs:
                if s.kind == "tree":
                    log(f"{s.sub}: dry run — would copy, verify, keep the original as "
                        f"{backup_dir(http_root)}/.{s.sub}.pre-store.<stamp> and link {s.src} -> {s.dst}")
            log("all done" if all(s.kind == "linked" for s in subs)
                else "dry run complete; nothing blocks --apply")
            return 0
        apply_subs(subs, http_root)
        log("all done")
        return 0
    except MigrateError as exc:
        print(f"[store-migrate][error] {exc}", file=sys.stderr, flush=True)
        return 1
    except OSError as exc:
        print(f"[store-migrate][error] {exc}", file=sys.stderr, flush=True)
        return 1
    finally:
        if lock_fd is not None:
            os.close(lock_fd)


if __name__ == "__main__":
    sys.exit(main())
