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
  serverDiscoveryEnrich.c
  serverScan.c
  serverOui.c
  serverAssets.c
  serverTopology.c
  serverSnmp.c
  solariCtl.c
)

add_library(servercore STATIC ${SERVER_CORE_SOURCES})
if(NOT SOLARI_WITH_IO)
  target_sources(servercore PRIVATE serverNetStub.c)
endif()

# Optional best-effort discovery enrichment via host tools (avahi/SNMP). OFF by
# default so the core scan stays pure standard C; enable to layer mDNS/SNMP hints
# onto port-scan results (requires avahi-utils / net-snmp on the server host).
option(SOLARI_WITH_DISCOVERY_TOOLS "Enrich discovery scans via host avahi/SNMP tools" OFF)
if(SOLARI_WITH_DISCOVERY_TOOLS)
  target_compile_definitions(servercore PRIVATE SOLARI_WITH_DISCOVERY_TOOLS=1)
endif()

# Optional SNMP paths (serverSnmp.c IF-MIB polling and serverTopology.c LLDP
# topology discovery). OFF -> both fetch paths return ERR_PLATFORM, while pure
# parsers remain available. ON shells out to net-snmp's snmpbulkwalk at runtime;
# there is no libnetsnmp build-time dependency.
option(SOLARI_WITH_SNMP "Enable IF-MIB polling and LLDP topology walks (needs net-snmp CLI at runtime)" OFF)
if(SOLARI_WITH_SNMP)
  target_compile_definitions(servercore PUBLIC SOLARI_WITH_SNMP=1)
endif()
target_include_directories(servercore PUBLIC
  ${CMAKE_SOURCE_DIR}/include
  ${CMAKE_CURRENT_SOURCE_DIR}
)
if(MARIADB_INCS)
  target_include_directories(servercore PUBLIC ${MARIADB_INCS})
endif()
target_link_libraries(servercore PUBLIC solari ${MARIADB_LIBS})

# Enable real CSR signing only when the installed mbedTLS exposes the CSR SAN
# API used by the identity contract. Older distro packages may provide the
# headers but omit parsed CSR extensions; those builds must fail closed.
if(SOLARI_WITH_IO)
  include(CheckCSourceCompiles)
  find_path(SOLARI_MBEDTLS_INCLUDE_DIR mbedtls/x509_csr.h)
  if(SOLARI_MBEDTLS_INCLUDE_DIR)
    set(_SOLARI_TRY_COMPILE_TYPE "${CMAKE_TRY_COMPILE_TARGET_TYPE}")
    set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)
    set(CMAKE_REQUIRED_INCLUDES "${SOLARI_MBEDTLS_INCLUDE_DIR}")
    set(CMAKE_REQUIRED_FLAGS "-Werror=implicit-function-declaration")
    check_c_source_compiles("#include <mbedtls/x509.h>
#include <mbedtls/x509_csr.h>
int main(void) {
    mbedtls_x509_csr csr;
    mbedtls_x509_subject_alternative_name san;
    const mbedtls_x509_sequence *entry = &csr.subject_alt_names;
    if (entry && entry->buf.p)
        (void) mbedtls_x509_parse_subject_alt_name(&entry->buf, &san);
    mbedtls_x509_free_subject_alt_name(&san);
    return 0;
}" SOLARI_HAVE_MBEDTLS_CSR_SAN_API)
    unset(CMAKE_REQUIRED_INCLUDES)
    unset(CMAKE_REQUIRED_FLAGS)
    if(_SOLARI_TRY_COMPILE_TYPE)
      set(CMAKE_TRY_COMPILE_TARGET_TYPE "${_SOLARI_TRY_COMPILE_TYPE}")
    else()
      unset(CMAKE_TRY_COMPILE_TARGET_TYPE)
    endif()
  endif()
  if(SOLARI_HAVE_MBEDTLS_CSR_SAN_API)
    target_compile_definitions(servercore PRIVATE SOLARI_HAVE_MBEDTLS_X509=1)
    message(STATUS "solariServer: CSR signing enabled (mbedTLS CSR SAN API)")
  else()
    message(STATUS "solariServer: CSR signing disabled; installed mbedTLS lacks CSR SAN support")
  endif()
endif()

add_executable(solariServer main.c)
target_link_libraries(solariServer PRIVATE servercore)

# Durable discovery scans run in a separate process with their own MariaDB
# connection, leaving the server lease/control event loop responsive.
add_executable(solariScanWorker scanWorkerMain.c)
target_link_libraries(solariScanWorker PRIVATE servercore)

# solariSnmpPoll — standalone SNMP polling driver (systemd-timer friendly).
add_executable(solariSnmpPoll snmpPollMain.c)
target_link_libraries(solariSnmpPoll PRIVATE servercore)

# solariCtlClient — standard-C client for the solariCtl operator bridge
# (replaces the python3 helper the deploy scripts used to embed). Pure
# libc + POSIX sockets; no servercore dependency.
add_executable(solariCtlClient ctlClientMain.c)

message(STATUS "solariServer: MariaDB libs=${MARIADB_LIBS}")
