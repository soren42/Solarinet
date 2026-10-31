#!/usr/bin/env python3
"""netbootlib — iPXE boot menu, per-MAC profile and image paths for netboot.

One module owns every path and every line of iPXE script the provisioning
host serves, so the boot menu (image-refresh.py), the per-MAC profile
(fleet-provision.sh, prov-stage.py) and the fetcher (fetch-images.sh) can never
disagree about where an image lives.

Layout under HTTP_ROOT (release-aware, so Debian 12 and 13 or Leap and
Tumbleweed coexist; arch is the prov_images vocabulary — x86_64, arm64 — and is
translated to the distro's own name, e.g. amd64 / aarch64, only inside
upstream URLs):

    installers/<distro>/<release_name>/<arch>/
        <kernel> <initrd>        the boot artifacts actually served
        SHA256SUMS.solari        sha256sum-format manifest of those artifacts;
                                 prov_images.sha256 = sha256 of THIS file
        solari-image.json        row snapshot (distro, release, label, arch,
                                 version, source_url, manifest_sha256,
                                 fetched_at) for callers without SoR access

iPXE script is a command language: `${…}` expands, whitespace splits
arguments and `||`/`&&` chain commands. Every value interpolated here is
therefore checked against a strict pattern first (distro and arch against
fixed tables, release/version/label/hostname/MAC/URLs against narrow
charsets with no `$`, `{`, `}`, quotes, `;`, `|`, `&`, `#` or whitespace
beyond the label's single spaces). Free-text columns such as prov_images.notes
are never rendered. Anything that fails raises ValueError — fail closed.

Pure functions, stdlib only, deterministic output (golden-tested by
tests/deploy/test_image_refresh.py). The only file read is the per-MAC
template next to this module.

CLI (for shell callers):
    netbootlib.py bootstrap-menu                  # static zero-image boot.ipxe (@@HTTP_BASE@@ token)
    netbootlib.py image-dir --distro D --release R --arch A
    netbootlib.py mac-profile --image-json FILE|- --distro D --release R --arch A
                              --mac M --hostname H --http-base URL
"""
from __future__ import annotations

import argparse
import json
import os
import re
import sys
from typing import Any, Iterable

IMAGE_MANIFEST = "SHA256SUMS.solari"
IMAGE_META = "solari-image.json"
BOOTSTRAP_BASE = "@@HTTP_BASE@@"

# Same order as the prov_images.arch ENUM (stable sort key).
ARCH_ORDER = ("x86_64", "arm64", "armhf", "riscv64", "i386")

# distro handle -> display title, prov arch -> upstream arch name, served files.
# Only combinations with a real netboot installer upstream are listed; anything
# else is refused rather than guessed.
DISTROS: dict[str, dict[str, Any]] = {
    "debian": {"title": "Debian", "arches": {"x86_64": "amd64", "arm64": "arm64"},
               "kernel": "linux", "initrd": "initrd.gz"},
    "opensuse": {"title": "openSUSE", "arches": {"x86_64": "x86_64", "arm64": "aarch64"},
                 "kernel": "linux", "initrd": "initrd"},
    "ubuntu": {"title": "Ubuntu", "arches": {"x86_64": "amd64", "arm64": "arm64"},
               "kernel": "linux", "initrd": "initrd"},
}

RELEASE_RE = re.compile(r"[A-Za-z0-9][A-Za-z0-9._-]{0,47}")
VERSION_RE = re.compile(r"[A-Za-z0-9][A-Za-z0-9._+~-]{0,63}")
LABEL_RE = re.compile(r"[A-Za-z0-9][A-Za-z0-9 ._()+,:/-]{0,95}")
HOSTNAME_RE = re.compile(r"[A-Za-z0-9](?:[A-Za-z0-9-]{0,61}[A-Za-z0-9])?")
MAC_RE = re.compile(r"[0-9a-f]{2}(?:-[0-9a-f]{2}){5}")
SHA256_RE = re.compile(r"[0-9a-f]{64}")
# Base URL of the provisioning host's HTTP root: no trailing slash.
HTTP_BASE_RE = re.compile(r"https?://[A-Za-z0-9.-]+(?::[0-9]{1,5})?(?:/[A-Za-z0-9._~-]+)*")
# Upstream directory URL (always ends in '/'); also used for derived file URLs.
UPSTREAM_DIR_RE = re.compile(r"https?://[A-Za-z0-9.-]+(?::[0-9]{1,5})?/(?:[A-Za-z0-9._~+-]+/)*")
UPSTREAM_FILE_RE = re.compile(r"https?://[A-Za-z0-9.-]+(?::[0-9]{1,5})?/(?:[A-Za-z0-9._~+-]+/)*[A-Za-z0-9._~+-]+")
UBUNTU_POINT_RE = re.compile(r"[0-9]+\.[0-9]+(?:\.[0-9]+)*")

