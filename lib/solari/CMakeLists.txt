# libsolari — the shared library every component links (§6).
# Phase 1 "core" sources have zero external dependencies. Phase 2 "I/O"
# sources (transport, config-overlay, spool) are added only when
# SOLARI_WITH_IO is set and the vendored deps are present.

set(SOLARI_CORE_SOURCES
  solariError.c
  solariTime.c
  solariLog.c
  solariCrypto.c
  solariFrame.c
  solariTlv.c
  solariMsg.c
)

set(SOLARI_IO_SOURCES "")
if(SOLARI_WITH_IO)
  foreach(io_src solariConfig.c solariSpool.c solariNet.c)
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/${io_src}")
      list(APPEND SOLARI_IO_SOURCES ${io_src})
    endif()
  endforeach()
endif()

add_library(solari STATIC ${SOLARI_CORE_SOURCES} ${SOLARI_IO_SOURCES})

target_include_directories(solari PUBLIC
  $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/include>
)

# The logger is thread-safe (pthread on POSIX); link the platform thread lib.
find_package(Threads REQUIRED)
target_link_libraries(solari PUBLIC Threads::Threads)

# Link the vendored deps only when the I/O layer is compiled in.
if(SOLARI_WITH_IO)
  if(TARGET nng)
    target_link_libraries(solari PUBLIC nng)
  endif()
  if(TARGET mbedtls)
    target_link_libraries(solari PUBLIC mbedtls)
  endif()
  if(TARGET cjson)
    target_link_libraries(solari PUBLIC cjson)
  endif()
  # SQLite amalgamation is compiled directly into the spool object set.
endif()
