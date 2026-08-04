cmake_minimum_required(VERSION 3.13)

# ---------------------------------------------------------------------------
# solari-panel-fw — SolariNet status panel firmware for the Pimoroni Galactic
# Unicorn (Pico 2 W / RP2350, 53x11 RGB matrix).
#
# Board is VERIFIED hardware, not assumed: the attached unit's MicroPython REPL
# reports "Raspberry Pi Pico2 W (Galactic Unicorn) with RP2350", so PICO_BOARD
# is pinned to pico2_w.
#
# Reproducible builds: SOURCE_DATE_EPOCH is pinned below and the build string is
# derived from it, so no __DATE__/__TIME__ ever enters the image.
# ---------------------------------------------------------------------------

set(PICO_BOARD pico2_w CACHE STRING "Pico board")
set(PICO_PLATFORM rp2350 CACHE STRING "Pico platform")

# Pinned epoch for reproducibility (CONTRACT.md v1.1 §9 acceptance 3).
# 2026-08-04T00:00:00Z. Override only with a deliberate release bump.
set(SOLARI_SOURCE_DATE_EPOCH "1785801600" CACHE STRING "Pinned build epoch")
set(ENV{SOURCE_DATE_EPOCH} "${SOLARI_SOURCE_DATE_EPOCH}")

include(pico_sdk_import.cmake)
include(pimoroni_pico_import.cmake)

project(solari-panel-fw C CXX ASM)
set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

pico_sdk_init()

# The C++ surface here is one wrapper file over the Pimoroni driver: no
# exceptions, no RTTI, no STL, no operator new. Debian trixie ships the
# arm-none-eabi C++ headers (libstdc++-arm-none-eabi-dev) but not the runtime
# archive (libstdc++-arm-none-eabi-newlib is not installed), and this firmware
# has no business pulling one in. CMake drives the link with arm-none-eabi-g++
# because the target has a C++ TU, and the g++ driver appends -lstdc++ on its
# own below CMake's link line, so -nostdlib++ is the way to suppress it. Note
# this drops ONLY libstdc++: libm/libc/libgcc still come in as usual. If a
# future change starts using the STL the link fails loudly, which is correct.
add_link_options(-nostdlib++)

# Shared wire codec, owned by the Lead, consumed verbatim (CONTRACT §9).
set(SOLARI_PROTOCOL_DIR "${CMAKE_CURRENT_LIST_DIR}/.." CACHE PATH
    "Directory holding protocol.h / protocol.c")
set(SOLARI_PROTOCOL_C "${SOLARI_PROTOCOL_DIR}/protocol.c")
if(NOT EXISTS "${SOLARI_PROTOCOL_C}")
  # Peer C2 owns protocol.c. Until it lands, compile-test against a local stub
  # so the firmware can be built and reviewed; the stub is NEVER shipped.
  message(WARNING
    "protocol.c not found at ${SOLARI_PROTOCOL_C} — building with "
    "protocol_stub.c (COMPILE-TEST ONLY, not a shippable image)")
  set(SOLARI_PROTOCOL_C "${CMAKE_CURRENT_LIST_DIR}/protocol_stub.c")
  add_compile_definitions(SOLARI_PROTOCOL_STUBBED=1)
endif()

add_executable(solari-panel-fw
  main.c
  panelFb.c
  panelFont.c
  panelHist.c
  panelScreensA.c
  panelScreensB.c
  panelScreensC.c
  panelScreensD.c
  panelInlay.c
  panelHw.cpp
  "${SOLARI_PROTOCOL_C}"
)

target_include_directories(solari-panel-fw PRIVATE
  ${CMAKE_CURRENT_LIST_DIR}
  ${SOLARI_PROTOCOL_DIR}
)

# Build identity: derived from the pinned epoch, never from __DATE__/__TIME__.
string(TIMESTAMP SOLARI_BUILD_STAMP "%Y%m%d" UTC)
target_compile_definitions(solari-panel-fw PRIVATE
  SOLARI_FW_VERSION_MAJOR=1
  SOLARI_FW_VERSION_MINOR=0
  SOLARI_BUILD_EPOCH=${SOLARI_SOURCE_DATE_EPOCH}
  PICO_STDIO_ENABLE_CRLF_SUPPORT=0   # binary-safe stdio: no LF->CRLF mangling
  PICO_STDIO_DEFAULT_CRLF=0
  PICO_STDIO_USB_SUPPORT_CHARS_AVAILABLE_CALLBACK=0
)

# galactic_unicorn.cmake pulls in pico_graphics, which pulls in hershey_fonts.
# hershey_fonts is an INTERFACE library, so its translation unit is compiled
# straight into this target and its file-scope std::map<std::string,...> runs a
# global constructor at startup -- dragging the whole STL in unconditionally.
# This firmware paints through GalacticUnicorn::set_pixel() and never touches
# PicoGraphics or a Hershey font, so pre-declare hershey_fonts as headers-only.
# The guard in pico_graphics.cmake (if NOT TARGET hershey_fonts) then leaves it
# alone. pico_graphics itself is a static library: unreferenced, it is never
# pulled from the archive.
add_library(hershey_fonts INTERFACE)
target_include_directories(hershey_fonts INTERFACE
  ${PIMORONI_PICO_PATH}/libraries/hershey_fonts)

include(${PIMORONI_PICO_PATH}/libraries/galactic_unicorn/galactic_unicorn.cmake)

target_link_libraries(solari-panel-fw
  pico_stdlib
  pico_unique_id
  hardware_adc
  hardware_pio
  hardware_dma
  galactic_unicorn
  m                # newlib libm: sqrtf/powf/log10f in the screen renderers
)

# USB CDC is the data link (CONTRACT §4); UART stdio would fight it for stdout.
pico_enable_stdio_usb(solari-panel-fw 1)
pico_enable_stdio_uart(solari-panel-fw 0)

# Strip absolute paths so the image does not depend on the build directory.
target_compile_options(solari-panel-fw PRIVATE
  -Wall -Wextra -Wno-unused-parameter
  $<$<COMPILE_LANGUAGE:CXX>:-fno-exceptions;-fno-rtti;-fno-use-cxa-atexit>
  -ffile-prefix-map=${CMAKE_CURRENT_LIST_DIR}=.
  -ffile-prefix-map=${PICO_SDK_PATH}=/pico-sdk
  -ffile-prefix-map=${PIMORONI_PICO_PATH}=/pimoroni-pico
)

pico_add_extra_outputs(solari-panel-fw)
