/* panelProvStore.h — A/B credential record store (CONTRACT-SW §13 D5).
 *
 * Flash map, last 16 KiB of device flash (each region one 4 KiB sector):
 *   [-16 KiB] slot A   credential record
 *   [-12 KiB] slot B   credential record
 *   [ -8 KiB] spare    reserved, never written by this module
 *   [ -4 KiB] AW screen-config sector (panelScreenCfg.c, unchanged)
 *
 * Record wire layout (byte-exact, no native struct padding), one per slot:
 *   [0..3]   magic 0x564F5250 ("PROV" LE)
 *   [4]      version (1)
 *   [5]      reserved (0)
 *   [6..9]   generation u32 LE — monotonic commit counter, NOT the u8 wire
 *            generation from PROVISION TLVs (that one scopes a staging session)
 *   [10..29] u16 LE length per itemId 1..10 (commit/wipe ids 8,9 always 0)
 *   [30..]   item payloads concatenated in ascending itemId order
 *   [last 4] CRC32 (IEEE, reflected) LE over every preceding byte
 *
 * Commit discipline (power-loss safe): serialize -> write the INACTIVE slot ->
 * read back and re-decode -> only then adopt it as active. A cut at any point
 * leaves the previous record intact in the other slot; boot picks the
 * CRC-valid record with the highest generation.
 *
 * Like panelScreenCfg, all flash I/O goes through a callback seam so the host
 * suite exercises every path (corrupt A / corrupt B / both / torn write)
 * without hardware. panelProvDeviceFlash() returns the RP2350 implementation
 * when built with SOLARI_PANEL_PICO_FLASH and an inert seam otherwise. */

#ifndef PANEL_PROV_STORE_H
#define PANEL_PROV_STORE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define PANEL_PROV_SECTOR_SIZE 4096u
#define PANEL_PROV_ITEM_COUNT 10u
#define PANEL_PROV_RECORD_MAGIC 0x564F5250u
#define PANEL_PROV_RECORD_VERSION 1u
#define PANEL_PROV_RECORD_HDR 30u
#define PANEL_PROV_RECORD_CRC 4u
/* Serialized payload capacity of one slot. A max-size staged set (RSA-sized
 * DER bounds from protocol.h sum to 8795) does NOT fit — commit answers
 * PANEL_PROVST_STORAGE_FULL. Real EC-based chlorine chains are ~2-3 KiB. */
#define PANEL_PROV_RECORD_CAP \
  (PANEL_PROV_SECTOR_SIZE - PANEL_PROV_RECORD_HDR - PANEL_PROV_RECORD_CRC)

/* Flash seam. slot is 0 (A) or 1 (B); writeSlot erases the slot's sector and
 * programs len bytes (len <= PANEL_PROV_SECTOR_SIZE); readSlot fills out with
 * the first len bytes of the slot. Both return false on I/O failure. */
typedef struct PanelProvFlash {
  bool (*readSlot)(void *user, uint8_t slot, uint8_t *out, size_t len);
  bool (*writeSlot)(void *user, uint8_t slot, const uint8_t *data, size_t len);
  void *user;
} PanelProvFlash;

typedef struct PanelProvStore {
  bool valid;          /* a CRC-valid record is loaded below            */
  uint8_t activeSlot;  /* slot the loaded record came from (0/1)       */
  uint32_t generation; /* store generation of the loaded record        */
  uint16_t len[PANEL_PROV_ITEM_COUNT];      /* itemId-1 indexed        */
  uint16_t off[PANEL_PROV_ITEM_COUNT];      /* offsets into payload[]  */
  uint8_t payload[PANEL_PROV_RECORD_CAP];
} PanelProvStore;

/* Load both slots, keep the CRC-valid record with the highest generation.
 * Returns store->valid. Never fails hard: unreadable/corrupt slots simply
 * lose the election, and zero valid slots yields an unprovisioned store. */
bool panelProvStoreLoad(PanelProvStore *store, const PanelProvFlash *flash);

/* Serialize (itemLen[10]/itemData indexed by itemId-1) into the inactive
 * slot, read-back verify, then adopt. Returns a PanelProvStatus value from
 * protocol.h: PANEL_PROVST_OK, _STORAGE_FULL (serialized record exceeds one
 * sector) or _FLASH_IO (write or read-back verify failed). */
int panelProvStoreCommit(PanelProvStore *store, const PanelProvFlash *flash,
                         const uint16_t itemLen[PANEL_PROV_ITEM_COUNT],
                         const uint8_t *const itemData[PANEL_PROV_ITEM_COUNT]);

/* D4 wipe: invalidate BOTH slots (magic-less blank record) and reset the
 * in-RAM store to unprovisioned. Returns false if either write failed —
 * callers must treat a failed wipe as credentials still present. */
bool panelProvStoreWipe(PanelProvStore *store, const PanelProvFlash *flash);

/* Loaded item accessor: NULL when the store is invalid or the item is empty,
 * otherwise a pointer into store->payload with *lenOut set. itemId is 1-based
 * (PanelProvItem). */
const uint8_t *panelProvStoreItem(const PanelProvStore *store, uint8_t itemId,
                                  uint16_t *lenOut);

/* CRC32 (IEEE 802.3, reflected, init/xorout 0xFFFFFFFF) — exposed so tests
 * can forge and corrupt records byte-exactly. */
uint32_t panelProvCrc32(const uint8_t *data, size_t len);

/* Device seam: RP2350 flash-backed slots under SOLARI_PANEL_PICO_FLASH,
 * inert (NULL callbacks) on hosts. */
PanelProvFlash panelProvDeviceFlash(void);

#endif /* PANEL_PROV_STORE_H */
