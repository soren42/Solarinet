#!/usr/bin/env bash
# Build portable Linux release binaries against the pinned static I/O libraries.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
PREFIX="${1:?usage: build-release-binaries.sh PINNED_IO_PREFIX [BUILD_DIR]}"
BUILD_DIR="${2:-${ROOT}/build-release}"

for file in libnng.a libmbedx509.a libmbedtls.a libmbedcrypto.a; do
  if [[ ! -f "${PREFIX}/lib/${file}" ]]; then
    printf 'pinned static dependency is missing: %s/lib/%s\n' "${PREFIX}" "${file}" >&2
    exit 1
  fi
done

cmake -S "${ROOT}" -B "${BUILD_DIR}" \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_PREFIX_PATH="${PREFIX}" \
  -DCMAKE_C_FLAGS="-I${PREFIX}/include" \
  -DNNG_LIB="${PREFIX}/lib/libnng.a" \
  -DNNG_INC="${PREFIX}/include" \
  -DMBED_mbedx509="${PREFIX}/lib/libmbedx509.a" \
  -DMBED_mbedtls="${PREFIX}/lib/libmbedtls.a" \
  -DMBED_mbedcrypto="${PREFIX}/lib/libmbedcrypto.a" \
  -DSOLARI_MBEDTLS_INCLUDE_DIR="${PREFIX}/include" \
  -DSOLARI_BUILD_SERVER=ON \
  -DSOLARI_BUILD_TESTS=OFF \
  -DSOLARI_WITH_IO=ON \
  -DSOLARI_WITH_DISCOVERY_TOOLS=ON \
  -DSOLARI_WITH_SQLITE=ON \
  -DSOLARI_WITH_JSON=ON
cmake --build "${BUILD_DIR}" --target solariServer solariScanWorker -j
python3 "${ROOT}/deploy/dashboard/check-release-binaries.py" "${BUILD_DIR}/src/server"
