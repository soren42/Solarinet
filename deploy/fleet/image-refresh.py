#!/usr/bin/env python3
"""image-refresh.py — the IMAGE_REFRESH helper: re-fetch one netboot image.

    image-refresh.py --image <id> --op <principal>

Exec'd (no shell) by solariServer's queue consumer for the operator-gated
IMAGE_REFRESH verb; safe to run by hand. Exit 0 ok, 1 failure/refusal, 2 usage.

1. Validate (provlib), connect to the SoR, load the LIVE prov_images row.
   Only boot_method='ipxe' rows are refreshed here: Pi-native netboot and
   write-to-media images are not served by the iPXE menu.
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
   and atomically swaps it into installers/<distro>/<release>/<arch>.
5. UPDATE prov_images SET version, sha256, fetched_at=NOW(). sha256 is the
   sha256 of that image's SHA256SUMS.solari manifest (one CHAR(64) for a
   multi-file image). version = the upstream point release where the names
   carry it (Ubuntu), else `fetched-YYYYMMDD` (UTC) — Debian's SHA256SUMS and
   openSUSE's CHECKSUMS carry no build id, and no extra request is made for one.
6. Regenerate boot.ipxe from every live, fetched iPXE row (netbootlib) and
   write it to HTTP_ROOT/ipxe/boot.ipxe on benzene (provlib.remote_write).

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

_CK_LINE = re.compile(r"([0-9a-fA-F]{64}) [ *](\S.*)")
_UBU_TARBALL = re.compile(r"ubuntu-([0-9]+\.[0-9]+(?:\.[0-9]+)*)-netboot-([a-z0-9]+)\.tar\.gz")
_MANIFEST_LINE = re.compile(r"manifest-sha256 ([0-9a-f]{64})")

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
    if row["boot_method"] != "ipxe":
        raise ProvError(f"image {image_id} ({row['distro']} {row['release_name']} {row['arch']}) boots via "
                        f"{row['boot_method']}; only iPXE images are fetched for the boot menu "
                        "(Pi netboot / media images are built by the pi/ tooling)")
    return row


def record(conn, row: dict, version: str, sha256: str) -> None:
    with conn.cursor() as cur:
        cur.execute(
            "UPDATE prov_images SET version=%s, sha256=%s, fetched_at=NOW()"
            " WHERE id=%s AND deleted_at IS NULL AND boot_method='ipxe'"
            " AND distro=%s AND release_name=%s AND arch=%s"
            " AND source_url <=> %s AND checksum_url <=> %s",
            (version, sha256, row["id"], row["distro"], row["release_name"], row["arch"],
             row["source_url"], row["checksum_url"]))
        n = cur.rowcount
    if n != 1:
        conn.rollback()
        raise ProvError(f"image {row['id']} was edited or deleted during the refresh; files are on "
                        "benzene but the SoR row was not updated — re-run IMAGE_REFRESH")
    conn.commit()


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
            cur.execute("SELECT RELEASE_LOCK(%s)", (MENU_LOCK,))
            cur.fetchall()
            conn.rollback()
    return len(rows)


# ---------------------------------------------------------------------------
# Fetch
# ---------------------------------------------------------------------------
def run_fetch(row: dict, plan: dict) -> str:
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
        proc = subprocess.Popen(argv, stdout=subprocess.PIPE, text=True)
    except OSError as exc:
        raise ProvError(f"cannot run fetch-images.sh: {exc}") from None
    digest = None
    assert proc.stdout is not None
    for line in proc.stdout:
        line = line.rstrip("\n")
        print(line, flush=True)
        m = _MANIFEST_LINE.fullmatch(line.strip())
        if m:
            digest = m.group(1)
    try:
        rc = proc.wait(timeout=FETCH_TIMEOUT)
    except subprocess.TimeoutExpired:
        proc.kill()
        raise ProvError("fetch-images.sh timed out") from None
    if rc != 0:
        raise ProvError(f"fetch-images.sh failed (exit {rc}); the previous image (if any) is untouched")
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

    conn = None
    try:
        conn = provlib.connect_sor()
        row = load_row(conn, image_id)
        log(f"op={op} image={image_id} {row['distro']} {row['release_name']} {row['arch']}")
        today = dt.datetime.now(dt.timezone.utc).date()
        plan = build_plan(row, today)
        preflight(row, plan["version"])
        for name, url, sha in plan["files"]:
            log(f"plan {name} <- {url} sha256={sha}")
        digest = run_fetch(row, plan)
        record(conn, row, plan["version"], digest)
        log(f"recorded image {image_id}: version={plan['version']} manifest sha256={digest}")
        n = regenerate_menu(conn)
        log(f"boot menu regenerated with {n} fetched image(s)")
    except ProvError as exc:
        return provlib.die(str(exc))
    finally:
        if conn is not None:
            try:
                conn.close()
            except Exception:  # noqa: BLE001
                pass
    return 0


if __name__ == "__main__":
    sys.exit(main())
