# SolariNet Monitoring — top-level build (§12)
# Strict ISO C99, functions-only. Orchestrates libsolari, the three C
# components, and the test suites. Heavy third-party deps (nng, mbedTLS,
# SQLite, cJSON) are vendored under third_party/ and only pulled in when the
# I/O layer (Phase 2+) is enabled, so the Phase 1 core builds with zero
# external dependencies.
cmake_minimum_required(VERSION 3.16)
project(solarinet C)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)            # -std=c99, not gnu99: keep it portable

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Debug CACHE STRING "Build type" FORCE)
endif()

add_compile_options(-Wall -Wextra -Wpedantic -Werror=implicit-function-declaration)

# ---- options -------------------------------------------------------------
option(SOLARI_BUILD_SERVER "Build solariServer (x86_64/arm64 only)" OFF)
option(SOLARI_BUILD_CLIENT "Build solariClient (needs a PAL for the target OS)" ON)
option(SOLARI_BUILD_MONITOR "Build solariMonitor (POSIX/Linux for now)"        ON)
option(SOLARI_BUILD_TESTS  "Build the Unity test suites"            ON)
option(SOLARI_WITH_IO      "Build the nng/mbedTLS transport layer"  OFF)
option(SOLARI_WITH_SQLITE  "Build the SQLite store-and-forward spool" OFF)
option(SOLARI_WITH_JSON    "Enable the cJSON config overlay"        OFF)
option(SOLARI_BUILD_FUZZERS "Build Clang/libFuzzer protocol parser targets" OFF)

if(SOLARI_BUILD_FUZZERS)
  if(NOT CMAKE_C_COMPILER_ID MATCHES "Clang")
    message(FATAL_ERROR "SOLARI_BUILD_FUZZERS requires Clang with libFuzzer support")
  endif()
  if(NOT SOLARI_BUILD_TESTS)
    message(FATAL_ERROR "SOLARI_BUILD_FUZZERS requires SOLARI_BUILD_TESTS=ON")
  endif()
endif()

# ---- vendored deps (only when the I/O layer is on) -----------------------
if(SOLARI_WITH_IO)
  if(EXISTS "${CMAKE_SOURCE_DIR}/third_party/mbedtls/CMakeLists.txt")
    add_subdirectory(third_party/mbedtls)
  endif()
  if(EXISTS "${CMAKE_SOURCE_DIR}/third_party/nng/CMakeLists.txt")
    add_subdirectory(third_party/nng)
  endif()
  if(EXISTS "${CMAKE_SOURCE_DIR}/third_party/cJSON/CMakeLists.txt")
    add_subdirectory(third_party/cJSON)
  endif()
endif()

# ---- libsolari (always) --------------------------------------------------
add_subdirectory(lib/solari)

# Instrument the shared protocol parser code for coverage-guided fuzzing.
# `fuzzer-no-link` leaves normal test executables without libFuzzer's main;
# the dedicated fuzzer target supplies it at link time.
if(SOLARI_BUILD_FUZZERS)
  target_compile_options(solari PRIVATE
    -fsanitize=fuzzer-no-link,address,undefined -fno-omit-frame-pointer)
  target_link_options(solari INTERFACE
    -fsanitize=fuzzer-no-link,address,undefined)
endif()

# ---- components (only when enabled and their entry point exists) ---------
# Client/monitor are Linux-first; the client self-skips on an OS whose PAL is
# not yet implemented (src/client/CMakeLists.txt), and either can be turned off
# explicitly so the portable libsolari core still cross-builds for macOS/Windows.
if(SOLARI_BUILD_CLIENT AND EXISTS "${CMAKE_SOURCE_DIR}/src/client/main.c")
  add_subdirectory(src/client)
endif()
if(SOLARI_BUILD_MONITOR AND EXISTS "${CMAKE_SOURCE_DIR}/src/monitor/main.c")
  add_subdirectory(src/monitor)
endif()
if(SOLARI_BUILD_SERVER AND EXISTS "${CMAKE_SOURCE_DIR}/src/server/main.c")
  add_subdirectory(src/server)
endif()

# ---- tests ---------------------------------------------------------------
if(SOLARI_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()

message(STATUS "SolariNet config: tests=${SOLARI_BUILD_TESTS} io=${SOLARI_WITH_IO} sqlite=${SOLARI_WITH_SQLITE} server=${SOLARI_BUILD_SERVER}")
