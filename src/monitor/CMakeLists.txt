# solariMonitor (sec 8). Linux-only (sec 12.2). The probe engine + HRW + config
# build into a `monitorcore` static lib that both the executable and the tests
# link. Needs only libsolari's core (crypto/time/log/config), so it builds
# without the heavy I/O options; reporting (push-or-spool) extends it next.

if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
  message(STATUS "solariMonitor: Linux-only; skipping on ${CMAKE_SYSTEM_NAME}")
  return()
endif()

add_library(monitorcore STATIC
  probeNet.c
  monitorPeer.c
  monitorConfig.c
)
target_include_directories(monitorcore PUBLIC
  ${CMAKE_SOURCE_DIR}/include
  ${CMAKE_CURRENT_SOURCE_DIR}
)
target_link_libraries(monitorcore PUBLIC solari)

add_executable(solariMonitor main.c)
target_link_libraries(solariMonitor PRIVATE monitorcore)
