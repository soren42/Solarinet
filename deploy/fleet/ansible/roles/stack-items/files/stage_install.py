#!/usr/bin/env python3
"""stage_install — copy a build's DESTDIR stage into its prefix, as root,
without trusting anything the build produced.

Every build install phase runs as the unprivileged admin user into
<stage> = <build>/_stage (DESTDIR). This is the ONLY root step of a build.
It never runs build-controlled code; it copies data, defensively:

  * the stage is walked with directory fds and O_NOFOLLOW, never following a
    symlink, and every staged entry must be owned by the (non-root) owner of
    the stage root, so a hardlink to a root-owned file cannot be laundered
    into the prefix;
  * only directories, regular files and symlinks are accepted; devices,
    fifos and sockets are refused, as is anything staged outside
    <stage><prefix> (bare ancestor directories of the prefix excepted);
  * realpath(prefix) is resolved ONCE; every destination path component is
    then opened or created relative to its parent's fd with O_NOFOLLOW, so a
    symlink already present in the destination (prefix/lib -> /etc) is never
    traversed: the install is refused instead;
  * regular files are copied content-only into a new inode (never a
    hardlink), root:root, mode 0755 if any execute bit was staged else 0644
    (setuid/setgid/sticky dropped), written to a temp file in the same
    directory, fsynced and renamed over the destination (atomic replace);
  * staged symlinks are recreated only if their target, resolved lexically
    from the link's destination directory, stays inside the prefix;
  * new directories are root:root 0755; existing ones are left as they are.

Usage: stage_install.py --stage DIR --prefix DIR [--check]
  --check   validate the stage only; the prefix need not exist, nothing is
            written. The role runs this unprivileged before creating the
            prefix, then runs the full install as root (which validates again).

Ownership is set to root:root only when running as root (the role always
runs the install with become); unprivileged test runs keep the caller's uid.
Exit: 0 ok (summary on stdout), 1 refused (reason on stderr), 2 usage.
Stdlib only.
"""
from __future__ import annotations

import argparse
import errno
import json
import os
import secrets
import stat
import sys

TAG = "stack-items"
O_DIR = os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW | os.O_CLOEXEC


class Refused(Exception):
    pass


def _kind(mode: int) -> str:
    for test, name in ((stat.S_ISDIR, "directory"), (stat.S_ISREG, "file"), (stat.S_ISLNK, "symlink"),
                       (stat.S_ISFIFO, "fifo"), (stat.S_ISSOCK, "socket"), (stat.S_ISCHR, "char device"),
                       (stat.S_ISBLK, "block device")):
        if test(mode):
            return name
    return "unknown"


def _within(path: str, root: str) -> bool:
    return path == root or path.startswith(root.rstrip("/") + "/")


def _open_dir(name: str, dir_fd: int | None, what: str) -> int:
    try:
        return os.open(name, O_DIR, dir_fd=dir_fd)
    except OSError as e:
        if e.errno in (errno.ELOOP, errno.ENOTDIR):
            raise Refused(f"{what} is a symlink or not a directory") from None
        raise


