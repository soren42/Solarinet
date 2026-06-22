# solariServer (§9, §12.2). x86_64/arm64 only; gated by SOLARI_BUILD_SERVER.
# Mirrors the monitor shape: every subsystem .c builds into a `servercore`
# static lib that both the executable and (future) tests link; main.c is a thin
# binary. Links libsolari (which transitively brings nng + mbedTLS) plus the
# MariaDB Connector/C (libmariadb), found via pkg-config with a mariadb_config
# fallback.

# ---- MariaDB Connector/C discovery -------------------------------------
# Prefer pkg-config 'libmariadb'; fall back to mariadb_config; fall back to a
# plain find_library. Sets MARIADB_LIBS and MARIADB_INCS.
find_package(PkgConfig QUIET)
set(MARIADB_LIBS "")
set(MARIADB_INCS "")
if(PkgConfig_FOUND)
  pkg_check_modules(MARIADB QUIET libmariadb)
endif()
if(MARIADB_FOUND)
  set(MARIADB_LIBS ${MARIADB_LIBRARIES})
  set(MARIADB_INCS ${MARIADB_INCLUDE_DIRS})
else()
  find_program(MARIADB_CONFIG NAMES mariadb_config)
  if(MARIADB_CONFIG)
    execute_process(COMMAND ${MARIADB_CONFIG} --libs
      OUTPUT_VARIABLE MARIADB_LIBS OUTPUT_STRIP_TRAILING_WHITESPACE)
    execute_process(COMMAND ${MARIADB_CONFIG} --include
      OUTPUT_VARIABLE _mariadb_inc_flags OUTPUT_STRIP_TRAILING_WHITESPACE)
    string(REPLACE "-I" "" MARIADB_INCS "${_mariadb_inc_flags}")
    separate_arguments(MARIADB_INCS)
  else()
    find_library(MARIADB_LIB NAMES mariadb mysqlclient REQUIRED)
    find_path(MARIADB_INC NAMES mariadb/mysql.h mysql/mysql.h)
    set(MARIADB_LIBS ${MARIADB_LIB})
    if(MARIADB_INC)
      set(MARIADB_INCS ${MARIADB_INC})
    endif()
  endif()
endif()

set(SERVER_CORE_SOURCES
  serverContext.c
  serverDb.c
  serverIngest.c
  serverLease.c
  serverMaster.c
  serverAlert.c
  serverControl.c
  serverProvision.c
  serverDiscovery.c
  serverScan.c
  serverTopology.c
  solariCtl.c
)

add_library(servercore STATIC ${SERVER_CORE_SOURCES})

# Optional best-effort discovery enrichment via host tools (avahi/SNMP). OFF by
# default so the core scan stays pure standard C; enable to layer mDNS/SNMP hints
# onto port-scan results (requires avahi-utils / net-snmp on the server host).
option(SOLARI_WITH_DISCOVERY_TOOLS "Enrich discovery scans via host avahi/SNMP tools" OFF)
if(SOLARI_WITH_DISCOVERY_TOOLS)
  target_compile_definitions(servercore PRIVATE SOLARI_WITH_DISCOVERY_TOOLS=1)
endif()
target_include_directories(servercore PUBLIC
  ${CMAKE_SOURCE_DIR}/include
  ${CMAKE_CURRENT_SOURCE_DIR}
)
if(MARIADB_INCS)
  target_include_directories(servercore PUBLIC ${MARIADB_INCS})
endif()
target_link_libraries(servercore PUBLIC solari ${MARIADB_LIBS})

add_executable(solariServer main.c)
target_link_libraries(solariServer PRIVATE servercore)

message(STATUS "solariServer: MariaDB libs=${MARIADB_LIBS}")
