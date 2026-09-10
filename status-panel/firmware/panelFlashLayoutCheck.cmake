# Reserved-sector guard: the image must not reach the final reserved 4 KB
# sectors — credential slots A/B + spare (panelProvStore.c, CONTRACT-SW §13
# D5) below the AW screen-config sector (panelScreenCfg.c, CONTRACT-AW §10.4).
# The sector count comes from PANEL_PROV_RESERVED_SECTORS in panelProvStore.h
# so the C map and this guard cannot drift apart (D5: shared definition).
if(NOT DEFINED IMAGE OR NOT DEFINED FLASH_BYTES OR NOT DEFINED SECTOR_BYTES OR NOT DEFINED PROV_HEADER)
  message(FATAL_ERROR "panel flash-layout guard missing IMAGE/FLASH_BYTES/SECTOR_BYTES/PROV_HEADER")
endif()
if(NOT EXISTS "${IMAGE}")
  message(FATAL_ERROR "panel flash-layout guard cannot find image: ${IMAGE}")
endif()
file(STRINGS "${PROV_HEADER}" reservedLine REGEX "#define PANEL_PROV_RESERVED_SECTORS ")
if(NOT reservedLine MATCHES "PANEL_PROV_RESERVED_SECTORS ([0-9]+)u")
  message(FATAL_ERROR "PANEL_PROV_RESERVED_SECTORS not found in ${PROV_HEADER}")
endif()
set(reservedSectors ${CMAKE_MATCH_1})
file(SIZE "${IMAGE}" imageSize)
math(EXPR maxImageSize "${FLASH_BYTES} - ${reservedSectors} * ${SECTOR_BYTES}")
if(imageSize GREATER maxImageSize)
  message(FATAL_ERROR
    "solari-panel-fw image (${imageSize} bytes) overlaps the final reserved ${reservedSectors} x ${SECTOR_BYTES}-byte config/credential sectors (maximum ${maxImageSize})")
endif()
