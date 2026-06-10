# Test tree. Unity (vendored under third_party/unity) is built once and each
# unit suite links it plus libsolari, then registers with ctest.

add_library(unity STATIC ${CMAKE_SOURCE_DIR}/third_party/unity/unity.c)
target_include_directories(unity PUBLIC ${CMAKE_SOURCE_DIR}/third_party/unity)

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
