# Test tree. Unity (vendored under third_party/unity) is built once and each
# unit suite links it plus libsolari, then registers with ctest.

add_library(unity STATIC ${CMAKE_SOURCE_DIR}/third_party/unity/unity.c)
target_include_directories(unity PUBLIC ${CMAKE_SOURCE_DIR}/third_party/unity)

set(SOLARI_UNIT_TESTS
  test_crypto
  test_tlv
  test_frame
  test_msg
)

foreach(t ${SOLARI_UNIT_TESTS})
  add_executable(${t} unit/${t}.c)
  target_link_libraries(${t} PRIVATE solari unity)
  target_include_directories(${t} PRIVATE ${CMAKE_SOURCE_DIR}/include)
  add_test(NAME ${t} COMMAND ${t})
endforeach()