_TEMPLATE = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                         "netboot", "ipxe", "mac-profile.ipxe.tmpl")


# ---------------------------------------------------------------------------
# Validation helpers
# ---------------------------------------------------------------------------
def _match(rx: re.Pattern, value: Any, what: str) -> str:
    if not isinstance(value, str) or not rx.fullmatch(value):
        raise ValueError(f"unsafe or invalid {what}: {value!r}")
    return value


def _spec(row: dict) -> dict[str, Any]:
    distro = row.get("distro")
    if distro not in DISTROS:
        raise ValueError(f"unsupported netboot distro: {distro!r}")
    return DISTROS[distro]


def upstream_arch(distro: str, arch: str) -> str:
    """prov_images arch (x86_64, arm64, …) -> the distro's name for it (amd64, aarch64, …)."""
    spec = _spec({"distro": distro})
    if arch not in spec["arches"]:
        raise ValueError(f"no {distro} netboot installer for arch {arch!r}")
    return spec["arches"][arch]


def _ident(row: dict) -> tuple[str, str, str]:
    distro = row.get("distro")
    _spec(row)
    release = _match(RELEASE_RE, row.get("release_name"), "release_name")
    arch = row.get("arch")
    upstream_arch(distro, arch)
    return distro, release, arch


def is_fetched(row: dict) -> bool:
    """True for a live iPXE row that a refresh has fetched and verified."""
    return (row.get("boot_method") == "ipxe" and not row.get("deleted_at")
            and bool(row.get("sha256")) and bool(row.get("fetched_at")))


def normalize_mac(mac: str) -> str:
    """aa:bb:…, AA-BB-…, aabbcc… -> aa-bb-cc-dd-ee-ff (iPXE ${net0/mac:hexhyp})."""
    if not isinstance(mac, str):
        raise ValueError("mac must be a string")
    raw = mac.strip().lower()
    if not re.fullmatch(r"(?:[0-9a-f]{2}[:-]?){5}[0-9a-f]{2}", raw):
        raise ValueError(f"invalid MAC address: {mac!r}")
    hexs = raw.replace(":", "").replace("-", "")
    return _match(MAC_RE, "-".join(hexs[i:i + 2] for i in range(0, 12, 2)), "mac")


# ---------------------------------------------------------------------------
# Paths
# ---------------------------------------------------------------------------
def image_dir(row: dict) -> str:
    """Directory of an image relative to HTTP_ROOT: installers/<distro>/<release>/<arch>."""
    distro, release, arch = _ident(row)
    return f"installers/{distro}/{release}/{arch}"


def boot_files(row: dict) -> tuple[str, str]:
    """(kernel, initrd) file names served from image_dir(row)."""
    spec = _spec(row)
    return spec["kernel"], spec["initrd"]


def suse_install_repo(row: dict) -> str:
    """openSUSE install= repo: the UPSTREAM repo root derived from source_url.

    source_url names the loader directory (…/repo/oss/boot/<arch>/loader/);
    stripping `boot/<arch>/loader/` gives the repo linuxrc installs from. The
    target therefore needs outbound HTTP(S) to that mirror (documented in
    netboot/README.md); only the kernel + initrd are served locally.
    """
    if row.get("distro") != "opensuse":
        raise ValueError("install repo applies to opensuse rows only")
    uarch = upstream_arch("opensuse", row.get("arch"))
    src = _match(UPSTREAM_DIR_RE, row.get("source_url"), "source_url")
    suffix = f"boot/{uarch}/loader/"
    if not src.endswith(suffix) or len(src) == len(suffix):
        raise ValueError(f"opensuse source_url must end in {suffix}: {src!r}")
    return _match(UPSTREAM_DIR_RE, src[: -len(suffix)], "opensuse install repo")


