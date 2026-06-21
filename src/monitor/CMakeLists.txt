# solariMonitor (sec 8). Linux-only (sec 12.2). The probe engine + HRW + config
# build into a `monitorcore` static lib that both the executable and the tests
# link. Needs only libsolari's core (crypto/time/log/config), so it builds
# without the heavy I/O options; reporting (push-or-spool) extends it next.

if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
  message(STATUS "solariMonitor: Linux-only; skipping on ${CMAKE_SYSTEM_NAME}")
  return()
endif()

set(MONITOR_CORE_SOURCES
  probeNet.c
  monitorPeer.c
  monitorConfig.c
  monitorControl.c
)
set(MONITOR_HAS_REPORTING OFF)
if(SOLARI_WITH_IO AND SOLARI_WITH_SQLITE)
  list(APPEND MONITOR_CORE_SOURCES monitorReport.c monitorGossip.c)
  set(MONITOR_HAS_REPORTING ON)
endif()

add_library(monitorcore STATIC ${MONITOR_CORE_SOURCES})
target_include_directories(monitorcore PUBLIC
  ${CMAKE_SOURCE_DIR}/include
  ${CMAKE_CURRENT_SOURCE_DIR}
)
target_link_libraries(monitorcore PUBLIC solari)
if(MONITOR_HAS_REPORTING)
  target_compile_definitions(monitorcore PUBLIC MONITOR_WITH_REPORTING=1)
endif()

add_executable(solariMonitor main.c)
target_link_libraries(solariMonitor PRIVATE monitorcore)
