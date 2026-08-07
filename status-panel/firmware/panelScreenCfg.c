/* panelScreenCfg.c — CRC-protected screen configuration and flash seam.
 * CONTRACT-AW.md §3.4 requires the final 4 KB sector on-device. The record
 * encoder below is deliberately independent of that hardware operation. */

#include "panelScreenCfg.h"

#include <string.h>

#include "../protocol.h"

#ifdef SOLARI_PANEL_PICO_FLASH
#include "hardware/flash.h"
#include "pico/flash.h"
#include "pico/platform.h"
#endif

#define PANEL_SCREEN_CFG_MAGIC 0x53434647u
#define PANEL_SCREEN_CFG_VERSION 1u
#define PANEL_SCREEN_CFG_RECORD_SIZE 19u

/* Purpose: initialize the factory-safe all-enabled, 1x configuration.
 * Input: config/current clock. Output: a clean default configuration. */
static void panelScreenCfgDefaults(PanelScreenCfg *cfg, uint32_t nowMs) {
  uint8_t i;
  for (i = 0u; i < PANEL_SCREEN_COUNT; ++i) cfg->screenCfg[i] = 0x05u;
  cfg->changedMs = nowMs;
  cfg->dirty = false;
  cfg->persisted = false;
}

/* Purpose: encode the fixed persisted record without native struct padding.
 * Input: config/output record. Output: none; CRC uses protocol.h's canonical CRC. */
static void panelScreenCfgEncode(const PanelScreenCfg *cfg, uint8_t out[PANEL_SCREEN_CFG_RECORD_SIZE]) {
  uint16_t crc;
  out[0] = (uint8_t)PANEL_SCREEN_CFG_MAGIC;
  out[1] = (uint8_t)(PANEL_SCREEN_CFG_MAGIC >> 8);
  out[2] = (uint8_t)(PANEL_SCREEN_CFG_MAGIC >> 16);
  out[3] = (uint8_t)(PANEL_SCREEN_CFG_MAGIC >> 24);
  out[4] = PANEL_SCREEN_CFG_VERSION;
  memcpy(out + 5u, cfg->screenCfg, PANEL_SCREEN_COUNT);
  crc = panelCrc16(out, 17u);
  out[17] = (uint8_t)crc;
  out[18] = (uint8_t)(crc >> 8);
}

/* Purpose: validate and decode a persisted record.
 * Input: record/config. Output: true only for a complete valid record. */
static bool panelScreenCfgDecode(const uint8_t in[PANEL_SCREEN_CFG_RECORD_SIZE],
                                 PanelScreenCfg *cfg) {
  uint8_t i;
  uint16_t crc = (uint16_t)in[17] | ((uint16_t)in[18] << 8);
  if (in[0] != (uint8_t)PANEL_SCREEN_CFG_MAGIC ||
      in[1] != (uint8_t)(PANEL_SCREEN_CFG_MAGIC >> 8) ||
      in[2] != (uint8_t)(PANEL_SCREEN_CFG_MAGIC >> 16) ||
      in[3] != (uint8_t)(PANEL_SCREEN_CFG_MAGIC >> 24) ||
      in[4] != PANEL_SCREEN_CFG_VERSION || panelCrc16(in, 17u) != crc) return false;
  for (i = 0u; i < PANEL_SCREEN_COUNT; ++i)
    if ((in[5u + i] & 0xF0u) != 0u || ((in[5u + i] >> 1) & 0x07u) > 5u) return false;
  memcpy(cfg->screenCfg, in + 5u, PANEL_SCREEN_COUNT);
  cfg->dirty = false;
  cfg->persisted = true;
  return true;
}

void panelScreenCfgLoad(PanelScreenCfg *cfg, const PanelScreenCfgFlash *flash,
                        uint32_t nowMs) {
  uint8_t record[PANEL_SCREEN_CFG_RECORD_SIZE];
  if (cfg == NULL) return;
  panelScreenCfgDefaults(cfg, nowMs);
  if (flash == NULL || flash->read == NULL || !flash->read(flash->user, record, sizeof(record))) return;
  if (!panelScreenCfgDecode(record, cfg)) panelScreenCfgDefaults(cfg, nowMs);
}

bool panelScreenCfgSet(PanelScreenCfg *cfg, uint8_t index, uint8_t value,
                       uint32_t nowMs) {
  if (cfg == NULL || index >= PANEL_SCREEN_COUNT || (value & 0xF0u) != 0u ||
      ((value >> 1) & 0x07u) > 5u || cfg->screenCfg[index] == value) return false;
  cfg->screenCfg[index] = value;
  cfg->changedMs = nowMs;
  cfg->dirty = true;
  cfg->persisted = false;
  return true;
}

bool panelScreenCfgSaveDue(PanelScreenCfg *cfg, const PanelScreenCfgFlash *flash,
                           uint32_t nowMs) {
  uint8_t record[PANEL_SCREEN_CFG_RECORD_SIZE];
  if (cfg == NULL || !cfg->dirty || flash == NULL || flash->writeSector == NULL ||
      (uint32_t)(nowMs - cfg->changedMs) < PANEL_SCREEN_CFG_DEBOUNCE_MS) return false;
  panelScreenCfgEncode(cfg, record);
  if (!flash->writeSector(flash->user, record, sizeof(record))) {
    /* Review S2: a failing write must not retry at frame rate forever —
     * push changedMs forward so the next attempt waits a full debounce. */
    cfg->changedMs = nowMs;
    return false;
  }
  cfg->dirty = false;
  cfg->persisted = true;
  return true;
}

