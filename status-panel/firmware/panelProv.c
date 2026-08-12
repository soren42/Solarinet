/* panelProv.c — PROVISION TLV handler (CONTRACT-SW §13 D1-D4).
 * Session rules documented in panelProv.h; wire format in protocol.h. */

#include "panelProv.h"

#include <string.h>

#include "../protocol.h"

/* Per-item staging bounds and fixed base offsets into PanelProv.stage.
 * Indexed by itemId-1; commit/wipe (8,9) carry no data. */
static const uint16_t kItemMax[PANEL_PROV_ITEM_COUNT] = {
  PANEL_PROV_MAX_SSID,       /* 1 ssid       */
  PANEL_PROV_MAX_PSK,        /* 2 psk        */
  PANEL_PROV_MAX_HOST,       /* 3 serverHost */
  2u,                        /* 4 serverPort */
  PANEL_PROV_MAX_CACERT,     /* 5 caCertDER  */
  PANEL_PROV_MAX_CLIENTCERT, /* 6 clientCert */
  PANEL_PROV_MAX_CLIENTKEY,  /* 7 clientKey  */
  0u,                        /* 8 commit     */
  0u,                        /* 9 wipe       */
  PANEL_PROV_MAX_HOST        /* 10 ntpHost   */
};

static uint16_t itemBase(uint8_t idx) {
  uint16_t base = 0u;
  uint8_t i;
  for (i = 0u; i < idx; ++i) base = (uint16_t)(base + kItemMax[i]);
  return base;
}

void panelProvInit(PanelProv *prov) {
  if (prov != NULL) memset(prov, 0, sizeof(*prov));
}

/* Purpose: reset staging and open a session at the given wire generation. */
static void startSession(PanelProv *prov, uint8_t generation) {
  memset(prov->fill, 0, sizeof(prov->fill));
  memset(prov->touched, 0, sizeof(prov->touched));
  prov->generation = generation;
  prov->active = true;
}

/* Purpose: run the D4 commit validation over the staged set.
 * Input: staging + crypto seam. Output: a PanelProvStatus — _OK, _COMMIT_
 * INVALID (required item missing or semantically bad) or _CRYPTO_INVALID. */
static int validateStaged(const PanelProv *prov,
                          const PanelProvCrypto *crypto) {
  /* Required set: ssid, psk, serverHost, serverPort, caCert, clientCert,
   * clientKey. ntpHost (10) is the one optional item. */
  static const uint8_t required[7] = { 1u, 2u, 3u, 4u, 5u, 6u, 7u };
  uint8_t i;
  for (i = 0u; i < (uint8_t)sizeof(required); ++i)
    if (prov->fill[required[i] - 1u] == 0u) return PANEL_PROVST_COMMIT_INVALID;
  if (prov->fill[1] < PANEL_PROV_MIN_PSK) return PANEL_PROVST_COMMIT_INVALID;
  if (prov->fill[3] != 2u) return PANEL_PROVST_COMMIT_INVALID;
  /* serverPort is little-endian on the wire like every other u16. */
  if (((uint16_t)prov->stage[itemBase(3u)] |
       ((uint16_t)prov->stage[itemBase(3u) + 1u] << 8)) == 0u)
    return PANEL_PROVST_COMMIT_INVALID;
  if (crypto != NULL) {
    const uint8_t *ca = prov->stage + itemBase(4u);
    const uint8_t *cert = prov->stage + itemBase(5u);
    const uint8_t *key = prov->stage + itemBase(6u);
    if (crypto->validCert != NULL &&
        (!crypto->validCert(crypto->user, ca, prov->fill[4], 1) ||
         !crypto->validCert(crypto->user, cert, prov->fill[5], 0)))
      return PANEL_PROVST_CRYPTO_INVALID;
    if (crypto->validKey != NULL &&
        !crypto->validKey(crypto->user, key, prov->fill[6]))
      return PANEL_PROVST_CRYPTO_INVALID;
    if (crypto->keyMatchesCert != NULL &&
        !crypto->keyMatchesCert(crypto->user, key, prov->fill[6], cert,
                                prov->fill[5]))
      return PANEL_PROVST_CRYPTO_INVALID;
  }
  return PANEL_PROVST_OK;
}

