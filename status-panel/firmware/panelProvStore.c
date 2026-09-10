/* panelProvStore.c — A/B credential record store (CONTRACT-SW §13 D5).
 * Record format and commit discipline documented in panelProvStore.h. */

#include "panelProvStore.h"

#include <string.h>

#include "../protocol.h"

#ifdef SOLARI_PANEL_PICO_FLASH
#include "hardware/flash.h"
#include "pico/flash.h"
#include "pico/platform.h"
#endif

uint32_t panelProvCrc32(const uint8_t *data, size_t len) {
  uint32_t crc = 0xFFFFFFFFu;
  size_t i;
  int b;
  for (i = 0u; i < len; ++i) {
    crc ^= data[i];
    for (b = 0; b < 8; ++b)
      crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
  }
  return crc ^ 0xFFFFFFFFu;
}

static void putLe16(uint8_t *p, uint16_t v) {
  p[0] = (uint8_t)v;
  p[1] = (uint8_t)(v >> 8);
}

static void putLe32(uint8_t *p, uint32_t v) {
  p[0] = (uint8_t)v;
  p[1] = (uint8_t)(v >> 8);
  p[2] = (uint8_t)(v >> 16);
  p[3] = (uint8_t)(v >> 24);
}

static uint16_t getLe16(const uint8_t *p) {
  return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static uint32_t getLe32(const uint8_t *p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
         ((uint32_t)p[3] << 24);
}

/* Purpose: validate one raw slot image and unpack it into the store shape.
 * Input: raw sector bytes / output fields. Output: true only for a record
 * whose magic, version, lengths and CRC are all intact. */
static bool decodeRecord(const uint8_t *raw, uint32_t *generation,
                         uint16_t len[PANEL_PROV_ITEM_COUNT],
                         uint16_t off[PANEL_PROV_ITEM_COUNT],
                         uint8_t payload[PANEL_PROV_RECORD_CAP]) {
  /* Per-item D3 wire bounds (itemId-1 indexed; commit/wipe carry no data).
   * A CRC-valid record with an out-of-bounds length is still refused — CRC
   * proves integrity, not that the writer honored the contract. */
  static const uint16_t kMax[PANEL_PROV_ITEM_COUNT] = {
    PANEL_PROV_MAX_SSID, PANEL_PROV_MAX_PSK,        PANEL_PROV_MAX_HOST,
    2u,                  PANEL_PROV_MAX_CACERT,     PANEL_PROV_MAX_CLIENTCERT,
    PANEL_PROV_MAX_CLIENTKEY, 0u, 0u,               PANEL_PROV_MAX_HOST
  };
  size_t total = 0u, i;
  if (getLe32(raw) != PANEL_PROV_RECORD_MAGIC ||
      raw[4] != PANEL_PROV_RECORD_VERSION || raw[5] != 0u) return false;
  for (i = 0u; i < PANEL_PROV_ITEM_COUNT; ++i) {
    len[i] = getLe16(raw + 10u + 2u * i);
    if (len[i] > kMax[i]) return false;
    off[i] = (uint16_t)total;
    total += len[i];
    if (total > PANEL_PROV_RECORD_CAP) return false;
  }
  if (panelProvCrc32(raw, PANEL_PROV_RECORD_HDR + total) !=
      getLe32(raw + PANEL_PROV_RECORD_HDR + total)) return false;
  *generation = getLe32(raw + 6u);
  memcpy(payload, raw + PANEL_PROV_RECORD_HDR, total);
  return true;
}

/* Purpose: serialize the itemLen/itemData table into one slot image.
 * Input: staged items / output sector buffer. Output: full record size, or 0
 * when the serialized record exceeds one sector (storage-full). */
static size_t encodeRecord(uint32_t generation,
                           const uint16_t itemLen[PANEL_PROV_ITEM_COUNT],
                           const uint8_t *const itemData[PANEL_PROV_ITEM_COUNT],
                           uint8_t raw[PANEL_PROV_SECTOR_SIZE]) {
  size_t total = 0u, i;
  for (i = 0u; i < PANEL_PROV_ITEM_COUNT; ++i) total += itemLen[i];
  if (total > PANEL_PROV_RECORD_CAP) return 0u;
  putLe32(raw, PANEL_PROV_RECORD_MAGIC);
  raw[4] = PANEL_PROV_RECORD_VERSION;
  raw[5] = 0u;
  putLe32(raw + 6u, generation);
  total = 0u;
  for (i = 0u; i < PANEL_PROV_ITEM_COUNT; ++i) {
    putLe16(raw + 10u + 2u * i, itemLen[i]);
    if (itemLen[i] != 0u)
      memcpy(raw + PANEL_PROV_RECORD_HDR + total, itemData[i], itemLen[i]);
    total += itemLen[i];
  }
  putLe32(raw + PANEL_PROV_RECORD_HDR + total,
          panelProvCrc32(raw, PANEL_PROV_RECORD_HDR + total));
  return PANEL_PROV_RECORD_HDR + total + PANEL_PROV_RECORD_CRC;
}

bool panelProvStoreLoad(PanelProvStore *store, const PanelProvFlash *flash) {
  /* One sector of scratch; static because RP2350 main-loop stacks are small
   * and this only runs single-threaded (boot + lockstep provisioning). */
  static uint8_t raw[PANEL_PROV_SECTOR_SIZE];
  uint8_t slot;
  if (store == NULL) return false;
  memset(store, 0, sizeof(*store));
  if (flash == NULL || flash->readSlot == NULL) return false;
  for (slot = 0u; slot < 2u; ++slot) {
    uint32_t generation;
    uint16_t len[PANEL_PROV_ITEM_COUNT], off[PANEL_PROV_ITEM_COUNT];
    static uint8_t payload[PANEL_PROV_RECORD_CAP];
    if (!flash->readSlot(flash->user, slot, raw, sizeof(raw))) continue;
    if (!decodeRecord(raw, &generation, len, off, payload)) continue;
    /* Highest generation wins, serial-number arithmetic so a u32 wrap (never
     * reached in device life, but cheap to be correct about) still elects the
     * newer record; a tie (which the commit discipline never produces) keeps
     * the earlier slot deterministically. */
    if (store->valid && (int32_t)(generation - store->generation) <= 0)
      continue;
    store->valid = true;
    store->activeSlot = slot;
    store->generation = generation;
    memcpy(store->len, len, sizeof(len));
    memcpy(store->off, off, sizeof(off));
    memcpy(store->payload, payload, sizeof(payload));
  }
  return store->valid;
}

int panelProvStoreCommit(PanelProvStore *store, const PanelProvFlash *flash,
                         const uint16_t itemLen[PANEL_PROV_ITEM_COUNT],
                         const uint8_t *const itemData[PANEL_PROV_ITEM_COUNT]) {
  static uint8_t raw[PANEL_PROV_SECTOR_SIZE];
  static uint8_t verify[PANEL_PROV_SECTOR_SIZE];
  uint32_t generation;
  uint16_t vLen[PANEL_PROV_ITEM_COUNT], vOff[PANEL_PROV_ITEM_COUNT];
  static uint8_t vPayload[PANEL_PROV_RECORD_CAP];
  uint8_t target;
  size_t n;
  if (store == NULL || flash == NULL || flash->readSlot == NULL ||
      flash->writeSlot == NULL) return PANEL_PROVST_FLASH_IO;
  generation = store->valid ? store->generation + 1u : 1u;
  n = encodeRecord(generation, itemLen, itemData, raw);
  if (n == 0u) return PANEL_PROVST_STORAGE_FULL;
  target = store->valid ? (uint8_t)(1u - store->activeSlot) : 0u;
  if (!flash->writeSlot(flash->user, target, raw, n))
    return PANEL_PROVST_FLASH_IO;
  /* Read-back verify BEFORE adopting: a torn or misprogrammed write must
   * leave the store pointing at the previous record. Byte-exact compare of
   * the programmed record, not just "some CRC-valid record decoded". */
  if (!flash->readSlot(flash->user, target, verify, sizeof(verify)) ||
      memcmp(verify, raw, n) != 0 ||
      !decodeRecord(verify, &generation, vLen, vOff, vPayload) ||
      generation != (store->valid ? store->generation + 1u : 1u))
    return PANEL_PROVST_FLASH_IO;
  store->valid = true;
  store->activeSlot = target;
  store->generation = generation;
  memcpy(store->len, vLen, sizeof(vLen));
  memcpy(store->off, vOff, sizeof(vOff));
  memcpy(store->payload, vPayload, sizeof(vPayload));
  return PANEL_PROVST_OK;
}

bool panelProvStoreWipe(PanelProvStore *store, const PanelProvFlash *flash) {
  /* A blank (zero-magic) record; decodeRecord refuses it, so both slots lose
   * the boot election. Header-sized so the device seam still erases the
   * whole sector before programming. */
  uint8_t blank[PANEL_PROV_RECORD_HDR + PANEL_PROV_RECORD_CRC];
  bool ok;
  if (store == NULL || flash == NULL || flash->writeSlot == NULL) return false;
  memset(blank, 0, sizeof(blank));
  ok = flash->writeSlot(flash->user, 0u, blank, sizeof(blank));
  ok = flash->writeSlot(flash->user, 1u, blank, sizeof(blank)) && ok;
  memset(store, 0, sizeof(*store));
  return ok;
}

const uint8_t *panelProvStoreItem(const PanelProvStore *store, uint8_t itemId,
                                  uint16_t *lenOut) {
  if (lenOut != NULL) *lenOut = 0u;
  if (store == NULL || !store->valid || itemId < 1u ||
      itemId > PANEL_PROV_ITEM_COUNT || store->len[itemId - 1u] == 0u)
    return NULL;
  if (lenOut != NULL) *lenOut = store->len[itemId - 1u];
  return store->payload + store->off[itemId - 1u];
}

#ifdef SOLARI_PANEL_PICO_FLASH
/* Slot base offsets from the end of flash — see the map in panelProvStore.h.
 * The AW config sector (panelScreenCfg.c) stays the FINAL sector; the two
 * credential slots and the spare sit directly below it. */
#define PANEL_PROV_FLASH_BASE(slot)                               \
  (PICO_FLASH_SIZE_BYTES - PANEL_PROV_RESERVED_SECTORS * FLASH_SECTOR_SIZE + \
   (slot) * FLASH_SECTOR_SIZE)

typedef struct {
  uint32_t offset;
  size_t len;
  uint8_t sector[PANEL_PROV_SECTOR_SIZE];
} PanelProvFlashWrite;

/* pico-sdk flash_safe_execute callback: runs with the other core locked out
 * and XIP suspended; touches only its SRAM-passed parameter block. */
static void __no_inline_not_in_flash_func(panelProvFlashSafeWrite)(void *param) {
  PanelProvFlashWrite *w = (PanelProvFlashWrite *)param;
  flash_range_erase(w->offset, FLASH_SECTOR_SIZE);
  flash_range_program(w->offset, w->sector,
                      (w->len + FLASH_PAGE_SIZE - 1u) & ~(FLASH_PAGE_SIZE - 1u));
}

static bool panelProvFlashRead(void *user, uint8_t slot, uint8_t *out,
                               size_t len) {
  const uint8_t *flash;
  (void)user;
  if (out == NULL || slot > 1u || len > PANEL_PROV_SECTOR_SIZE) return false;
  flash = (const uint8_t *)(XIP_BASE + PANEL_PROV_FLASH_BASE(slot));
  memcpy(out, flash, len);
  return true;
}

static bool panelProvFlashWriteSlot(void *user, uint8_t slot,
                                    const uint8_t *data, size_t len) {
  static PanelProvFlashWrite w; /* 4 KiB — deliberately not on the stack */
  (void)user;
  if (data == NULL || slot > 1u || len > PANEL_PROV_SECTOR_SIZE) return false;
  w.offset = PANEL_PROV_FLASH_BASE(slot);
  w.len = len;
  memset(w.sector, 0xff, sizeof(w.sector));
  memcpy(w.sector, data, len);
  return flash_safe_execute(panelProvFlashSafeWrite, &w, 3000u) == PICO_OK;
}
#endif

PanelProvFlash panelProvDeviceFlash(void) {
  PanelProvFlash flash;
  flash.readSlot = NULL;
  flash.writeSlot = NULL;
  flash.user = NULL;
#ifdef SOLARI_PANEL_PICO_FLASH
  flash.readSlot = panelProvFlashRead;
  flash.writeSlot = panelProvFlashWriteSlot;
#endif
  return flash;
}
