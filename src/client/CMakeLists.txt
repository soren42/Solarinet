# solariClient (sec 7). The platform-agnostic core (collection, reporting, PAL)
# is built into a static `clientcore` lib so both the executable and the test
# suite link the same objects. The PAL is chosen by target OS; the reporting
# layer (clientReport.c) compiles only when libsolari carries the nng transport
# and the SQLite spool.

set(CLIENT_CORE_SOURCES clientCollect.c clientWatchdog.c)

# Select the PAL for the target OS. Only platLinux.c exists today; platMacos.c
# and platWindows.c are future PAL steps (base plan sec 7 / Phase 3). If the
# chosen PAL source is not present yet, skip the client targets entirely so the
# portable libsolari core still configures and builds on that OS, rather than
# hard-failing add_library() on a missing source.
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  set(CLIENT_PAL_SOURCE plat/platLinux.c)
elseif(CMAKE_SYSTEM_NAME STREQUAL "Darwin")
  set(CLIENT_PAL_SOURCE plat/platMacos.c)     # macOS PAL step
elseif(CMAKE_SYSTEM_NAME STREQUAL "Windows")
  set(CLIENT_PAL_SOURCE plat/platWindows.c)   # Windows PAL step
else()
  set(CLIENT_PAL_SOURCE "")
endif()

if(NOT CLIENT_PAL_SOURCE OR NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/${CLIENT_PAL_SOURCE}")
  message(STATUS "solariClient: no PAL for ${CMAKE_SYSTEM_NAME} yet - skipping client build")
  return()
endif()

list(APPEND CLIENT_CORE_SOURCES ${CLIENT_PAL_SOURCE})

set(CLIENT_HAS_REPORTING OFF)
if(SOLARI_WITH_IO AND SOLARI_WITH_SQLITE)
  list(APPEND CLIENT_CORE_SOURCES clientReport.c clientDiscovery.c)
  set(CLIENT_HAS_REPORTING ON)
endif()

add_library(clientcore STATIC ${CLIENT_CORE_SOURCES})
target_include_directories(clientcore PUBLIC
  ${CMAKE_SOURCE_DIR}/include
  ${CMAKE_CURRENT_SOURCE_DIR}
)
target_link_libraries(clientcore PUBLIC solari)
if(CLIENT_HAS_REPORTING)
  target_compile_definitions(clientcore PUBLIC CLIENT_WITH_REPORTING=1)
endif()

add_executable(solariClient main.c)
target_link_libraries(solariClient PRIVATE clientcore)