def ubuntu_iso_url(row: dict) -> str:
    """Ubuntu live-server ISO the netboot initrd loads (iso-url=), from source_url + version.

    The 24.04 netboot tarball's own pxelinux config boots with
    `root=/dev/ram0 ramdisk_size=1500000 ip=dhcp iso-url=<live-server ISO>`;
    without iso-url the installer has nothing to install from.
    """
    if row.get("distro") != "ubuntu":
        raise ValueError("iso-url applies to ubuntu rows only")
    uarch = upstream_arch("ubuntu", row.get("arch"))
    src = _match(UPSTREAM_DIR_RE, row.get("source_url"), "source_url")
    version = _match(UBUNTU_POINT_RE, row.get("version"), "ubuntu version")
    release = _match(RELEASE_RE, row.get("release_name"), "release_name")
    if version != release and not version.startswith(release + "."):
        raise ValueError(f"ubuntu version {version!r} is not a {release} point release")
    return _match(UPSTREAM_FILE_RE, f"{src}ubuntu-{version}-live-server-{uarch}.iso", "ubuntu iso url")


# ---------------------------------------------------------------------------
# iPXE text
# ---------------------------------------------------------------------------
def _checked_fetched(row: dict) -> dict:
    if row.get("boot_method") != "ipxe":
        raise ValueError(f"image {row.get('id', '?')} boots via {row.get('boot_method')!r}, not iPXE")
    if not is_fetched(row):
        raise ValueError(f"image {row.get('id', '?')} has not been fetched and verified (run IMAGE_REFRESH)")
    _match(SHA256_RE, row.get("sha256"), "sha256")
    _ident(row)
    _match(VERSION_RE, row.get("version"), "version")
    label = row.get("release_label")
    if label is not None and label != "":
        _match(LABEL_RE, label, "release_label")
    return row


def item_id(row: dict) -> str:
    distro, release, arch = _ident(row)
    return f"{distro}-{release}-{arch}"


def item_title(row: dict) -> str:
    """Menu text built only from validated pieces (never notes)."""
    distro, release, arch = _ident(row)
    label = row.get("release_label") or release
    _match(LABEL_RE, label, "release_label")
    version = _match(VERSION_RE, row.get("version"), "version")
    return f"{DISTROS[distro]['title']} {label} - {arch} - {version}"


def _boot_lines(row: dict, fail: str) -> list[str]:
    """kernel/initrd/boot for one fetched row; ${base}/${cfgkey}/${cmdline_*} set by the caller."""
    distro = row["distro"]
    kernel, initrd = boot_files(row)
    lines = [f"set installer ${{base}}/{image_dir(row)}"]
    if distro == "debian":
        lines.append(f"kernel ${{installer}}/{kernel} ${{cmdline_common}} ${{debian_cmdline}}"
                     f" preseed/url=${{base}}/configs/${{cfgkey}}.preseed || goto {fail}")
    elif distro == "ubuntu":
        # 's=' MUST end with a slash: subiquity fetches user-data + meta-data from it.
        lines.append(f"kernel ${{installer}}/{kernel} ${{cmdline_common}} root=/dev/ram0"
                     f" ramdisk_size=1500000 ip=dhcp iso-url={ubuntu_iso_url(row)} autoinstall"
                     f" \"ds=nocloud-net;s=${{base}}/configs/ubuntu/${{cfgkey}}/\" || goto {fail}")
    elif distro == "opensuse":
        # install= is the UPSTREAM repo (not mirrored locally); autoyast= our control file.
        lines.append(f"kernel ${{installer}}/{kernel} ${{cmdline_common}}"
                     f" install={suse_install_repo(row)}"
                     f" autoyast=${{base}}/configs/${{cfgkey}}.xml || goto {fail}")
    else:  # pragma: no cover — _spec() already refused it
        raise ValueError(f"unsupported netboot distro: {distro!r}")
    lines.append(f"initrd ${{installer}}/{initrd} || goto {fail}")
    lines.append(f"boot || goto {fail}")
    return lines


