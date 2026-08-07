# CONTRACT-AW §10.4 reserved-sector guard: the image must not reach the
# final 4 KB sector used by panelScreenCfg's CRC-protected record.
if(NOT DEFINED IMAGE OR NOT DEFINED FLASH_BYTES OR NOT DEFINED SECTOR_BYTES)
  message(FATAL_ERROR "panel flash-layout guard missing IMAGE/FLASH_BYTES/SECTOR_BYTES")
endif()
if(NOT EXISTS "${IMAGE}")
  message(FATAL_ERROR "panel flash-layout guard cannot find image: ${IMAGE}")
endif()
file(SIZE "${IMAGE}" imageSize)
math(EXPR maxImageSize "${FLASH_BYTES} - ${SECTOR_BYTES}")
if(imageSize GREATER maxImageSize)
  message(FATAL_ERROR
    "solari-panel-fw image (${imageSize} bytes) overlaps the final reserved ${SECTOR_BYTES}-byte config sector (maximum ${maxImageSize})")
endif()
