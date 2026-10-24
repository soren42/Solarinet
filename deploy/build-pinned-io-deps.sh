#!/usr/bin/env bash
# Build the exact I/O dependency versions used by bootstrap and CI.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
PREFIX="${1:-${REPO_ROOT}/third_party/.prefix}"
SOURCE_ROOT="${PREFIX}/src"
BUILD_JOBS="${BUILD_JOBS:-2}"
NNG_TAG="${NNG_TAG:-v1.8.0}"
MBEDTLS_TAG="${MBEDTLS_TAG:-v3.6.2}"
MBEDTLS_CMAKE_ARGS=()

# GCC 15 promoted upstream mbedTLS 3.6.2's intentional fixed-size TLS label
# initializers to an error under its own -Werror flags. Keep that one warning
# non-fatal only for mbedTLS; do not weaken warnings for NNG or SolariNet.
C_COMPILER="${CC:-cc}"
GCC_MAJOR="$(${C_COMPILER} -dM -E - </dev/null 2>/dev/null | awk '$2 == "__GNUC__" && !found { print $3; found = 1 } END { if (!found) exit 1 }')"
if [[ "${GCC_MAJOR}" =~ ^[0-9]+$ ]] && (( GCC_MAJOR >= 15 )); then
  MBEDTLS_CMAKE_ARGS+=("-DCMAKE_C_FLAGS=-Wno-error=unterminated-string-initialization")
fi

clone_pinned() {
  local url="$1" tag="$2" dest="$3" current
  if [ -d "${dest}/.git" ]; then
    current="$(git -C "${dest}" describe --tags --exact-match HEAD 2>/dev/null || true)"
    if [ "${current}" != "${tag}" ]; then
      printf 'dependency source at %s is not pinned to %s\n' "${dest}" "${tag}" >&2
      return 1
    fi
    git -C "${dest}" submodule update --init --recursive --depth 1
  elif [ -e "${dest}" ]; then
    printf 'dependency source path exists but is not a git checkout: %s\n' "${dest}" >&2
    return 1
  else
    mkdir -p "$(dirname "${dest}")"
    git clone --depth 1 --branch "${tag}" --recurse-submodules \
      --shallow-submodules "${url}" "${dest}"
  fi
}

mkdir -p "${PREFIX}" "${SOURCE_ROOT}"
clone_pinned "https://github.com/Mbed-TLS/mbedtls.git" "${MBEDTLS_TAG}" \
  "${SOURCE_ROOT}/mbedtls"
cmake -S "${SOURCE_ROOT}/mbedtls" -B "${SOURCE_ROOT}/mbedtls-build" \
  -DCMAKE_INSTALL_PREFIX="${PREFIX}" \
  -DENABLE_TESTING=OFF -DENABLE_PROGRAMS=OFF \
  -DCMAKE_POSITION_INDEPENDENT_CODE=ON "${MBEDTLS_CMAKE_ARGS[@]}"
cmake --build "${SOURCE_ROOT}/mbedtls-build" -j"${BUILD_JOBS}"
cmake --install "${SOURCE_ROOT}/mbedtls-build"

clone_pinned "https://github.com/nanomsg/nng.git" "${NNG_TAG}" \
  "${SOURCE_ROOT}/nng"
cmake -S "${SOURCE_ROOT}/nng" -B "${SOURCE_ROOT}/nng-build" \
  -DCMAKE_INSTALL_PREFIX="${PREFIX}" -DCMAKE_PREFIX_PATH="${PREFIX}" \
  -DNNG_ENABLE_TLS=ON -DNNG_TESTS=OFF -DNNG_TOOLS=OFF \
  -DCMAKE_POSITION_INDEPENDENT_CODE=ON
cmake --build "${SOURCE_ROOT}/nng-build" -j"${BUILD_JOBS}"
cmake --install "${SOURCE_ROOT}/nng-build"
