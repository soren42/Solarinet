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

# ---- options -------------------------------------------------------------
option(SOLARI_BUILD_SERVER "Build solariServer (x86_64/arm64 only)" OFF)
option(SOLARI_BUILD_CLIENT "Build solariClient (needs a PAL for the target OS)" ON)
option(SOLARI_BUILD_MONITOR "Build solariMonitor (POSIX/Linux for now)"        ON)
option(SOLARI_BUILD_TESTS  "Build the Unity test suites"            ON)
option(SOLARI_WITH_IO      "Build the nng/mbedTLS transport layer"  OFF)
option(SOLARI_WITH_SQLITE  "Build the SQLite store-and-forward spool" OFF)
option(SOLARI_WITH_JSON    "Enable the cJSON config overlay"        OFF)
option(SOLARI_BUILD_FUZZERS "Build Clang/libFuzzer protocol parser targets" OFF)
set(_solari_vendored_deps_default OFF)
if(EXISTS "${CMAKE_SOURCE_DIR}/third_party/mbedtls/CMakeLists.txt" AND
   EXISTS "${CMAKE_SOURCE_DIR}/third_party/nng/CMakeLists.txt" AND
   EXISTS "${CMAKE_SOURCE_DIR}/third_party/cJSON/CMakeLists.txt")
  set(_solari_vendored_deps_default ON)
endif()
option(SOLARI_VENDORED_DEPS "Build vendored static nng, mbedTLS, and cJSON" ${_solari_vendored_deps_default})
option(SOLARI_ALLOW_SYSTEM_DEPS "Permit a Release I/O build against system nng/mbedTLS" OFF)

if(SOLARI_BUILD_FUZZERS)
  if(NOT CMAKE_C_COMPILER_ID MATCHES "Clang")
    message(FATAL_ERROR "SOLARI_BUILD_FUZZERS requires Clang with libFuzzer support")
  endif()
  if(NOT SOLARI_BUILD_TESTS)
    message(FATAL_ERROR "SOLARI_BUILD_FUZZERS requires SOLARI_BUILD_TESTS=ON")
  endif()
endif()

