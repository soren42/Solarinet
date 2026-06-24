#!/usr/bin/env bash
#
# cross-build-client.sh - build solariClient for a foreign CPU arch.
#
# Rather than cross-compiling the whole TLS stack (nng + mbedTLS + sqlite), this
# does a NATIVE build for the target arch inside an emulated container (Docker +
# qemu/binfmt), using the distro's arch-native dev packages. The result is a
# ready-to-deploy binary at deploy/dist/<arch>/solariClient that remote-deploy.sh
# picks up automatically.
#
# Prereq (one-time): register qemu binfmt so Docker can run foreign images, e.g.
#   sudo docker run --privileged --rm tonistiigi/binfmt --install arm64
#
# Usage:
#   deploy/cross-build-client.sh [arch] [image]
#     arch  : arm64 (default) | arm32 | amd64
#     image : base image (default ubuntu:24.04 — has libnng/libmbedtls/sqlite)
set -euo pipefail

ARCH="${1:-arm64}"
IMAGE="${2:-ubuntu:24.04}"
REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

case "${ARCH}" in
  arm64)        PLATFORM="linux/arm64" ;;
  arm32|armhf)  PLATFORM="linux/arm/v7" ;;
  amd64|x86_64) PLATFORM="linux/amd64" ;;
  *) echo "unsupported arch: ${ARCH}" >&2; exit 1 ;;
esac

DOCKER="docker"; docker info >/dev/null 2>&1 || DOCKER="sudo docker"
echo "[xbuild] building solariClient for ${ARCH} (${PLATFORM}) in ${IMAGE}"

${DOCKER} run --rm --platform "${PLATFORM}" -v "${REPO}:/src" -w /src "${IMAGE}" bash -c '
  set -e
  export DEBIAN_FRONTEND=noninteractive
  apt-get update -y
  apt-get install -y --no-install-recommends \
    build-essential cmake pkg-config ca-certificates \
    libnng-dev libmbedtls-dev libsqlite3-dev libcjson-dev
  rm -rf build-'"${ARCH}"'
  cmake -S . -B build-'"${ARCH}"' \
    -DSOLARI_BUILD_CLIENT=ON -DSOLARI_BUILD_MONITOR=OFF \
    -DSOLARI_BUILD_SERVER=OFF -DSOLARI_BUILD_TESTS=OFF \
    -DSOLARI_WITH_IO=ON -DSOLARI_WITH_SQLITE=ON
  cmake --build build-'"${ARCH}"' -j"$(nproc)" --target solariClient
  install -D build-'"${ARCH}"'/src/client/solariClient deploy/dist/'"${ARCH}"'/solariClient
'

# the container wrote as root; reclaim ownership on the host
sudo chown -R "$(id -u):$(id -g)" "${REPO}/build-${ARCH}" "${REPO}/deploy/dist/${ARCH}" 2>/dev/null || true
echo "[xbuild] done -> deploy/dist/${ARCH}/solariClient"
file "${REPO}/deploy/dist/${ARCH}/solariClient" 2>/dev/null || true
