# Reserved-sector guard: the image must not reach the final FOUR 4 KB
# sectors — credential slots A/B + spare (panelProvStore.c, CONTRACT-SW §13
# D5) below the AW screen-config sector (panelScreenCfg.c, CONTRACT-AW §10.4).
if(NOT DEFINED IMAGE OR NOT DEFINED FLASH_BYTES OR NOT DEFINED SECTOR_BYTES)
  message(FATAL_ERROR "panel flash-layout guard missing IMAGE/FLASH_BYTES/SECTOR_BYTES")
endif()
if(NOT EXISTS "${IMAGE}")
  message(FATAL_ERROR "panel flash-layout guard cannot find image: ${IMAGE}")
endif()
file(SIZE "${IMAGE}" imageSize)
math(EXPR maxImageSize "${FLASH_BYTES} - 4 * ${SECTOR_BYTES}")
if(imageSize GREATER maxImageSize)
  message(FATAL_ERROR
    "solari-panel-fw image (${imageSize} bytes) overlaps the final reserved 4 x ${SECTOR_BYTES}-byte config/credential sectors (maximum ${maxImageSize})")
endif()
