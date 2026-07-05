#!/usr/bin/env bash
# =============================================================================
# fetch-images.sh - download netboot installer kernels/initrds to benzene.
#
# Go-live PREP (network-heavy but non-destructive): populates
#   /srv/solari-provision/netboot/http/installers/<distro>/<arch>/
# with the kernel + initrd the iPXE per-MAC scripts chainload. Safe to run any
# time before flipping the PXE switch. Idempotent (skips files already present).
#
#   deploy/fleet/fetch-images.sh [--distro debian|ubuntu|opensuse|all] [--arch x86_64|arm64|all]
#
# Runs the downloads ON benzene (close to the HTTP root, no double transfer).
# =============================================================================
set -euo pipefail
. "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/fleet-lib.sh"

WANT_DISTRO="all" WANT_ARCH="all"
while [ $# -gt 0 ]; do
  case "$1" in
    --distro) WANT_DISTRO="${2:?}"; shift ;;
    --arch)   WANT_ARCH="${2:?}"; shift ;;
    *) fdie "unknown arg: $1" ;;
  esac; shift
done

# Netboot kernel/initrd sources. amd64 links are stable distro mirrors; arm64
# netboot install (non-Pi) is added as those images are curated. Pi uses images
# (see fleet-image.sh / pi/), not these installers.
want() { { [ "$WANT_DISTRO" = all ] || [ "$WANT_DISTRO" = "$1" ]; } && { [ "$WANT_ARCH" = all ] || [ "$WANT_ARCH" = "$2" ]; }; }

fetch_one() { # distro arch  kurl iurl  kname iname
  local d="$1" a="$2" kurl="$3" iurl="$4" kname="$5" iname="$6"
  local dir="${HTTP_ROOT}/installers/${d}/${a}"
  flog "fetching ${d}/${a} netboot kernel+initrd"
  bssh "set -e; install -d '${dir}'
    cd '${dir}'
    [ -s '${kname}' ] || curl -fL --retry 3 -o '${kname}' '${kurl}'
    [ -s '${iname}' ] || curl -fL --retry 3 -o '${iname}' '${iurl}'
    ls -lh '${dir}'" || fwarn "fetch failed for ${d}/${a} (URL may have moved; update fetch-images.sh)"
}

DEB=https://deb.debian.org/debian/dists/bookworm/main/installer-amd64/current/images/netboot/debian-installer/amd64
# Ubuntu renames the netboot tarball on every point release (24.04 -> 24.04.N),
# so resolve the newest matching name from the release index at run time.
UBU_BASE=https://releases.ubuntu.com/24.04
UBU_FILE="$(curl -fsL "${UBU_BASE}/" | grep -oE 'ubuntu-24\.04[0-9.]*-netboot-amd64\.tar\.gz' | sort -uV | tail -1)"
UBU="${UBU_BASE}/${UBU_FILE:-ubuntu-24.04-netboot-amd64.tar.gz}"    # note: tarball; see below
SUSE=https://download.opensuse.org/distribution/leap/15.6/repo/oss/boot/x86_64/loader

want debian   x86_64 && fetch_one debian   x86_64 "${DEB}/linux"  "${DEB}/initrd.gz" linux initrd.gz
want opensuse x86_64 && fetch_one opensuse x86_64 "${SUSE}/linux" "${SUSE}/initrd"   linux initrd

# Ubuntu 24.04 ships a netboot tarball rather than loose kernel/initrd; unpack it.
if want ubuntu x86_64; then
  flog "fetching ubuntu/x86_64 netboot tarball"
  bssh "set -e; d='${HTTP_ROOT}/installers/ubuntu/x86_64'; install -d \"\$d\"; cd \"\$d\"
    [ -s vmlinuz ] || { curl -fL --retry 3 -o nb.tgz '${UBU}' && tar xzf nb.tgz && \
      { find . -name vmlinuz -exec cp {} vmlinuz \\; ; [ -s vmlinuz ] || find . -name linux -path '*/amd64/*' -exec cp {} vmlinuz \\; ; } && \
      find . -name initrd -path '*/amd64/*' -exec cp {} initrd \\; ; }
    ls -lh \"\$d\" 2>/dev/null | head" || fwarn "ubuntu netboot fetch/unpack failed; update the URL in fetch-images.sh"
fi

flog "done. installer trees under ${HTTP_ROOT}/installers/ on benzene."
