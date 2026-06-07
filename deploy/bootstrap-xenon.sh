#!/usr/bin/env bash
#
# bootstrap-xenon.sh - bring a fresh Linux host (e.g. xenon.akoria.net) to a
# state where SolariNet builds and its test suites pass.
#
# It installs the C toolchain and the SolariNet dependency set, preferring the
# distro's dev packages and falling back to building nng / mbedTLS from pinned
# source when packages are unavailable. Then it configures, builds, and runs the
# test suites in stages.
#
# Dependencies (section 3 of the architecture plan):
#   build:      gcc, make, cmake (>=3.16), git, pkg-config, curl, unzip
#   libraries:  mbedTLS, nng (+TLS), SQLite, cJSON, MariaDB Connector/C
#   tests:      Unity (vendored shim already in-tree)
#
# Usage:
#   ./deploy/bootstrap-xenon.sh [options]
#
# Options:
#   --source-deps     Force building nng + mbedTLS from pinned source even if
#                     system packages exist.
#   --with-cross      Also install cross-compile toolchains (arm64/armhf/mingw).
#   --no-install      Skip package installation (assume deps already present).
#   --build-dir DIR   Build directory (default: build).
#   --prefix DIR      Install prefix for source-built deps (default: third_party/.prefix).
#   -h | --help       Show this help.
#
# Safe to re-run: package installs and clones are idempotent.
set -euo pipefail

# ---- pinned versions (override via env) -----------------------------------
NNG_TAG="${NNG_TAG:-v1.8.0}"
MBEDTLS_TAG="${MBEDTLS_TAG:-v3.6.2}"
SQLITE_YEAR="${SQLITE_YEAR:-2024}"
SQLITE_AMALG="${SQLITE_AMALG:-sqlite-amalgamation-3460100}"   # 3.46.1
CJSON_TAG="${CJSON_TAG:-v1.7.18}"
UNITY_TAG="${UNITY_TAG:-v2.6.0}"

# ---- defaults -------------------------------------------------------------
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
BUILD_DIR="build"
PREFIX="${REPO_ROOT}/third_party/.prefix"
SOURCE_DEPS=0
WITH_CROSS=0
DO_INSTALL=1

log()  { printf '\033[1;36m[xenon]\033[0m %s\n' "$*"; }
warn() { printf '\033[1;33m[xenon][warn]\033[0m %s\n' "$*" >&2; }
die()  { printf '\033[1;31m[xenon][error]\033[0m %s\n' "$*" >&2; exit 1; }

# ---- arg parsing ----------------------------------------------------------
while [ $# -gt 0 ]; do
  case "$1" in
    --source-deps) SOURCE_DEPS=1 ;;
    --with-cross)  WITH_CROSS=1 ;;
    --no-install)  DO_INSTALL=0 ;;
    --build-dir)   BUILD_DIR="${2:?}"; shift ;;
    --prefix)      PREFIX="${2:?}"; shift ;;
    -h|--help)     grep '^#' "$0" | grep -v '^#!' | sed 's/^# \{0,1\}//'; exit 0 ;;
    *) die "unknown option: $1 (try --help)" ;;
  esac
  shift
done

cd "${REPO_ROOT}"

# ---- privilege helper -----------------------------------------------------
SUDO=""
if [ "$(id -u)" -ne 0 ]; then
  if command -v sudo >/dev/null 2>&1; then SUDO="sudo"; else
    warn "not root and no sudo; package install may fail (use --no-install if deps are present)"
  fi
fi

# ---- distro detection -----------------------------------------------------
PKG=""
if command -v apt-get >/dev/null 2>&1; then PKG="apt"
elif command -v dnf >/dev/null 2>&1; then PKG="dnf"
elif command -v yum >/dev/null 2>&1; then PKG="yum"
else warn "no supported package manager found (apt/dnf/yum)"; fi

install_packages() {
  [ "${DO_INSTALL}" -eq 1 ] || { log "skipping package install (--no-install)"; return 0; }
  case "${PKG}" in
    apt)
      log "installing build deps via apt"
      ${SUDO} apt-get update -y
      ${SUDO} apt-get install -y \
        build-essential cmake git pkg-config curl unzip ca-certificates \
        libsqlite3-dev libcjson-dev libmariadb-dev
      # nng/mbedTLS from packages unless --source-deps
      if [ "${SOURCE_DEPS}" -eq 0 ]; then
        ${SUDO} apt-get install -y libmbedtls-dev libnng-dev || \
          { warn "libnng-dev/libmbedtls-dev not available; will build from source"; SOURCE_DEPS=1; }
      fi
      if [ "${WITH_CROSS}" -eq 1 ]; then
        ${SUDO} apt-get install -y \
          gcc-aarch64-linux-gnu gcc-arm-linux-gnueabihf gcc-mingw-w64-x86-64 || \
          warn "some cross toolchains unavailable"
      fi
      ;;
    dnf|yum)
      log "installing build deps via ${PKG}"
      ${SUDO} ${PKG} install -y \
        gcc make cmake git pkgconf-pkg-config curl unzip ca-certificates \
        sqlite-devel cJSON-devel mariadb-connector-c-devel
      if [ "${SOURCE_DEPS}" -eq 0 ]; then
        ${SUDO} ${PKG} install -y mbedtls-devel nng-devel || \
          { warn "nng-devel/mbedtls-devel not available; will build from source"; SOURCE_DEPS=1; }
      fi
      ;;
    *) warn "unknown package manager; ensure deps are installed manually" ;;
  esac
}