bool panelScreenCfgEnabled(const PanelScreenCfg *cfg, uint8_t theme,
                           uint8_t screen) {
  uint8_t i, base;
  if (cfg == NULL || theme > 3u || screen > 2u) return true;
  base = (uint8_t)(theme * 3u);
  for (i = 0u; i < 3u; ++i)
    if ((cfg->screenCfg[base + i] & 1u) != 0u) return (cfg->screenCfg[base + screen] & 1u) != 0u;
  return true; /* CONTRACT-AW §4: never leave an all-disabled theme black. */
}

uint8_t panelScreenCfgNext(const PanelScreenCfg *cfg, uint8_t theme,
                           uint8_t screen) {
  uint8_t i;
  for (i = 1u; i <= 3u; ++i) {
    uint8_t candidate = (uint8_t)((screen + i) % 3u);
    if (panelScreenCfgEnabled(cfg, theme, candidate)) return candidate;
  }
  return screen;
}

float panelScreenCfgDwellSec(const PanelScreenCfg *cfg, uint8_t index,
                             float baseSec) {
  static const float weights[6] = { 0.25f, 0.5f, 1.0f, 2.0f, 5.0f, 10.0f };
  uint8_t code = 2u;
  if (cfg != NULL && index < PANEL_SCREEN_COUNT) code = (uint8_t)((cfg->screenCfg[index] >> 1) & 0x07u);
  if (code > 5u) code = 2u;
  return baseSec * weights[code];
}

#ifdef SOLARI_PANEL_PICO_FLASH
typedef struct {
  uint8_t page[FLASH_PAGE_SIZE];
} PanelScreenCfgFlashWrite;

/* Purpose: erase and program the final flash sector while pico_flash is safe.
 * Input: page-aligned write request. Output: pico flash-safe status.
 * This SRAM callback only erases/programs its stack-passed page; it does not
 * allocate, block, or access flash-backed state while interrupts are disabled. */
/* pico-sdk flash_safe_execute callback: void(*)(void*) — runs with the
 * other core locked out and XIP suspended; keep it minimal and SRAM-safe. */
static void __no_inline_not_in_flash_func(panelScreenCfgFlashSafeWrite)(void *param) {
  PanelScreenCfgFlashWrite *write = (PanelScreenCfgFlashWrite *)param;
  uint32_t offset = PICO_FLASH_SIZE_BYTES - FLASH_SECTOR_SIZE;
  flash_range_erase(offset, FLASH_SECTOR_SIZE);
  flash_range_program(offset, write->page, FLASH_PAGE_SIZE);
}

/* Purpose: read the first persisted record bytes from the last flash sector.
 * Input: unused user/output buffer/length. Output: false for oversized reads. */
static bool panelScreenCfgFlashRead(void *user, uint8_t *out, size_t len) {
  const uint8_t *flash = (const uint8_t *)(XIP_BASE + PICO_FLASH_SIZE_BYTES - FLASH_SECTOR_SIZE);
  (void)user;
  if (out == NULL || len > FLASH_PAGE_SIZE) return false;
  memcpy(out, flash, len);
  return true;
}

/* Purpose: persist one compact record via pico-sdk's dual-core flash guard.
 * Input: unused user/record bytes/length. Output: true only on successful write. */
static bool panelScreenCfgFlashWrite(void *user, const uint8_t *data, size_t len) {
  PanelScreenCfgFlashWrite write;
  (void)user;
  if (data == NULL || len > sizeof(write.page)) return false;
  memset(write.page, 0xff, sizeof(write.page));
  memcpy(write.page, data, len);
  return flash_safe_execute(panelScreenCfgFlashSafeWrite, &write, 3000u) == PICO_OK;
}
#endif

PanelScreenCfgFlash panelScreenCfgDeviceFlash(void) {
  PanelScreenCfgFlash flash;
  flash.read = NULL;
  flash.writeSector = NULL;
  flash.user = NULL;
#ifdef SOLARI_PANEL_PICO_FLASH
  flash.read = panelScreenCfgFlashRead;
  flash.writeSector = panelScreenCfgFlashWrite;
#endif
  return flash;
}

/* Purpose: §10 D5 dwell rescale — the ONE shipped implementation (review
 * finding: the test previously asserted a local reimplementation while
 * main.c carried its own copy in no test binary). */
float panelScreenCfgRescaleDwell(float elapsed, float oldDwell,
                                 float newDwell, int *advanced) {
  if (advanced != NULL) *advanced = 0;
  if (oldDwell > 0.0f) elapsed = elapsed / oldDwell * newDwell;
  if (elapsed >= newDwell) {
    if (advanced != NULL) *advanced = 1;
    return 0.0f;
  }
  return elapsed;
}