_HEADER = """\
#!ipxe
# =============================================================================
# SolariNet Fleet Provisioning - top-level iPXE boot script
# =============================================================================
# GENERATED by deploy/fleet/netbootlib.py render_boot_menu() from the live,
# fetched + verified prov_images rows (IMAGE_REFRESH rewrites it). Do not edit
# on benzene. The copy in the repo (deploy/fleet/netboot/ipxe/boot.ipxe) is the
# zero-image bootstrap: render_boot_menu([]) with an HTTP_BASE placeholder.
#
# Served at <HTTP_BASE>/ipxe/boot.ipxe. Firmware PXE loads an iPXE binary from
# benzene's TFTP root (its embedded script chains here); see netboot/README.md.
#
#   STAGE 1  chain ipxe/mac-<aa-bb-cc-dd-ee-ff>.ipxe (a staged unattended
#            install); a 404 = nothing staged -> fall through, never a wipe.
#   STAGE 2  menu of the fetched + verified images only.
#   STAGE 3  safe default: boot the local disk (menu timeout / any failure).
# =============================================================================

set base {base}

# Shared kernel command-line fragments (in scope for every label below).
#   cmdline_common : installer output to the VGA console AND serial ttyS0.
#   debian_cmdline : debian-installer non-interactive knobs.
set cmdline_common console=tty0 console=ttyS0,115200n8
set debian_cmdline auto=true priority=critical locale=en_US.UTF-8 keymap=us

# Unattended configs are keyed on the booting NIC (aa-bb-cc-dd-ee-ff). Set
# before the menu so every menu target sees it.
set cfgkey ${{net0/mac:hexhyp}}

echo
echo ================================================================
echo  SolariNet Fleet Provisioning
echo  host mac : ${{net0/mac}}
echo  client   : ${{ip}} / ${{netmask}}  via ${{gateway}}
echo  server   : ${{base}}
echo ================================================================
echo

# =============================================================================
# STAGE 1 - per-MAC staged install (written by fleet-provision.sh / PROV_STAGE)
# =============================================================================
echo Checking for a staged install profile for ${{net0/mac}} ...
chain --autofree ${{base}}/ipxe/mac-${{net0/mac:hexhyp}}.ipxe || goto menu
goto menu

# =============================================================================
# STAGE 2 - interactive menu (operator at the console)
# =============================================================================
:menu
menu SolariNet Provisioning - select an action
"""

_FOOTER = """\
item --gap --                   Utilities:
item shell           iPXE interactive shell
item reboot          Reboot
item --gap --                   Safe default:
item local           Boot from local disk (default)
choose --default local --timeout 15000 target && goto ${target} || goto local
{targets}
# =============================================================================
# Utility labels
# =============================================================================
:shell
echo Dropping to iPXE shell. Type 'exit' to return to the menu.
shell
goto menu

:reboot
reboot

:retry
echo
echo Boot step FAILED. Returning to menu in 10s ...
sleep 10
goto menu

# =============================================================================
# STAGE 3 - safe default: boot from local disk
# =============================================================================
# 'sanboot --drive 0x80' boots the first BIOS disk; 'exit 1' hands control
# back to UEFI firmware for its NVRAM boot entries.
:local
echo Booting from local disk ...
sanboot --no-describe --drive 0x80 || exit 1
"""


def _natural(s: str) -> tuple:
    return tuple((0, int(p), "") if p.isdigit() else (1, 0, p) for p in re.split(r"([0-9]+)", s) if p)


def _menu(rows: Iterable[dict], base: str) -> str:
    fetched = [_checked_fetched(r) for r in rows if is_fetched(r)]
    fetched.sort(key=lambda r: (r["distro"], _natural(r["release_name"]), ARCH_ORDER.index(r["arch"])))
    seen: set[str] = set()
    items: list[str] = []
    targets: list[str] = []
    last_distro = None
    for row in fetched:
        iid = item_id(row)
        if iid in seen:
            raise ValueError(f"duplicate image {iid} in boot menu rows")
        seen.add(iid)
        if row["distro"] != last_distro:
            items.append(f"item --gap --                     {DISTROS[row['distro']]['title']}")
            last_distro = row["distro"]
        items.append(f"item {iid} {item_title(row)}")
        targets.append("")
        targets.append(f":{iid}")
        targets.extend(_boot_lines(row, "retry"))
    if items:
        items.insert(0, "item --gap --                   Unattended installs (WIPES the target disk):")
    else:
        items.append("item --gap --                   No installer images fetched yet (IMAGE_REFRESH).")
    target_text = "\n".join(targets) + "\n" if targets else ""
    if target_text:
        target_text = ("\n# Menu targets: one label per fetched image (generated).\n"
                       + target_text.lstrip("\n"))
    return (_HEADER.format(base=base) + "\n".join(items) + "\n"
            + _FOOTER.replace("{targets}", target_text))


def render_boot_menu(rows: list[dict], *, http_base: str) -> str:
    """Complete boot.ipxe: stage-1 per-MAC chain, a menu of ONLY the fetched
    iPXE rows (grouped distro -> release -> arch), shell/reboot/local disk.

    Rows that are not live + iPXE + fetched (sha256 and fetched_at set) are
    left out; a fetched row with any unsafe value raises ValueError.
    """
    _match(HTTP_BASE_RE, http_base, "http_base")
    return _menu(rows, http_base)


