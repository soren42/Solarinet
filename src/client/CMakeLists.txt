# solariClient (sec 7). The platform-agnostic core (collection, reporting, PAL)
# is built into a static `clientcore` lib so both the executable and the test
# suite link the same objects. The PAL is chosen by target OS; the reporting
# layer (clientReport.c) compiles only when libsolari carries the nng transport
# and the SQLite spool.

set(CLIENT_CORE_SOURCES clientCollect.c)

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  list(APPEND CLIENT_CORE_SOURCES plat/platLinux.c)
elseif(CMAKE_SYSTEM_NAME STREQUAL "Darwin")
  list(APPEND CLIENT_CORE_SOURCES plat/platMacos.c)     # macOS PAL step
elseif(CMAKE_SYSTEM_NAME STREQUAL "Windows")
  list(APPEND CLIENT_CORE_SOURCES plat/platWindows.c)   # Windows PAL step
else()
  message(WARNING "solariClient: no PAL for ${CMAKE_SYSTEM_NAME}")
endif()

set(CLIENT_HAS_REPORTING OFF)
if(SOLARI_WITH_IO AND SOLARI_WITH_SQLITE)
  list(APPEND CLIENT_CORE_SOURCES clientReport.c)
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
