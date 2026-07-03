#!/usr/bin/env bash
# =============================================================================
# build-outpost.sh - build a PORTABLE solariMonitor outpost bundle for an arch.
#
# Produces deploy/dist/<arch>/solariMonitor plus a matched, TLS-enabled
# libnng.so.1 next to it. Some distros (openSUSE, appliances) ship an nng whose
# TLS transport is ABI-incompatible with our build, so the outpost carries its
# own libnng and LD_LIBRARY_PATHs it (deploy/deploy-monitor.sh wires this up).
# The native build happens inside a debian:bookworm container (glibc floor +
# TLS-enabled libnng), emulated for foreign arches via qemu/binfmt.
#
#   deploy/build-outpost.sh [arch]        arch: x86_64 (default) | arm64 | arm32
#   (arm64 covers Pi Zero 2 W / Pi 3/4/5-class aarch64 outposts.)
# =============================================================================
set -euo pipefail
cd "$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

ARCH="${1:-x86_64}"
case "${ARCH}" in
  x86_64) PLAT="linux/amd64" ;;
  arm64)  PLAT="linux/arm64" ;;
  arm32)  PLAT="linux/arm/v7" ;;
  *) echo "unsupported arch: ${ARCH}" >&2; exit 1 ;;
esac
DOCKER="docker"; docker info >/dev/null 2>&1 || DOCKER="sudo docker"
echo "[outpost] building portable solariMonitor + libnng for ${ARCH} (${PLAT})"

${DOCKER} run --rm --platform "${PLAT}" -v "$(pwd):/src" -w /src debian:bookworm-slim bash -c '
  set -e; export DEBIAN_FRONTEND=noninteractive
  apt-get update -y >/dev/null 2>&1
  apt-get install -y --no-install-recommends build-essential cmake pkg-config \
    ca-certificates libnng-dev libmbedtls-dev libsqlite3-dev libcjson-dev binutils >/dev/null 2>&1
  rm -rf build-outpost-'"${ARCH}"'
  cmake -S . -B build-outpost-'"${ARCH}"' \
    -DSOLARI_BUILD_CLIENT=OFF -DSOLARI_BUILD_MONITOR=ON -DSOLARI_BUILD_SERVER=OFF \
    -DSOLARI_BUILD_TESTS=OFF -DSOLARI_WITH_IO=ON -DSOLARI_WITH_SQLITE=ON >/dev/null 2>&1
  cmake --build build-outpost-'"${ARCH}"' -j"$(nproc)" --target solariMonitor \
    2>&1 | grep -iE "error|Built target solariMonitor" | tail -2
  install -D build-outpost-'"${ARCH}"'/src/monitor/solariMonitor deploy/dist/'"${ARCH}"'/solariMonitor
  # bundle the matched, TLS-enabled libnng (verify it actually has TLS symbols)
  l=$(ldconfig -p | grep "libnng.so.1" | awk "{print \$NF}" | head -1)
  echo "[outpost] libnng: $l  tls-symbols=$(nm -D "$l" 2>/dev/null | grep -ic tls)"
  cp -L "$l" deploy/dist/'"${ARCH}"'/libnng.so.1
'
sudo chown -R "$(id -u):$(id -g)" "build-outpost-${ARCH}" "deploy/dist/${ARCH}" 2>/dev/null || true
echo "[outpost] done -> deploy/dist/${ARCH}/{solariMonitor,libnng.so.1}"
file "deploy/dist/${ARCH}/solariMonitor" 2>/dev/null | cut -d, -f1-2 || true
