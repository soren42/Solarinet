# solariClient (sec 7). Builds across the OS x arch matrix; the platform
# implementation is selected from the target OS via the PAL seam (platOS.h).
#
# This increment links the Linux PAL + collection + a runnable skeleton; the
# report-send/watchdog/CONTROL loop (clientReport.c, clientWatchdog.c) extends
# this target next. The client needs only libsolari's core (config/log/time),
# so it builds even with the heavy I/O options off.

set(CLIENT_SOURCES
  main.c
  clientCollect.c
)

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  list(APPEND CLIENT_SOURCES plat/platLinux.c)
elseif(CMAKE_SYSTEM_NAME STREQUAL "Darwin")
  list(APPEND CLIENT_SOURCES plat/platMacos.c)     # arrives in the macOS PAL step
elseif(CMAKE_SYSTEM_NAME STREQUAL "Windows")
  list(APPEND CLIENT_SOURCES plat/platWindows.c)   # arrives in the Windows PAL step
else()
  message(WARNING "solariClient: no PAL for ${CMAKE_SYSTEM_NAME}; skipping")
endif()

add_executable(solariClient ${CLIENT_SOURCES})
target_include_directories(solariClient PRIVATE
  ${CMAKE_SOURCE_DIR}/include
  ${CMAKE_CURRENT_SOURCE_DIR}
)
target_link_libraries(solariClient PRIVATE solari)