def bootstrap_menu() -> str:
    """The repo's static boot.ipxe: render_boot_menu([]) with the @@HTTP_BASE@@ token."""
    return _menu([], BOOTSTRAP_BASE)


def _template() -> str:
    with open(_TEMPLATE, encoding="utf-8") as fh:
        return fh.read()


def render_mac_profile(row: dict, *, mac: str, hostname: str, http_base: str) -> str:
    """ipxe/mac-<mac>.ipxe for one fetched iPXE image (same paths as the menu)."""
    _checked_fetched(row)
    mac_h = normalize_mac(mac)
    host = _match(HOSTNAME_RE, hostname, "hostname")
    base = _match(HTTP_BASE_RE, http_base, "http_base")
    out = _template()
    for token, value in (("HTTP_BASE", base), ("MAC", mac_h), ("HOSTNAME", host),
                         ("IMAGE_TITLE", item_title(row)),
                         ("BOOT_BLOCK", "\n".join(_boot_lines(row, "giveup")))):
        out = out.replace(f"@@{token}@@", value)
    if "@@" in out:
        raise ValueError("mac-profile template has an unrendered token")
    return out


def row_from_meta(meta: Any, *, distro: str, release: str, arch: str) -> dict:
    """Rebuild a fetched row from an image dir's solari-image.json, checking identity."""
    if not isinstance(meta, dict):
        raise ValueError(f"{IMAGE_META} is not a JSON object")
    keys = ("distro", "release_name", "release_label", "arch", "version", "source_url",
            "manifest_sha256", "fetched_at")
    vals = {k: meta.get(k) for k in keys}
    if any(v is not None and not isinstance(v, str) for v in vals.values()):
        raise ValueError(f"{IMAGE_META} values must be strings")
    if (vals["distro"], vals["release_name"], vals["arch"]) != (distro, release, arch):
        raise ValueError(f"{IMAGE_META} describes {vals['distro']}/{vals['release_name']}/"
                         f"{vals['arch']}, not {distro}/{release}/{arch}")
    return {"distro": distro, "release_name": release, "arch": arch,
            "release_label": vals["release_label"] or None, "version": vals["version"],
            "source_url": vals["source_url"], "boot_method": "ipxe",
            "sha256": vals["manifest_sha256"], "fetched_at": vals["fetched_at"]}


# ---------------------------------------------------------------------------
# CLI
# ---------------------------------------------------------------------------
def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(prog="netbootlib.py", description=__doc__.split("\n\n")[0])
    sub = ap.add_subparsers(dest="cmd", required=True)
    sub.add_parser("bootstrap-menu", help="print the static zero-image boot.ipxe")
    p_dir = sub.add_parser("image-dir", help="print installers/<distro>/<release>/<arch>")
    p_mac = sub.add_parser("mac-profile", help="render ipxe/mac-<mac>.ipxe from solari-image.json")
    for p in (p_dir, p_mac):
        p.add_argument("--distro", required=True)
        p.add_argument("--release", required=True)
        p.add_argument("--arch", required=True)
    p_mac.add_argument("--image-json", required=True, help="path to solari-image.json, or - for stdin")
    p_mac.add_argument("--mac", required=True)
    p_mac.add_argument("--hostname", required=True)
    p_mac.add_argument("--http-base", required=True)
    args = ap.parse_args(argv)
    try:
        if args.cmd == "bootstrap-menu":
            sys.stdout.write(bootstrap_menu())
        elif args.cmd == "image-dir":
            print(image_dir({"distro": args.distro, "release_name": args.release, "arch": args.arch}))
        else:
            if args.image_json == "-":
                raw = sys.stdin.read(65536)
            else:
                with open(args.image_json, encoding="utf-8") as fh:
                    raw = fh.read(65536)
            try:
                meta = json.loads(raw)
            except ValueError:
                raise ValueError(f"{IMAGE_META} is not valid JSON") from None
            row = row_from_meta(meta, distro=args.distro, release=args.release, arch=args.arch)
            sys.stdout.write(render_mac_profile(row, mac=args.mac, hostname=args.hostname,
                                                http_base=args.http_base))
    except (ValueError, OSError) as exc:
        print(f"netbootlib: {exc}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