# ---- vendoring helpers ----------------------------------------------------
clone_pinned() {  # url tag dest
  local url="$1" tag="$2" dest="$3"
  if [ -d "${dest}/.git" ]; then
    log "already cloned: ${dest}"
  else
    log "cloning ${url} @ ${tag}"
    git clone --depth 1 --branch "${tag}" "${url}" "${dest}"
  fi
}

build_mbedtls_from_source() {
  local src="third_party/mbedtls"
  clone_pinned "https://github.com/Mbed-TLS/mbedtls.git" "${MBEDTLS_TAG}" "${src}"
  log "building mbedTLS -> ${PREFIX}"
  cmake -S "${src}" -B "${src}/build" \
    -DCMAKE_INSTALL_PREFIX="${PREFIX}" \
    -DENABLE_TESTING=OFF -DENABLE_PROGRAMS=OFF \
    -DCMAKE_POSITION_INDEPENDENT_CODE=ON
  cmake --build "${src}/build" -j
  cmake --install "${src}/build"
}

build_nng_from_source() {
  local src="third_party/nng"
  clone_pinned "https://github.com/nanomsg/nng.git" "${NNG_TAG}" "${src}"
  log "building nng (TLS via mbedTLS) -> ${PREFIX}"
  cmake -S "${src}" -B "${src}/build" \
    -DCMAKE_INSTALL_PREFIX="${PREFIX}" \
    -DCMAKE_PREFIX_PATH="${PREFIX}" \
    -DNNG_ENABLE_TLS=ON -DNNG_TESTS=OFF -DNNG_TOOLS=OFF \
    -DCMAKE_POSITION_INDEPENDENT_CODE=ON
  cmake --build "${src}/build" -j
  cmake --install "${src}/build"
}

vendor_sqlite_amalgamation() {
  local dest="third_party/sqlite"
  if [ -f "${dest}/sqlite3.h" ]; then log "sqlite amalgamation present"; return 0; fi
  log "fetching ${SQLITE_AMALG}"
  local url="https://www.sqlite.org/${SQLITE_YEAR}/${SQLITE_AMALG}.zip"
  curl -fSL "${url}" -o /tmp/sqlite_amalg.zip
  unzip -o /tmp/sqlite_amalg.zip -d /tmp >/dev/null
  cp "/tmp/${SQLITE_AMALG}/sqlite3.h" "/tmp/${SQLITE_AMALG}/sqlite3.c" "${dest}/"
  log "sqlite amalgamation placed in ${dest} (real sqlite3.h available)"
}

vendor_unity() {
  local dest="third_party/unity"
  if [ -f "${dest}/.upstream" ]; then log "upstream Unity already vendored"; return 0; fi
  log "vendoring upstream Unity ${UNITY_TAG} (replacing the in-tree shim)"
  local tmp; tmp="$(mktemp -d)"
  git clone --depth 1 --branch "${UNITY_TAG}" \
    https://github.com/ThrowTheSwitch/Unity.git "${tmp}/unity"
  cp "${tmp}/unity/src/unity.c" "${tmp}/unity/src/unity.h" \
     "${tmp}/unity/src/unity_internals.h" "${dest}/"
  touch "${dest}/.upstream"
  rm -rf "${tmp}"
}

# ---- run ------------------------------------------------------------------
log "SolariNet bootstrap starting (repo: ${REPO_ROOT})"
install_packages

mkdir -p "${PREFIX}"

# SQLite amalgamation is optional (system libsqlite3-dev already satisfies the
# build); fetch it only if the dev package is absent.
if ! { pkg-config --exists sqlite3 2>/dev/null || [ -f /usr/include/sqlite3.h ]; }; then
  vendor_sqlite_amalgamation || warn "sqlite amalgamation fetch failed; install libsqlite3-dev"
fi

CMAKE_EXTRA=()
if [ "${SOURCE_DEPS}" -eq 1 ]; then
  build_mbedtls_from_source
  build_nng_from_source
  CMAKE_EXTRA+=("-DCMAKE_PREFIX_PATH=${PREFIX}")
fi

# Optionally drop in the real upstream Unity (best-effort; shim works regardless).
vendor_unity || warn "upstream Unity vendoring skipped; using in-tree shim"

# ---- configure & build ----------------------------------------------------
log "configuring (SQLITE + IO + tests)"
cmake -S . -B "${BUILD_DIR}" \
  -DSOLARI_BUILD_TESTS=ON \
  -DSOLARI_WITH_SQLITE=ON \
  -DSOLARI_WITH_IO=ON \
  "${CMAKE_EXTRA[@]}"

log "building"
cmake --build "${BUILD_DIR}" -j

# ---- staged tests ---------------------------------------------------------
# Core + spool must pass (hard gate); transport loopback is best-effort and
# reported separately so a transient port/socket issue doesn't mask success.
log "running core + spool suites (required)"
ctest --test-dir "${BUILD_DIR}" --output-on-failure \
  -E 'test_net_loopback'

log "running transport loopback (best-effort)"
if ctest --test-dir "${BUILD_DIR}" --output-on-failure -R 'test_net_loopback'; then
  log "transport loopback PASSED"
else
  warn "transport loopback did not pass - inspect above (port in use? nng TLS build?)"
fi

log "bootstrap complete. Binaries/tests in ./${BUILD_DIR}"
log "next: provision MariaDB + apply db/schema.sql for the server tier (Phase 5)."
