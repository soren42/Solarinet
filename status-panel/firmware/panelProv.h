/* panelProv.h — PROVISION TLV handler (CONTRACT-SW §13 D1-D4).
 *
 * The provisioning session is lockstep: the host tool sends one PROVISION
 * frame and waits for its PROVACK before the next. This handler owns the RAM
 * staging area, the per-item offset watermarks and the u8 wire generation;
 * panelProvStore.c owns the flash A/B records that a successful commit lands.
 *
 * Rules implemented here (see CONTRACT-SW §13 for the normative text):
 *  - USB TRANSPORT ONLY: every PROVISION arriving over WiFi is answered
 *    PANEL_PROVST_REJECTED_WIFI and touches nothing (D2).
 *  - A TLV whose generation differs from the staging area's discards the
 *    staged set and starts a fresh session at that generation (D1).
 *  - offset 0 (re)starts an item; offset == watermark appends; offset below
 *    the watermark is a retransmit of already-accepted bytes and is re-acked
 *    idempotently without writing; offset above it is _OFFSET_MISMATCH and
 *    the ack's nextOffset tells the tool where to resume (D3).
 *  - commit (item 8) validates the staged set (required items, PSK length,
 *    port != 0, crypto seam) before touching flash; wipe (item 9) clears
 *    staging AND both flash slots (D4).
 *
 * Every PROVISION frame gets exactly one PROVACK — including malformed ones
 * (frame CRC already passed, so the sender is live and waiting). */

#ifndef PANEL_PROV_H
#define PANEL_PROV_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "panelProvStore.h"

/* Staging capacity = sum of the per-item D3 bounds from protocol.h
 * (32+63+253+2+2048+4096+2048+253). Items stage at fixed offsets so a
 * replace never moves neighbours. */
#define PANEL_PROV_STAGE_CAP 8795u

/* Crypto validation seam (D4 commit step). NULL struct or NULL callbacks
 * accept — the host suite injects stubs, and P3 wires the real mbedTLS
 * checks on-device. Each returns nonzero for valid. */
typedef struct PanelProvCrypto {
  int (*validCert)(void *user, const uint8_t *der, uint16_t len, int isCa);
  int (*validKey)(void *user, const uint8_t *der, uint16_t len);
  int (*keyMatchesCert)(void *user, const uint8_t *keyDer, uint16_t keyLen,
                        const uint8_t *certDer, uint16_t certLen);
  void *user;
} PanelProvCrypto;

typedef struct PanelProv {
  bool active;        /* a staging session is open                       */
  uint8_t generation; /* wire generation of the open session             */
  uint16_t fill[PANEL_PROV_ITEM_COUNT]; /* accepted-bytes watermarks     */
  bool touched[PANEL_PROV_ITEM_COUNT];  /* item has been (re)started     */
  uint8_t stage[PANEL_PROV_STAGE_CAP];
} PanelProv;

void panelProvInit(PanelProv *prov);

/* Handle one PROVISION frame payload and build its PROVACK.
 * Input: the frame payload/len, whether it arrived over WiFi, the flash seam
 * + loaded store (commit/wipe targets), the crypto seam, and an ack buffer
 * of at least PANEL_PROVACK_SIZE bytes.
 * Output: PROVACK payload length (PANEL_PROVACK_SIZE), or 0 only when
 * ackPayload/ackCap cannot hold the ack. Always answers — errors are
 * expressed as PROVACK statuses, never silence. */
size_t panelProvHandle(PanelProv *prov, const uint8_t *payload, size_t len,
                       bool onWifi, PanelProvStore *store,
                       const PanelProvFlash *flash,
                       const PanelProvCrypto *crypto, uint8_t *ackPayload,
                       size_t ackCap);

/* Staged-byte accessor for tests and diagnostics: NULL unless the session is
 * active and the item has accepted bytes; *lenOut = watermark. */
const uint8_t *panelProvStaged(const PanelProv *prov, uint8_t itemId,
                               uint16_t *lenOut);

#endif /* PANEL_PROV_H */
