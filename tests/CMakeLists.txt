# Test tree. Unity (vendored under third_party/unity) is built once and each
# unit suite links it plus libsolari, then registers with ctest.

add_library(unity STATIC ${CMAKE_SOURCE_DIR}/third_party/unity/unity.c)
target_include_directories(unity PUBLIC ${CMAKE_SOURCE_DIR}/third_party/unity)
# Force 64-bit assertion support on every target ABI. Unity auto-enables it on
# 64-bit hosts but disables it on 32-bit ones (e.g. arm32), which would abort
# the u64 TLV/frame round-trip tests with "Unity 64-bit Support Disabled".
target_compile_definitions(unity PUBLIC UNITY_SUPPORT_64)

if(SOLARI_BUILD_FUZZERS)
  add_executable(fuzz_protocol fuzz/fuzz_protocol.c)
  target_link_libraries(fuzz_protocol PRIVATE solari)
  target_compile_options(fuzz_protocol PRIVATE
    -fsanitize=fuzzer-no-link,address,undefined -fno-omit-frame-pointer)
  target_link_options(fuzz_protocol PRIVATE -fsanitize=fuzzer,address,undefined)
  add_test(NAME fuzz_protocol_smoke COMMAND fuzz_protocol
    -runs=20000 -seed=20261020 -max_len=4096
    -dict=${CMAKE_CURRENT_SOURCE_DIR}/fuzz/protocol.dict)
endif()