class Installer:
    def __init__(self, stage: str, prefix: str, owner_uid: int | None = None):
        for label, p in (("stage", stage), ("prefix", prefix)):
            if not os.path.isabs(p) or os.path.normpath(p) != p.rstrip("/") or p == "/":
                raise Refused(f"{label} must be a normalised absolute path other than '/', got {p!r}")
        self.stage = stage.rstrip("/")
        self.prefix = prefix.rstrip("/")
        self.real_prefix = os.path.realpath(self.prefix)
        if self.real_prefix == "/":
            raise Refused(f"prefix {self.prefix} resolves to '/'")
        self.owner_uid = owner_uid      # None = the stage root's owner
        self.as_root = os.geteuid() == 0
        self.counts = {"dirs_created": 0, "files": 0, "files_replaced": 0, "symlinks": 0,
                       "hardlinked_sources": 0}

    # ---- stage side -------------------------------------------------------
    def _src_stat(self, name: str, dir_fd: int, rel: str) -> os.stat_result:
        st = os.stat(name, dir_fd=dir_fd, follow_symlinks=False)
        self._check_src(st, rel)
        return st

    def _check_src(self, st: os.stat_result, rel: str) -> None:
        kind = _kind(st.st_mode)
        if kind not in ("directory", "file", "symlink"):
            raise Refused(f"refusing staged {kind} {rel} (only directories, regular files and symlinks install)")
        if st.st_uid != self.owner_uid:
            raise Refused(f"refusing staged {kind} {rel}: owned by uid {st.st_uid}, not the build user "
                          f"(uid {self.owner_uid}) that owns the stage")

    def _link_ok(self, target: str, rel_parent: str) -> bool:
        if os.path.isabs(target):
            t = os.path.normpath(target)
            return _within(t, self.real_prefix) or _within(t, self.prefix)
        t = os.path.normpath(os.path.join(self.real_prefix, rel_parent, target))
        return _within(t, self.real_prefix)

    def _open_stage(self) -> int:
        fd = _open_dir(self.stage, None, f"stage {self.stage}")
        st = os.fstat(fd)
        if self.owner_uid is None:
            self.owner_uid = st.st_uid
        if self.owner_uid == 0:
            os.close(fd)
            raise Refused(f"stage {self.stage} is owned by root; installs must be staged by the "
                          "unprivileged build user")
        return fd

    def _empty(self) -> Refused:
        return Refused(f"build staged nothing under {self.prefix}: its install step did not honour "
                       f"DESTDIR ({self.stage}). Builds never run install logic as root; an item that "
                       "needs root-time install logic must be modelled as a `script` item (the "
                       "explicit, reviewed root-code path)")

    def validate(self) -> None:
        """Refuse anything outside <stage><prefix>, then check every entry in it."""
        parts = self.prefix.strip("/").split("/")
        outside: list[str] = []
        fd = self._open_stage()
        try:
            rel = ""
            for i, part in enumerate(parts):
                found = False
                for e in sorted(os.scandir(fd), key=lambda e: e.name):
                    erel = f"{rel}/{e.name}"
                    if e.name == part:
                        found = True
                        continue
                    self._collect_outside(fd, e.name, erel, outside)
                if not found:
                    if outside:
                        break
                    raise self._empty()
                st = os.stat(part, dir_fd=fd, follow_symlinks=False)
                if not stat.S_ISDIR(st.st_mode):
                    raise Refused(f"staged {rel}/{part} is a {_kind(st.st_mode)}, not a directory")
                self._check_src(st, f"{rel}/{part}")
                nfd = _open_dir(part, fd, f"staged {rel}/{part}")
                os.close(fd)
                fd = nfd
                rel = f"{rel}/{part}"
            if outside:
                raise Refused(f"refusing a build that staged files outside its prefix {self.prefix}:\n"
                              + "\n".join(outside))
            if not list(os.scandir(fd)):
                raise self._empty()
            self._walk_check(fd, "")
        finally:
            os.close(fd)

    def _collect_outside(self, dir_fd: int, name: str, rel: str, out: list[str]) -> None:
        st = os.stat(name, dir_fd=dir_fd, follow_symlinks=False)
        if not stat.S_ISDIR(st.st_mode):
            out.append(rel)
            return
        sub = _open_dir(name, dir_fd, f"staged {rel}")
        try:
            for e in sorted(os.scandir(sub), key=lambda e: e.name):
                self._collect_outside(sub, e.name, f"{rel}/{e.name}", out)
        finally:
            os.close(sub)

    def _walk_check(self, dir_fd: int, rel: str) -> None:
        for e in sorted(os.scandir(dir_fd), key=lambda e: e.name):
            erel = f"{rel}/{e.name}"
            st = self._src_stat(e.name, dir_fd, self.prefix + erel)
            if stat.S_ISLNK(st.st_mode):
                target = os.readlink(e.name, dir_fd=dir_fd)
                if not self._link_ok(target, rel.lstrip("/")):
                    raise Refused(f"refusing staged symlink {self.prefix}{erel} -> {target}: "
                                  f"target leaves the prefix {self.prefix}")
            elif stat.S_ISDIR(st.st_mode):
                sub = _open_dir(e.name, dir_fd, f"staged {self.prefix}{erel}")
                try:
                    self._walk_check(sub, erel)
                finally:
                    os.close(sub)

    # ---- destination side ---------------------------------------------------
    def _own(self, fd: int) -> None:
        if self.as_root:
            os.fchown(fd, 0, 0)

    def install(self) -> dict:
        self.validate()
        if not os.path.isabs(self.real_prefix) or not os.path.isdir(self.real_prefix):
            raise Refused(f"prefix {self.prefix} must be an existing directory")
        dst = _open_dir(self.real_prefix, None, f"prefix {self.real_prefix}")
        src = self._open_stage()
        try:
            for part in self.prefix.strip("/").split("/"):
                nfd = _open_dir(part, src, f"staged {part}")
                os.close(src)
                src = nfd
            self._copy_dir(src, dst, "")
        finally:
            os.close(src)
            os.close(dst)
        return self.counts

    def _dst_lstat(self, name: str, dir_fd: int) -> os.stat_result | None:
        try:
            return os.stat(name, dir_fd=dir_fd, follow_symlinks=False)
        except FileNotFoundError:
            return None

    def _copy_dir(self, src_fd: int, dst_fd: int, rel: str) -> None:
        for e in sorted(os.scandir(src_fd), key=lambda e: e.name):
            name, erel = e.name, f"{rel}/{e.name}"
            where = self.real_prefix + erel
            st = self._src_stat(name, src_fd, self.prefix + erel)
            cur = self._dst_lstat(name, dst_fd)
            if stat.S_ISDIR(st.st_mode):
                if cur is None:
                    os.mkdir(name, 0o700, dir_fd=dst_fd)
                    sub_dst = _open_dir(name, dst_fd, f"destination {where}")
                    self._own(sub_dst)
                    os.fchmod(sub_dst, 0o755)
                    self.counts["dirs_created"] += 1
                elif stat.S_ISLNK(cur.st_mode):
                    raise Refused(f"refusing to install through destination symlink {where}")
                elif not stat.S_ISDIR(cur.st_mode):
                    raise Refused(f"destination {where} is a {_kind(cur.st_mode)}, staged a directory")
                else:
                    sub_dst = _open_dir(name, dst_fd, f"destination {where}")
                try:
                    sub_src = _open_dir(name, src_fd, f"staged {self.prefix}{erel}")
                    try:
                        self._copy_dir(sub_src, sub_dst, erel)
                    finally:
                        os.close(sub_src)
                    os.fsync(sub_dst)
                finally:
                    os.close(sub_dst)
                continue
            if cur is not None and stat.S_ISDIR(cur.st_mode):
                raise Refused(f"destination {where} is a directory, staged a {_kind(st.st_mode)}")
            tmp = f".stage-install.{secrets.token_hex(8)}"
            try:
                if stat.S_ISLNK(st.st_mode):
                    target = os.readlink(name, dir_fd=src_fd)
                    if not self._link_ok(target, rel.lstrip("/")):
                        raise Refused(f"refusing staged symlink {self.prefix}{erel} -> {target}: "
                                      f"target leaves the prefix {self.prefix}")
                    os.symlink(target, tmp, dir_fd=dst_fd)
                    if self.as_root:
                        os.chown(tmp, 0, 0, dir_fd=dst_fd, follow_symlinks=False)
                    self.counts["symlinks"] += 1
                else:
                    self._copy_file(name, src_fd, tmp, dst_fd, self.prefix + erel)
                os.rename(tmp, name, src_dir_fd=dst_fd, dst_dir_fd=dst_fd)
                if cur is not None and stat.S_ISREG(st.st_mode):
                    self.counts["files_replaced"] += 1
            except BaseException:
                try:
                    os.unlink(tmp, dir_fd=dst_fd)
                except FileNotFoundError:
                    pass
                raise

    def _copy_file(self, name: str, src_fd: int, tmp: str, dst_fd: int, rel: str) -> None:
        # O_NONBLOCK: a fifo swapped in after validation must not hang root.
        fi = os.open(name, os.O_RDONLY | os.O_NOFOLLOW | os.O_NONBLOCK | os.O_CLOEXEC, dir_fd=src_fd)
        try:
            st = os.fstat(fi)
            self._check_src(st, rel)          # re-check the inode actually opened
            if st.st_nlink > 1:
                self.counts["hardlinked_sources"] += 1
            mode = 0o755 if st.st_mode & 0o111 else 0o644
            fo = os.open(tmp, os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_NOFOLLOW | os.O_CLOEXEC, 0o600,
                         dir_fd=dst_fd)
            try:
                while True:
                    buf = os.read(fi, 1 << 20)
                    if not buf:
                        break
                    view = memoryview(buf)
                    while view:
                        view = view[os.write(fo, view):]
                self._own(fo)
                os.fchmod(fo, mode)           # after chown, which may clear mode bits
                os.fsync(fo)
            finally:
                os.close(fo)
        finally:
            os.close(fi)
        self.counts["files"] += 1


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--stage", required=True)
    ap.add_argument("--prefix", required=True)
    ap.add_argument("--check", action="store_true", help="validate the stage only; write nothing")
    args = ap.parse_args(argv)
    try:
        inst = Installer(args.stage, args.prefix)
        if args.check:
            inst.validate()
            print(json.dumps({"checked": args.stage + args.prefix}))
        else:
            counts = inst.install()
            print(json.dumps({"installed": args.prefix, **counts}, sort_keys=True))
    except Refused as e:
        print(f"{TAG}: {e}", file=sys.stderr)
        return 1
    except OSError as e:
        print(f"{TAG}: stage install failed: {e}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
