# libsolari - the shared library every component links (sections 6, 12).
#
# CORE   : pure C99, zero external dependencies (Phase 1 + the config parser).
# SQLITE : the store-and-forward spool; needs SQLite (SOLARI_WITH_SQLITE).
# IO     : the nng/mbedTLS transport; needs nng + mbedTLS (SOLARI_WITH_IO).

set(SOLARI_CORE_SOURCES
  solariError.c
  solariTime.c
  solariLog.c
  solariCrypto.c
  solariFrame.c
  solariTlv.c
  solariMsg.c
  solariConfig.c        # zero deps (JSON overlay gated behind SOLARI_WITH_JSON)
)

set(SOLARI_EXTRA_SOURCES "")
if(SOLARI_WITH_SQLITE)
  list(APPEND SOLARI_EXTRA_SOURCES solariSpool.c)
endif()
if(SOLARI_WITH_IO)
  list(APPEND SOLARI_EXTRA_SOURCES solariNet.c)
endif()

add_library(solari STATIC ${SOLARI_CORE_SOURCES} ${SOLARI_EXTRA_SOURCES})

target_include_directories(solari PUBLIC
  $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/include>
)

# The logger is thread-safe (pthread on POSIX); link the platform thread lib.
find_package(Threads REQUIRED)
target_link_libraries(solari PUBLIC Threads::Threads)

# ---- SQLite spool -------------------------------------------------------
if(SOLARI_WITH_SQLITE)
  # Minimal vendored header (third_party/sqlite/sqlite3_min.h) until the full
  # amalgamation is dropped in; link the platform SQLite.
  target_include_directories(solari PRIVATE ${CMAKE_SOURCE_DIR}/third_party)
  find_library(SQLITE3_LIB NAMES sqlite3)
  if(SQLITE3_LIB)
    target_link_libraries(solari PUBLIC ${SQLITE3_LIB})
  else()
    # dev symlink absent but runtime present: link the versioned soname directly
    target_link_libraries(solari PUBLIC :libsqlite3.so.0)
  endif()
endif()

# ---- JSON overlay (optional) -------------------------------------------
if(SOLARI_WITH_JSON)
  target_compile_definitions(solari PRIVATE SOLARI_WITH_JSON=1)
  if(TARGET cjson)
    target_link_libraries(solari PUBLIC cjson)
  else()
    find_library(CJSON_LIB NAMES cjson REQUIRED)
    find_path(CJSON_INC NAMES cJSON.h PATH_SUFFIXES cjson)
    target_link_libraries(solari PUBLIC ${CJSON_LIB})
    if(CJSON_INC)
      target_include_directories(solari PRIVATE ${CJSON_INC})
    endif()
  endif()
endif()

# ---- nng/mbedTLS transport ---------------------------------------------
# Works either with vendored source (add_subdirectory exposes the `nng` /
# `mbedtls` targets) OR with system-installed dev packages (found here).
if(SOLARI_WITH_IO)
  if(TARGET nng)
    target_link_libraries(solari PUBLIC nng)
  else()
    find_library(NNG_LIB NAMES nng REQUIRED)
    find_path(NNG_INC NAMES nng/nng.h)
    target_link_libraries(solari PUBLIC ${NNG_LIB})
    if(NNG_INC)
      target_include_directories(solari PUBLIC ${NNG_INC})
    endif()
  endif()

  # mbedTLS is a transitive dependency of a TLS-enabled libnng, but link it
  # explicitly when available so static configurations resolve cleanly.
  if(TARGET mbedtls)
    target_link_libraries(solari PUBLIC mbedtls mbedx509 mbedcrypto)
  else()
    foreach(mb mbedtls mbedx509 mbedcrypto)
      find_library(MBED_${mb} NAMES ${mb})
      if(MBED_${mb})
        target_link_libraries(solari PUBLIC ${MBED_${mb}})
      endif()
    endforeach()
  endif()
endif()