set(SOLARI_UNIT_TESTS
  test_crypto
  test_tlv
  test_frame
  test_msg
  test_config
  test_json
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

# End-to-end enrollment installer validation against a local mock HTTP service.
# Requires the same host utilities as the production installer.
find_package(Python3 COMPONENTS Interpreter QUIET)
find_program(SOLARI_OPENSSL_EXECUTABLE openssl)
find_program(SOLARI_JQ_EXECUTABLE jq)
find_program(SOLARI_CURL_EXECUTABLE curl)
if(Python3_Interpreter_FOUND AND SOLARI_OPENSSL_EXECUTABLE)
  add_test(NAME test_certificate_backfill
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tools/test_backfill_certificate_issuance.py)
  add_test(NAME test_crl_generator
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/deploy/crl/test_generate_crl.py)
endif()
if(Python3_Interpreter_FOUND AND SOLARI_OPENSSL_EXECUTABLE AND
   SOLARI_JQ_EXECUTABLE AND SOLARI_CURL_EXECUTABLE)
  add_test(NAME test_enroll_install
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/integration/test_enroll_install.py)
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

# Client control-plane pure logic: blob apply, epoch monotonicity, addressing,
# CONTROL_RESULT wire shape. No transport needed (the SUB glue is gated inside
# clientControl.c), so it runs in the default build too.
if(TARGET clientcore)
  add_executable(test_client_control unit/test_client_control.c)
  target_link_libraries(test_client_control PRIVATE clientcore unity)
  add_test(NAME test_client_control COMMAND test_client_control)
  if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    add_executable(test_client_control_persistence unit/test_client_control_persistence.c)
    target_link_libraries(test_client_control_persistence PRIVATE clientcore unity)
    target_link_options(test_client_control_persistence PRIVATE
      "-Wl,--wrap=fsync" "-Wl,--wrap=rename")
    add_test(NAME test_client_control_persistence COMMAND test_client_control_persistence)
  endif()
endif()

# Config-push convergence end-to-end over loopback sockets: PUB->SUB directive,
# apply + persist, CONTROL_RESULT back over PUSH->PULL with correlation echo.
if(TARGET clientcore AND SOLARI_WITH_IO AND SOLARI_WITH_SQLITE)
  add_executable(test_control_e2e integration/test_control_e2e.c)
  target_link_libraries(test_control_e2e PRIVATE clientcore unity)
  add_test(NAME test_control_e2e COMMAND test_control_e2e)
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

  # Monitor control-plane pure logic: adopt + set-config/provision apply under
  # epoch monotonicity, addressing, CONTROL_RESULT wire shape.
  add_executable(test_monitor_control unit/test_monitor_control.c)
  target_link_libraries(test_monitor_control PRIVATE monitorcore unity)
  add_test(NAME test_monitor_control COMMAND test_monitor_control)
  if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    add_executable(test_monitor_control_persistence unit/test_monitor_control_persistence.c)
    target_link_libraries(test_monitor_control_persistence PRIVATE monitorcore unity)
    target_link_options(test_monitor_control_persistence PRIVATE
      "-Wl,--wrap=fsync" "-Wl,--wrap=rename")
    add_test(NAME test_monitor_control_persistence COMMAND test_monitor_control_persistence)
  endif()
endif()

# Monitor reporting + gossip - need the transport (and spool).
if(TARGET monitorcore AND SOLARI_WITH_IO AND SOLARI_WITH_SQLITE)
  add_executable(test_monitor_report integration/test_monitor_report.c)
  target_link_libraries(test_monitor_report PRIVATE monitorcore unity)
  add_test(NAME test_monitor_report COMMAND test_monitor_report)

  add_executable(test_gossip integration/test_gossip.c)
  target_link_libraries(test_gossip PRIVATE monitorcore unity)
  add_test(NAME test_gossip
    COMMAND bash ${CMAKE_SOURCE_DIR}/tests/integration/test_gossip.sh $<TARGET_FILE:test_gossip>)
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
    test_server_scan
    test_server_ctl
  )
  foreach(t ${SOLARI_SERVER_UNIT_TESTS})
    add_executable(${t} unit/${t}.c unit/server_stubs.c)
    target_compile_definitions(${t} PRIVATE _GNU_SOURCE)
    if(t STREQUAL "test_server_scan")
      # Exercise the optional process-backed enrichment path with hermetic fake
      # tools; the production servercore option remains off by default.
      target_compile_definitions(${t} PRIVATE
        SOLARI_WITH_DISCOVERY_TOOLS=1 SCAN_TOOL_TIMEOUT_MS=500)
      target_sources(${t} PRIVATE
        ${CMAKE_SOURCE_DIR}/src/server/serverDiscoveryEnrich.c
        ${CMAKE_SOURCE_DIR}/src/server/serverOui.c)
    endif()
    if(t STREQUAL "test_server_topo_snmp")
      # Exercise the production LLDP CLI driver with a fake snmpbulkwalk in the
      # test fixture; never query a real device from unit tests.
      target_compile_definitions(${t} PRIVATE SOLARI_WITH_SNMP=1)
    endif()
    target_include_directories(${t} PRIVATE
      ${CMAKE_SOURCE_DIR}/include
      ${CMAKE_SOURCE_DIR}/src/server
      $<TARGET_PROPERTY:servercore,INTERFACE_INCLUDE_DIRECTORIES>)
    target_link_libraries(${t} PRIVATE solari unity
      $<TARGET_PROPERTY:servercore,INTERFACE_LINK_LIBRARIES>)
    add_test(NAME ${t} COMMAND ${t})
  endforeach()

  add_executable(test_server_control_result
    unit/test_server_control_result.c unit/server_stubs.c
    ${CMAKE_SOURCE_DIR}/src/server/serverControl.c)
  target_include_directories(test_server_control_result PRIVATE
    ${CMAKE_SOURCE_DIR}/include ${CMAKE_SOURCE_DIR}/src/server)
  target_link_libraries(test_server_control_result PRIVATE solari unity)
  add_test(NAME test_server_control_result COMMAND test_server_control_result)

  # Exercise the production mbedTLS enrollment signer with generated CA/CSR
  # material. This covers identity rejection and preserves the SAN in the leaf.
  if(SOLARI_WITH_IO AND SOLARI_HAVE_MBEDTLS_CSR_SAN_API)
    add_executable(test_server_csr_sign integration/test_server_csr_sign.c)
    target_include_directories(test_server_csr_sign PRIVATE
      ${CMAKE_SOURCE_DIR}/include ${CMAKE_SOURCE_DIR}/src/server)
    target_link_libraries(test_server_csr_sign PRIVATE servercore)
    add_test(NAME test_server_csr_sign
      COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/integration/test_server_csr_sign.sh
                   $<TARGET_FILE:test_server_csr_sign>)
  endif()

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

  # A real C control-socket process used by the authenticated dashboard
  # discovery acceptance test. It is launched only against that test's
  # dedicated monitoring schema; no live transport or monitor is attached.
  add_executable(test_ctl_adopt_bridge integration/test_ctl_adopt_bridge.c)
  target_include_directories(test_ctl_adopt_bridge PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src/server)
  target_link_libraries(test_ctl_adopt_bridge PRIVATE servercore)

  if(Python3_Interpreter_FOUND AND TARGET solariCtlClient)
    add_test(NAME test_ctl_cli
      COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/integration/test_ctl_cli.py
              $<TARGET_FILE:solariCtlClient>)
  endif()

  # Pure SNMP IF-MIB parse/merge (serverSnmp.c). Links servercore for the
  # symbols; touches no DB or SNMP backend at runtime.
  add_executable(test_server_snmp unit/test_server_snmp.c)
  target_include_directories(test_server_snmp PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src/server)
  target_link_libraries(test_server_snmp PRIVATE servercore unity)
  add_test(NAME test_server_snmp COMMAND test_server_snmp)

  add_executable(test_server_discovery_enrich unit/test_server_discovery_enrich.c)
  target_include_directories(test_server_discovery_enrich PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src/server)
  target_link_libraries(test_server_discovery_enrich PRIVATE servercore unity)
  add_test(NAME test_server_discovery_enrich COMMAND test_server_discovery_enrich)
endif()
