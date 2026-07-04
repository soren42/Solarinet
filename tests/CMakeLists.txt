# Test tree. Unity (vendored under third_party/unity) is built once and each
# unit suite links it plus libsolari, then registers with ctest.

add_library(unity STATIC ${CMAKE_SOURCE_DIR}/third_party/unity/unity.c)
target_include_directories(unity PUBLIC ${CMAKE_SOURCE_DIR}/third_party/unity)
# Force 64-bit assertion support on every target ABI. Unity auto-enables it on
# 64-bit hosts but disables it on 32-bit ones (e.g. arm32), which would abort
# the u64 TLV/frame round-trip tests with "Unity 64-bit Support Disabled".
target_compile_definitions(unity PUBLIC UNITY_SUPPORT_64)

set(SOLARI_UNIT_TESTS
  test_crypto
  test_tlv
  test_frame
  test_msg
  test_config
)

foreach(t ${SOLARI_UNIT_TESTS})
  add_executable(${t} unit/${t}.c)
  target_link_libraries(${t} PRIVATE solari unity)
  target_include_directories(${t} PRIVATE ${CMAKE_SOURCE_DIR}/include)
  add_test(NAME ${t} COMMAND ${t})
endforeach()

# Spool fault test - only when the SQLite spool is built into libsolari.
if(SOLARI_WITH_SQLITE)
  add_executable(test_spool fault/test_spool.c)
  target_link_libraries(test_spool PRIVATE solari unity)
  target_include_directories(test_spool PRIVATE ${CMAKE_SOURCE_DIR}/include)
  add_test(NAME test_spool COMMAND test_spool)
endif()

# Transport loopback - only when the nng/mbedTLS I/O layer is built.
if(SOLARI_WITH_IO)
  add_executable(test_net_loopback integration/test_net_loopback.c)
  target_link_libraries(test_net_loopback PRIVATE solari unity)
  target_include_directories(test_net_loopback PRIVATE ${CMAKE_SOURCE_DIR}/include)
  add_test(NAME test_net_loopback COMMAND test_net_loopback)
endif()

# Linux PAL test (Phase 3) - compiles the platform reference impl directly so
# the seam is covered before the full client target is wired.
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  add_executable(test_platlinux unit/test_platlinux.c
                 ${CMAKE_SOURCE_DIR}/src/client/plat/platLinux.c)
  target_include_directories(test_platlinux PRIVATE
    ${CMAKE_SOURCE_DIR}/include ${CMAKE_SOURCE_DIR}/src/client)
  target_link_libraries(test_platlinux PRIVATE solari unity)
  add_test(NAME test_platlinux COMMAND test_platlinux)
endif()

# Client reporting (push-or-spool) integration test - needs the transport and
# the spool, so it links clientcore (which carries CLIENT_WITH_REPORTING).
if(TARGET clientcore AND SOLARI_WITH_IO AND SOLARI_WITH_SQLITE)
  add_executable(test_client_report integration/test_client_report.c)
  target_link_libraries(test_client_report PRIVATE clientcore unity)
  add_test(NAME test_client_report COMMAND test_client_report)
endif()

# Watchdog unit test - heartbeat frame + liveness decision (no transport needed).
if(TARGET clientcore AND CMAKE_SYSTEM_NAME STREQUAL "Linux")
  add_executable(test_watchdog unit/test_watchdog.c)
  target_link_libraries(test_watchdog PRIVATE clientcore unity)
  add_test(NAME test_watchdog COMMAND test_watchdog)
endif()

# Monitor: HRW ownership (unit) + the TCP probe engine (integration). Both link
# monitorcore, which is Linux-only.
if(TARGET monitorcore)
  add_executable(test_hrw unit/test_hrw.c)
  target_link_libraries(test_hrw PRIVATE monitorcore unity)
  add_test(NAME test_hrw COMMAND test_hrw)

  add_executable(test_probe integration/test_probe.c)
  target_link_libraries(test_probe PRIVATE monitorcore unity)
  add_test(NAME test_probe COMMAND test_probe)

  add_executable(test_peers unit/test_peers.c)
  target_link_libraries(test_peers PRIVATE monitorcore unity)
  add_test(NAME test_peers COMMAND test_peers)
endif()

# Monitor reporting + gossip - need the transport (and spool).
if(TARGET monitorcore AND SOLARI_WITH_IO AND SOLARI_WITH_SQLITE)
  add_executable(test_monitor_report integration/test_monitor_report.c)
  target_link_libraries(test_monitor_report PRIVATE monitorcore unity)
  add_test(NAME test_monitor_report COMMAND test_monitor_report)

  add_executable(test_gossip integration/test_gossip.c)
  target_link_libraries(test_gossip PRIVATE monitorcore unity)
  add_test(NAME test_gossip COMMAND test_gossip)
endif()

# ---------------------------------------------------------------------------
# Server pure-logic unit tests (Phase 5). Each test #includes exactly ONE
# subsystem .c to reach its static helpers, then links libsolari + unity +
# libmariadb (serverDb.c pulls in the MariaDB client headers) and the weak
# server_stubs.c that satisfies the cross-subsystem symbols the included TU
# references in its non-pure entry points (which the tests never call). No live
# MariaDB or socket is required. Gated on SOLARI_BUILD_SERVER so the default
# build is unaffected.
if(TARGET servercore)
  set(SOLARI_SERVER_UNIT_TESTS
    test_server_db
    test_server_ingest
    test_server_provision
    test_server_alert
    test_server_topology
    test_server_topo_snmp
    test_server_discovery
    test_server_ctl
  )
  foreach(t ${SOLARI_SERVER_UNIT_TESTS})
    add_executable(${t} unit/${t}.c unit/server_stubs.c)
    target_compile_definitions(${t} PRIVATE _GNU_SOURCE)
    target_include_directories(${t} PRIVATE
      ${CMAKE_SOURCE_DIR}/include
      ${CMAKE_SOURCE_DIR}/src/server
      $<TARGET_PROPERTY:servercore,INTERFACE_INCLUDE_DIRECTORIES>)
    target_link_libraries(${t} PRIVATE solari unity
      $<TARGET_PROPERTY:servercore,INTERFACE_LINK_LIBRARIES>)
    add_test(NAME ${t} COMMAND ${t})
  endforeach()

  # Live-DB integration suite: links the real servercore library and drives the
  # actual prepared statements. Self-skips (exit 0) unless SOLARI_TEST_DB is set,
  # so it is harmless in CI without a database.
  add_executable(test_server_db_live integration/test_server_db_live.c)
  target_compile_definitions(test_server_db_live PRIVATE _GNU_SOURCE)
  target_include_directories(test_server_db_live PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src/server)
  target_link_libraries(test_server_db_live PRIVATE servercore unity)
  add_test(NAME test_server_db_live COMMAND test_server_db_live)

  # Pure SNMP IF-MIB parse/merge (serverSnmp.c). Links servercore for the
  # symbols; touches no DB or SNMP backend at runtime.
  add_executable(test_server_snmp unit/test_server_snmp.c)
  target_include_directories(test_server_snmp PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src/server)
  target_link_libraries(test_server_snmp PRIVATE servercore unity)
  add_test(NAME test_server_snmp COMMAND test_server_snmp)
endif()