# ---- vendored deps (only when the I/O layer is on) -----------------------
if(SOLARI_WITH_IO OR SOLARI_WITH_JSON)
  if(SOLARI_VENDORED_DEPS)
    if((SOLARI_WITH_IO AND (NOT EXISTS "${CMAKE_SOURCE_DIR}/third_party/mbedtls/CMakeLists.txt" OR
                            NOT EXISTS "${CMAKE_SOURCE_DIR}/third_party/nng/CMakeLists.txt")) OR
       (SOLARI_WITH_JSON AND NOT EXISTS "${CMAKE_SOURCE_DIR}/third_party/cJSON/CMakeLists.txt"))
      message(FATAL_ERROR "SOLARI_VENDORED_DEPS=ON but a required dependency submodule is missing; run git submodule update --init --recursive or configure with -DSOLARI_VENDORED_DEPS=OFF for system libraries")
    endif()
    message(STATUS "SolariNet dependencies: vendored static libraries")
    if(SOLARI_WITH_IO)
      set(CMAKE_POSITION_INDEPENDENT_CODE ON)
      set(BUILD_SHARED_LIBS OFF CACHE BOOL "Build static third-party libraries" FORCE)
      set(ENABLE_PROGRAMS OFF CACHE BOOL "Disable mbedTLS programs" FORCE)
      set(ENABLE_TESTING OFF CACHE BOOL "Disable mbedTLS tests" FORCE)
      set(DISABLE_PACKAGE_CONFIG_AND_INSTALL ON CACHE BOOL "Disable mbedTLS package/install targets" FORCE)
      set(NNG_TESTS OFF CACHE BOOL "Disable nng tests" FORCE)
      set(NNG_TOOLS OFF CACHE BOOL "Disable nng tools" FORCE)
      set(NNG_ENABLE_NNGCAT OFF CACHE BOOL "Disable nngcat" FORCE)
      set(NNG_ENABLE_DOC OFF CACHE BOOL "Disable nng documentation" FORCE)
      set(NNG_ENABLE_TLS ON CACHE BOOL "Enable TLS in nng" FORCE)
      set(NNG_TLS_ENGINE mbed CACHE STRING "Use the in-tree mbedTLS engine" FORCE)
      set(MBEDTLS_USER_CONFIG_FILE "${CMAKE_SOURCE_DIR}/third_party/mbedtls_solari_config.h"
        CACHE FILEPATH "Thread-safe Mbed TLS for nng" FORCE)
      add_subdirectory(third_party/mbedtls)
      # NNG exports a build-tree package during configuration. Register its
      # private static TLS dependency in that export set to satisfy CMake's
      # export validation (the project itself only consumes the build targets).
      install(TARGETS mbedtls mbedx509 mbedcrypto everest p256m EXPORT nng-target
        ARCHIVE DESTINATION lib)
      add_subdirectory(third_party/nng)
      foreach(_dep_target mbedtls mbedx509 mbedcrypto nng)
        if(TARGET ${_dep_target})
          set_target_properties(${_dep_target} PROPERTIES POSITION_INDEPENDENT_CODE ON)
        endif()
      endforeach()
    endif()
    if(SOLARI_WITH_JSON)
      set(BUILD_SHARED_LIBS OFF CACHE BOOL "Build static third-party libraries" FORCE)
      set(ENABLE_CUSTOM_COMPILER_FLAGS OFF CACHE BOOL "Do not apply cJSON's global warning flags" FORCE)
      set(ENABLE_TARGET_EXPORT OFF CACHE BOOL "Disable cJSON export targets" FORCE)
      set(CJSON_OVERRIDE_BUILD_SHARED_LIBS ON CACHE BOOL "Select static cJSON" FORCE)
      set(CJSON_BUILD_SHARED_LIBS OFF CACHE BOOL "Build static cJSON" FORCE)
      set(ENABLE_CJSON_TEST OFF CACHE BOOL "Disable cJSON tests" FORCE)
      set(ENABLE_CJSON_UTILS OFF CACHE BOOL "Disable cJSON utilities" FORCE)
      set(ENABLE_CJSON_UNINSTALL OFF CACHE BOOL "Disable cJSON uninstall target" FORCE)
      set(ENABLE_FUZZING OFF CACHE BOOL "Disable cJSON fuzzing targets" FORCE)
      # cJSON 1.7.19 advertises CMake 3.0, below CMake 4's supported policy
      # floor. This compatibility floor lets its upstream project configure.
      set(CMAKE_POLICY_VERSION_MINIMUM 3.5)
      add_subdirectory(third_party/cJSON)
      set_target_properties(cjson PROPERTIES POSITION_INDEPENDENT_CODE ON)
    endif()
  elseif(SOLARI_WITH_IO AND CMAKE_BUILD_TYPE STREQUAL "Release" AND NOT SOLARI_ALLOW_SYSTEM_DEPS)
    # A release agent linked to distro nng runs only where that nng was built
    # with TLS (not Tumbleweed), so refuse rather than ship one by accident.
    message(FATAL_ERROR "Release builds with SOLARI_WITH_IO need the vendored dependencies: run git submodule update --init --recursive, or pass -DSOLARI_ALLOW_SYSTEM_DEPS=ON to link system libraries anyway")
  else()
    message(WARNING "SolariNet dependencies: system libraries (fallback mode); release builds requiring TLS should use vendored dependencies")
  endif()
endif()

# Keep the project's strict warnings off third-party sources. This is added
# after dependency targets have been created, so it applies to SolariNet code.
add_compile_options(-Wall -Wextra -Wpedantic -Werror=implicit-function-declaration)
if(CMAKE_C_COMPILER_ID MATCHES "Clang")
  add_compile_options(-Wno-c11-extensions)
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