size_t panelProvHandle(PanelProv *prov, const uint8_t *payload, size_t len,
                       bool onWifi, PanelProvStore *store,
                       const PanelProvFlash *flash,
                       const PanelProvCrypto *crypto, uint8_t *ackPayload,
                       size_t ackCap) {
  uint8_t itemId = 0u, generation = 0u, status;
  uint16_t offset = 0u, dataLen = 0u, next = 0u;
  const uint8_t *data = NULL;
  int decoded;

  if (prov == NULL || ackPayload == NULL || ackCap < PANEL_PROVACK_SIZE)
    return 0u;

  decoded = panelDecodeProvision(payload, len, &itemId, &generation, &offset,
                                 &data, &dataLen);
  if (decoded != 0) {
    /* Best-effort echo so the tool can correlate the failure: the frame CRC
     * already passed, so payload[0]/[1] are what the sender put there even
     * when the TLV structure is wrong. */
    itemId = (payload != NULL && len >= 1u) ? payload[0] : 0u;
    generation = (payload != NULL && len >= 2u) ? payload[1] : 0u;
    status = PANEL_PROVST_FORMAT_INVALID;
  } else if (onWifi) {
    status = PANEL_PROVST_REJECTED_WIFI; /* D2: USB transport only */
  } else if (itemId == PANEL_PROV_WIPE) {
    if (dataLen != 0u || offset != 0u) {
      status = PANEL_PROVST_FORMAT_INVALID;
    } else {
      /* D4: wipe clears BOTH the staged set and the flash records. A failed
       * flash write must not be reported as a successful wipe. */
      panelProvInit(prov);
      status = panelProvStoreWipe(store, flash) ? PANEL_PROVST_OK
                                                : PANEL_PROVST_FLASH_IO;
    }
  } else if (itemId == PANEL_PROV_COMMIT) {
    if (dataLen != 0u || offset != 0u) {
      status = PANEL_PROVST_FORMAT_INVALID;
    } else if (!prov->active || generation != prov->generation) {
      /* A commit may only land the session it names — a stale or unopened
       * generation must never flash whatever happens to be staged. */
      status = PANEL_PROVST_COMMIT_INVALID;
    } else {
      status = (uint8_t)validateStaged(prov, crypto);
      if (status == PANEL_PROVST_OK) {
        uint16_t itemLen[PANEL_PROV_ITEM_COUNT];
        const uint8_t *itemData[PANEL_PROV_ITEM_COUNT];
        uint8_t i;
        for (i = 0u; i < PANEL_PROV_ITEM_COUNT; ++i) {
          itemLen[i] = prov->fill[i];
          itemData[i] = prov->stage + itemBase(i);
        }
        status = (uint8_t)panelProvStoreCommit(store, flash, itemLen, itemData);
        /* Success consumes the session; a failed commit keeps staging so the
         * tool can retry commit (flash-io) without resending every item. */
        if (status == PANEL_PROVST_OK) panelProvInit(prov);
      }
    }
  } else if (itemId < 1u || itemId > PANEL_PROV_ITEM_COUNT ||
             kItemMax[itemId - 1u] == 0u) {
    status = PANEL_PROVST_FORMAT_INVALID;
  } else {
    uint8_t idx = (uint8_t)(itemId - 1u);
    if (!prov->active || generation != prov->generation)
      startSession(prov, generation); /* D1: new generation, fresh set */
    if (offset == 0u) {
      /* (Re)start the item — D3: offset 0 in the same generation replaces. */
      prov->fill[idx] = 0u;
      prov->touched[idx] = true;
    }
    if (offset > prov->fill[idx]) {
      status = PANEL_PROVST_OFFSET_MISMATCH;
      next = prov->fill[idx];
    } else if (offset < prov->fill[idx]) {
      /* Retransmit of accepted bytes (a lost PROVACK): re-ack the watermark
       * without writing, so duplicated chunks are idempotent. */
      status = PANEL_PROVST_OK;
      next = prov->fill[idx];
    } else if ((uint32_t)offset + dataLen > kItemMax[idx]) {
      status = PANEL_PROVST_FORMAT_INVALID;
      next = prov->fill[idx];
    } else {
      if (dataLen != 0u)
        memcpy(prov->stage + itemBase(idx) + offset, data, dataLen);
      prov->fill[idx] = (uint16_t)(offset + dataLen);
      prov->touched[idx] = true;
      status = PANEL_PROVST_OK;
      next = prov->fill[idx];
    }
  }

  return panelEncodeProvAck(itemId, status, generation, next, ackPayload,
                            ackCap);
}

const uint8_t *panelProvStaged(const PanelProv *prov, uint8_t itemId,
                               uint16_t *lenOut) {
  if (lenOut != NULL) *lenOut = 0u;
  if (prov == NULL || !prov->active || itemId < 1u ||
      itemId > PANEL_PROV_ITEM_COUNT || prov->fill[itemId - 1u] == 0u)
    return NULL;
  if (lenOut != NULL) *lenOut = prov->fill[itemId - 1u];
  return prov->stage + itemBase((uint8_t)(itemId - 1u));
}

