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
if(SOLARI_WITH_JSON AND TARGET cjson)
  target_compile_definitions(solari PRIVATE SOLARI_WITH_JSON=1)
  target_link_libraries(solari PUBLIC cjson)
endif()

# ---- nng/mbedTLS transport ---------------------------------------------
if(SOLARI_WITH_IO)
  if(TARGET nng)
    target_link_libraries(solari PUBLIC nng)
  endif()
  if(TARGET mbedtls)
    target_link_libraries(solari PUBLIC mbedtls)
  endif()
endif()
