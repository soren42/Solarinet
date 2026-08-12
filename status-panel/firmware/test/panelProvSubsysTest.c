/* panelProvSubsysTest.c — panelProvStore.c + panelProv.c host suite.
 *
 * Store (CONTRACT-SW §13 D5): A/B election by generation, corrupt-A /
 * corrupt-B / corrupt-both boot, torn-write power-loss (commit fails, old
 * record survives), wipe clears both slots.
 *
 * Handler (D1-D4): chunked staging with watermarks, lost-ack retransmit
 * idempotency, offset gaps, generation switch mid-session, WiFi rejection,
 * commit validation (missing items, short PSK, port 0, crypto stubs),
 * storage-full, flash-io, wipe.
 *
 * The fake flash is two RAM sectors with fault injection: failWrite,
 * failRead, and tearAfter (a write "loses power" after N bytes). */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../protocol.h"
#include "../panelProv.h"
#include "../panelProvStore.h"

static int gFails = 0;

#define CHECK(cond, label)                                       \
  do {                                                           \
    if (cond) {                                                  \
      printf("ok:   %s\n", label);                               \
    } else {                                                     \
      printf("FAIL: %s (%s:%d)\n", label, __FILE__, __LINE__);   \
      ++gFails;                                                  \
    }                                                            \
  } while (0)

/* ---- fake flash ---------------------------------------------------------- */

typedef struct {
  uint8_t sector[2][PANEL_PROV_SECTOR_SIZE];
  int failWrite;   /* nonzero: writeSlot returns false, touches nothing   */
  int failRead;    /* nonzero: readSlot returns false                     */
  size_t tearAfter;/* nonzero: write persists only this many bytes, then
                      reports failure — a power cut mid-program           */
  int writes;      /* writeSlot call count (either slot)                  */
} FakeFlash;

static bool fakeRead(void *user, uint8_t slot, uint8_t *out, size_t len) {
  FakeFlash *f = (FakeFlash *)user;
  if (slot > 1u || len > PANEL_PROV_SECTOR_SIZE) return false;
  if (f->failRead) return false;
  memcpy(out, f->sector[slot], len);
  return true;
}

static bool fakeWrite(void *user, uint8_t slot, const uint8_t *data,
                      size_t len) {
  FakeFlash *f = (FakeFlash *)user;
  if (slot > 1u || len > PANEL_PROV_SECTOR_SIZE) return false;
  ++f->writes;
  if (f->failWrite) return false;
  memset(f->sector[slot], 0xff, PANEL_PROV_SECTOR_SIZE); /* sector erase */
  if (f->tearAfter != 0u && f->tearAfter < len) {
    memcpy(f->sector[slot], data, f->tearAfter);
    return false;
  }
  memcpy(f->sector[slot], data, len);
  return true;
}

static PanelProvFlash fakeSeam(FakeFlash *f) {
  PanelProvFlash seam;
  seam.readSlot = fakeRead;
  seam.writeSlot = fakeWrite;
  seam.user = f;
  return seam;
}

/* ---- helpers ------------------------------------------------------------- */

/* Drive one PROVISION through the handler and decode its PROVACK. */
typedef struct {
  uint8_t itemId, status, generation;
  uint16_t nextOffset;
} Ack;

static Ack drive(PanelProv *prov, PanelProvStore *store,
                 const PanelProvFlash *flash, const PanelProvCrypto *crypto,
                 bool onWifi, uint8_t itemId, uint8_t generation,
                 uint16_t offset, const uint8_t *data, uint16_t dataLen) {
  uint8_t frame[PANEL_MAX_PAYLOAD], ackBuf[PANEL_PROVACK_SIZE];
  Ack ack = { 0u, 0xffu, 0u, 0u };
  size_t n = panelEncodeProvision(itemId, generation, offset, data, dataLen,
                                  frame, sizeof(frame));
  size_t an = panelProvHandle(prov, frame, n, onWifi, store, flash, crypto,
                              ackBuf, sizeof(ackBuf));
  if (an == PANEL_PROVACK_SIZE)
    (void)panelDecodeProvAck(ackBuf, an, &ack.itemId, &ack.status,
                             &ack.generation, &ack.nextOffset);
  return ack;
}

/* Send a whole item in PANEL_PROV_CHUNK slices, asserting OK acks. */
static int sendItem(PanelProv *prov, PanelProvStore *store,
                    const PanelProvFlash *flash, uint8_t itemId,
                    uint8_t generation, const uint8_t *data, uint16_t len) {
  uint16_t off = 0u;
  do {
    uint16_t chunk = (uint16_t)(len - off);
    Ack ack;
    if (chunk > PANEL_PROV_CHUNK) chunk = PANEL_PROV_CHUNK;
    ack = drive(prov, store, flash, NULL, false, itemId, generation, off,
                data + off, chunk);
    if (ack.status != PANEL_PROVST_OK || ack.nextOffset != off + chunk)
      return 0;
    off = (uint16_t)(off + chunk);
  } while (off < len);
  return 1;
}

/* Stage the full required set at the given generation (tiny fake DER blobs —
 * the crypto seam is NULL here, so structure is not inspected). */
static void stageFullSet(PanelProv *prov, PanelProvStore *store,
                         const PanelProvFlash *flash, uint8_t generation,
                         uint16_t certLen) {
  static uint8_t blob[PANEL_PROV_MAX_CLIENTCERT];
  static const uint8_t port[2] = { 0x1b, 0x1a }; /* 6683 LE */
  size_t i;
  for (i = 0u; i < sizeof(blob); ++i) blob[i] = (uint8_t)(i * 7u + 3u);
  sendItem(prov, store, flash, PANEL_PROV_SSID, generation,
           (const uint8_t *)"SolariNet-Panel", 15u);
  sendItem(prov, store, flash, PANEL_PROV_PSK, generation,
           (const uint8_t *)"testpass-not-a-secret", 21u);
  sendItem(prov, store, flash, PANEL_PROV_SERVERHOST, generation,
           (const uint8_t *)"xenon.akoria.net", 16u);
  sendItem(prov, store, flash, PANEL_PROV_SERVERPORT, generation, port, 2u);
  sendItem(prov, store, flash, PANEL_PROV_CACERT, generation, blob,
           certLen > PANEL_PROV_MAX_CACERT ? PANEL_PROV_MAX_CACERT : certLen);
  sendItem(prov, store, flash, PANEL_PROV_CLIENTCERT, generation, blob,
           certLen);
  sendItem(prov, store, flash, PANEL_PROV_CLIENTKEY, generation, blob,
           certLen > PANEL_PROV_MAX_CLIENTKEY ? PANEL_PROV_MAX_CLIENTKEY
                                              : certLen);
}

/* ---- store cases --------------------------------------------------------- */

static void caseStoreBoot(void) {
  FakeFlash f;
  PanelProvFlash seam = fakeSeam(&f);
  PanelProvStore store;
  uint16_t itemLen[PANEL_PROV_ITEM_COUNT] = { 4u, 8u, 0u, 2u, 0u, 0u, 0u, 0u, 0u, 0u };
  const uint8_t ssid[4] = { 'n', 'e', 't', '1' };
  const uint8_t psk[8] = { 'p', 'a', 's', 's', 'w', 'o', 'r', 'd' };
  const uint8_t port[2] = { 0x01, 0x00 };
  const uint8_t *itemData[PANEL_PROV_ITEM_COUNT] = { ssid, psk, NULL, port,
                                                     NULL, NULL, NULL, NULL,
                                                     NULL, NULL };
  uint16_t len = 0u;
  const uint8_t *p;

  /* Erased flash: no record, store unprovisioned. */
  memset(&f, 0, sizeof(f));
  memset(f.sector, 0xff, sizeof(f.sector));
  CHECK(!panelProvStoreLoad(&store, &seam), "S1: erased flash loads unprovisioned");

  /* First commit lands slot A, generation 1; items read back byte-exact. */
  CHECK(panelProvStoreCommit(&store, &seam, itemLen, itemData) ==
            PANEL_PROVST_OK,
        "S1: first commit ok");
  CHECK(store.valid && store.activeSlot == 0u && store.generation == 1u,
        "S1: first commit -> slot A gen 1");
  p = panelProvStoreItem(&store, PANEL_PROV_PSK, &len);
  CHECK(p != NULL && len == 8u && memcmp(p, psk, 8u) == 0,
        "S1: committed item reads back byte-exact");
  CHECK(panelProvStoreItem(&store, PANEL_PROV_SERVERHOST, &len) == NULL,
        "S1: empty item reads back NULL");

  /* Second commit alternates to slot B and bumps the generation. */
  CHECK(panelProvStoreCommit(&store, &seam, itemLen, itemData) ==
            PANEL_PROVST_OK &&
        store.activeSlot == 1u && store.generation == 2u,
        "S2: second commit alternates to slot B gen 2");

  /* Fresh boot elects the highest CRC-valid generation (B). */
  CHECK(panelProvStoreLoad(&store, &seam) && store.activeSlot == 1u &&
            store.generation == 2u,
        "S2: boot elects highest generation");

  /* Corrupt B (the newer record): boot falls back to A. */
  f.sector[1][40] ^= 0x5au;
  CHECK(panelProvStoreLoad(&store, &seam) && store.activeSlot == 0u &&
            store.generation == 1u,
        "S3: corrupt newest slot falls back to the other");

  /* Corrupt both: unprovisioned, never a hard failure. */
  f.sector[0][40] ^= 0x5au;
  CHECK(!panelProvStoreLoad(&store, &seam), "S3: corrupt both -> unprovisioned");
}

static void caseStoreTornWrite(void) {
  FakeFlash f;
  PanelProvFlash seam = fakeSeam(&f);
  PanelProvStore store;
  uint16_t itemLen[PANEL_PROV_ITEM_COUNT] = { 5u, 8u, 0u, 2u, 0u, 0u, 0u, 0u, 0u, 0u };
  const uint8_t ssid[5] = { 'n', 'e', 't', '-', 'a' };
  const uint8_t psk[8] = { 's', 'e', 't', 'u', 'p', 'k', 'e', 'y' };
  const uint8_t port[2] = { 0x02, 0x00 };
  const uint8_t *itemData[PANEL_PROV_ITEM_COUNT] = { ssid, psk, NULL, port,
                                                     NULL, NULL, NULL, NULL,
                                                     NULL, NULL };

  memset(&f, 0, sizeof(f));
  memset(f.sector, 0xff, sizeof(f.sector));
  panelProvStoreLoad(&store, &seam);
  CHECK(panelProvStoreCommit(&store, &seam, itemLen, itemData) ==
            PANEL_PROVST_OK,
        "S4: baseline commit ok");

  /* Power cut mid-program of the NEXT commit: the write tears, commit
   * reports flash-io, and the in-RAM store still points at the old record. */
  f.tearAfter = 20u;
  CHECK(panelProvStoreCommit(&store, &seam, itemLen, itemData) ==
            PANEL_PROVST_FLASH_IO,
        "S4: torn write -> flash-io");
  CHECK(store.valid && store.activeSlot == 0u && store.generation == 1u,
        "S4: store still on the pre-tear record");

  /* Reboot after the cut: the torn slot loses the election, gen 1 survives. */
  f.tearAfter = 0u;
  CHECK(panelProvStoreLoad(&store, &seam) && store.activeSlot == 0u &&
            store.generation == 1u,
        "S4: reboot after tear elects the surviving record");

  /* Read-back-verify catches a flash that acks writes but corrupts them:
   * inject the corruption via failRead on the verify pass. */
  f.failRead = 1;
  CHECK(panelProvStoreCommit(&store, &seam, itemLen, itemData) ==
            PANEL_PROVST_FLASH_IO,
        "S5: unreadable verify pass -> flash-io, not silent success");
  f.failRead = 0;

  /* Wipe invalidates both slots and the RAM store. */
  CHECK(panelProvStoreWipe(&store, &seam), "S6: wipe reports success");
  CHECK(!store.valid, "S6: wipe resets RAM store");
  CHECK(!panelProvStoreLoad(&store, &seam), "S6: post-wipe boot unprovisioned");

  /* A failing wipe must say so. */
  f.failWrite = 1;
  CHECK(!panelProvStoreWipe(&store, &seam), "S6: failed wipe reports failure");
}

/* ---- handler cases ------------------------------------------------------- */

static void caseHandlerStaging(void) {
  FakeFlash f;
  PanelProvFlash seam = fakeSeam(&f);
  PanelProvStore store;
  PanelProv prov;
  uint8_t data[300];
  uint16_t i, len = 0u;
  const uint8_t *p;
  Ack ack;

  memset(&f, 0, sizeof(f));
  memset(f.sector, 0xff, sizeof(f.sector));
  panelProvStoreLoad(&store, &seam);
  panelProvInit(&prov);
  for (i = 0u; i < sizeof(data); ++i) data[i] = (uint8_t)(i & 0xffu);

  /* Chunked host item (253 bytes in 192+61) with watermark acks. */
  ack = drive(&prov, &store, &seam, NULL, false, PANEL_PROV_SERVERHOST, 7u,
              0u, data, PANEL_PROV_CHUNK);
  CHECK(ack.status == PANEL_PROVST_OK && ack.nextOffset == PANEL_PROV_CHUNK &&
            ack.generation == 7u && ack.itemId == PANEL_PROV_SERVERHOST,
        "H1: first chunk acks watermark 192");
  ack = drive(&prov, &store, &seam, NULL, false, PANEL_PROV_SERVERHOST, 7u,
              PANEL_PROV_CHUNK, data + PANEL_PROV_CHUNK, 61u);
  CHECK(ack.status == PANEL_PROVST_OK && ack.nextOffset == 253u,
        "H1: second chunk completes the item");
  p = panelProvStaged(&prov, PANEL_PROV_SERVERHOST, &len);
  CHECK(p != NULL && len == 253u && memcmp(p, data, 253u) == 0,
        "H1: staged bytes are byte-exact");

  /* Lost-ack retransmit: resend chunk 2; watermark re-acked, nothing moves. */
  ack = drive(&prov, &store, &seam, NULL, false, PANEL_PROV_SERVERHOST, 7u,
              PANEL_PROV_CHUNK, data + PANEL_PROV_CHUNK, 61u);
  CHECK(ack.status == PANEL_PROVST_OK && ack.nextOffset == 253u,
        "H2: retransmit of accepted bytes re-acks idempotently");

  /* Gap: offset past the watermark names the resume point. */
  ack = drive(&prov, &store, &seam, NULL, false, PANEL_PROV_SSID, 7u, 8u,
              data, 8u);
  CHECK(ack.status == PANEL_PROVST_OFFSET_MISMATCH && ack.nextOffset == 0u,
        "H2: offset gap -> mismatch with resume watermark");

  /* Bound overrun: 253-byte host + one more byte. */
  ack = drive(&prov, &store, &seam, NULL, false, PANEL_PROV_SERVERHOST, 7u,
              253u, data, 1u);
  CHECK(ack.status == PANEL_PROVST_FORMAT_INVALID,
        "H2: exceeding the item bound is format-invalid");

  /* Offset-0 replace: same generation, shorter item wins. */
  ack = drive(&prov, &store, &seam, NULL, false, PANEL_PROV_SERVERHOST, 7u,
              0u, data, 10u);
  p = panelProvStaged(&prov, PANEL_PROV_SERVERHOST, &len);
  CHECK(ack.status == PANEL_PROVST_OK && p != NULL && len == 10u,
        "H3: offset 0 replaces the staged item (D3)");

  /* Generation switch discards the whole staged set. */
  ack = drive(&prov, &store, &seam, NULL, false, PANEL_PROV_SSID, 8u, 0u,
              data, 4u);
  CHECK(ack.status == PANEL_PROVST_OK && ack.generation == 8u,
        "H4: new generation opens a fresh session");
  CHECK(panelProvStaged(&prov, PANEL_PROV_SERVERHOST, &len) == NULL,
        "H4: generation switch discarded prior staging (D1)");

  /* Non-zero offset in a NEW generation cannot resume anything. */
  ack = drive(&prov, &store, &seam, NULL, false, PANEL_PROV_SSID, 9u, 4u,
              data, 4u);
  CHECK(ack.status == PANEL_PROVST_OFFSET_MISMATCH && ack.nextOffset == 0u,
        "H4: fresh generation at nonzero offset -> mismatch");

  /* Unknown itemId and malformed TLV each get an explicit error ack. */
  ack = drive(&prov, &store, &seam, NULL, false, 42u, 9u, 0u, data, 4u);
  CHECK(ack.status == PANEL_PROVST_FORMAT_INVALID && ack.itemId == 42u,
        "H5: unknown itemId is format-invalid");
  {
    uint8_t raw[3] = { PANEL_PROV_SSID, 9u, 0u }; /* truncated header */
    uint8_t ackBuf[PANEL_PROVACK_SIZE];
    size_t an = panelProvHandle(&prov, raw, sizeof(raw), false, &store, &seam,
                                NULL, ackBuf, sizeof(ackBuf));
    Ack a2 = { 0u, 0xffu, 0u, 0u };
    if (an == PANEL_PROVACK_SIZE)
      (void)panelDecodeProvAck(ackBuf, an, &a2.itemId, &a2.status,
                               &a2.generation, &a2.nextOffset);
    CHECK(an == PANEL_PROVACK_SIZE &&
              a2.status == PANEL_PROVST_FORMAT_INVALID &&
              a2.itemId == PANEL_PROV_SSID,
          "H5: malformed TLV still answers, echoing payload[0]");
  }

  /* D2: WiFi transport rejects everything, including commit and wipe. */
  ack = drive(&prov, &store, &seam, NULL, true, PANEL_PROV_SSID, 9u, 0u,
              data, 4u);
  CHECK(ack.status == PANEL_PROVST_REJECTED_WIFI,
        "H6: data TLV over WiFi rejected");
  ack = drive(&prov, &store, &seam, NULL, true, PANEL_PROV_COMMIT, 9u, 0u,
              NULL, 0u);
  CHECK(ack.status == PANEL_PROVST_REJECTED_WIFI,
        "H6: commit over WiFi rejected");
  ack = drive(&prov, &store, &seam, NULL, true, PANEL_PROV_WIPE, 9u, 0u,
              NULL, 0u);
  CHECK(ack.status == PANEL_PROVST_REJECTED_WIFI,
        "H6: wipe over WiFi rejected");
  CHECK(f.writes == 0, "H6: nothing above ever touched flash");
}

static void caseHandlerCommit(void) {
  FakeFlash f;
  PanelProvFlash seam = fakeSeam(&f);
  PanelProvStore store;
  PanelProv prov;
  Ack ack;
  uint16_t len = 0u;
  const uint8_t *p;
  static const uint8_t zeroPort[2] = { 0x00, 0x00 };

  memset(&f, 0, sizeof(f));
  memset(f.sector, 0xff, sizeof(f.sector));
  panelProvStoreLoad(&store, &seam);
  panelProvInit(&prov);

  /* Commit with no session open. */
  ack = drive(&prov, &store, &seam, NULL, false, PANEL_PROV_COMMIT, 1u, 0u,
              NULL, 0u);
  CHECK(ack.status == PANEL_PROVST_COMMIT_INVALID,
        "C1: commit without a session is invalid");

  /* Commit with data attached is malformed. */
  {
    static const uint8_t junk[1] = { 0u };
    ack = drive(&prov, &store, &seam, NULL, false, PANEL_PROV_COMMIT, 1u, 0u,
                junk, 1u);
    CHECK(ack.status == PANEL_PROVST_FORMAT_INVALID,
          "C1: commit carrying data is format-invalid");
  }

  /* Incomplete set: ssid alone cannot commit. */
  drive(&prov, &store, &seam, NULL, false, PANEL_PROV_SSID, 2u, 0u,
        (const uint8_t *)"net", 3u);
  ack = drive(&prov, &store, &seam, NULL, false, PANEL_PROV_COMMIT, 2u, 0u,
              NULL, 0u);
  CHECK(ack.status == PANEL_PROVST_COMMIT_INVALID,
        "C2: incomplete staged set fails commit");

  /* A commit naming a different generation refuses; staging is untouched
   * (only a DATA TLV at a new generation discards, per D1). */
  stageFullSet(&prov, &store, &seam, 3u, 600u);
  ack = drive(&prov, &store, &seam, NULL, false, PANEL_PROV_COMMIT, 4u, 0u,
              NULL, 0u);
  CHECK(ack.status == PANEL_PROVST_COMMIT_INVALID,
        "C2: commit naming the wrong generation is invalid");
  CHECK(panelProvStaged(&prov, PANEL_PROV_SSID, &len) != NULL &&
            prov.generation == 3u,
        "C2: a mismatched commit leaves the staged session intact");

  stageFullSet(&prov, &store, &seam, 5u, 600u);
  /* Break one required item: 4-byte PSK (below the WPA2 minimum). */
  drive(&prov, &store, &seam, NULL, false, PANEL_PROV_PSK, 5u, 0u,
        (const uint8_t *)"shrt", 4u);
  ack = drive(&prov, &store, &seam, NULL, false, PANEL_PROV_COMMIT, 5u, 0u,
              NULL, 0u);
  CHECK(ack.status == PANEL_PROVST_COMMIT_INVALID,
        "C3: short PSK fails commit validation");

  /* Port 0. */
  stageFullSet(&prov, &store, &seam, 6u, 600u);
  drive(&prov, &store, &seam, NULL, false, PANEL_PROV_SERVERPORT, 6u, 0u,
        zeroPort, 2u);
  ack = drive(&prov, &store, &seam, NULL, false, PANEL_PROV_COMMIT, 6u, 0u,
              NULL, 0u);
  CHECK(ack.status == PANEL_PROVST_COMMIT_INVALID,
        "C3: port 0 fails commit validation");

  /* Happy path: full set commits, lands in flash, session consumed. */
  stageFullSet(&prov, &store, &seam, 7u, 600u);
  ack = drive(&prov, &store, &seam, NULL, false, PANEL_PROV_COMMIT, 7u, 0u,
              NULL, 0u);
  CHECK(ack.status == PANEL_PROVST_OK, "C4: full set commits ok");
  CHECK(store.valid && store.generation == 1u,
        "C4: commit landed store generation 1");
  p = panelProvStoreItem(&store, PANEL_PROV_SERVERHOST, &len);
  CHECK(p != NULL && len == 16u && memcmp(p, "xenon.akoria.net", 16u) == 0,
        "C4: committed serverHost reads back byte-exact");
  CHECK(!prov.active, "C4: successful commit consumes the session");
  ack = drive(&prov, &store, &seam, NULL, false, PANEL_PROV_COMMIT, 7u, 0u,
              NULL, 0u);
  CHECK(ack.status == PANEL_PROVST_COMMIT_INVALID,
        "C4: a second commit of the consumed session is invalid");

  /* A reloaded store sees the same record (commit really hit the seam). */
  {
    PanelProvStore reload;
    CHECK(panelProvStoreLoad(&reload, &seam) && reload.generation == 1u,
          "C4: fresh load sees the committed record");
  }

  /* Storage-full: max-bound certs (2048+4096+2048) cannot fit one sector.
   * Staging accepts them (RAM bound is the D3 per-item bound); commit is
   * where capacity is enforced, and it must NOT consume the session. */
  stageFullSet(&prov, &store, &seam, 8u, PANEL_PROV_MAX_CLIENTCERT);
  ack = drive(&prov, &store, &seam, NULL, false, PANEL_PROV_COMMIT, 8u, 0u,
              NULL, 0u);
  CHECK(ack.status == PANEL_PROVST_STORAGE_FULL,
        "C5: oversized set answers storage-full at commit");
  CHECK(prov.active, "C5: failed commit keeps the session for a retry");
  CHECK(store.generation == 1u, "C5: active record untouched");

  /* Flash-io on commit keeps staging AND the old record. */
  stageFullSet(&prov, &store, &seam, 9u, 600u);
  f.failWrite = 1;
  ack = drive(&prov, &store, &seam, NULL, false, PANEL_PROV_COMMIT, 9u, 0u,
              NULL, 0u);
  CHECK(ack.status == PANEL_PROVST_FLASH_IO && prov.active &&
            store.generation == 1u,
        "C5: flash-io commit keeps session and old record");
  f.failWrite = 0;
  ack = drive(&prov, &store, &seam, NULL, false, PANEL_PROV_COMMIT, 9u, 0u,
              NULL, 0u);
  CHECK(ack.status == PANEL_PROVST_OK && store.generation == 2u,
        "C5: commit retry after flash-io succeeds without restaging");

  /* Wipe: clears staging and both slots. */
  stageFullSet(&prov, &store, &seam, 10u, 600u);
  ack = drive(&prov, &store, &seam, NULL, false, PANEL_PROV_WIPE, 10u, 0u,
              NULL, 0u);
  CHECK(ack.status == PANEL_PROVST_OK, "C6: wipe acks ok");
  CHECK(!prov.active && !store.valid, "C6: wipe cleared staging and store");
  {
    PanelProvStore reload;
    CHECK(!panelProvStoreLoad(&reload, &seam),
          "C6: post-wipe flash holds no valid record");
  }
}

/* Crypto seam: stubs prove the commit path calls every hook and maps a
 * refusal to _CRYPTO_INVALID without touching flash. */
typedef struct {
  int certCalls, keyCalls, matchCalls;
  int refuseCert, refuseKey, refuseMatch;
} CryptoStub;

static int stubCert(void *user, const uint8_t *der, uint16_t len, int isCa) {
  CryptoStub *s = (CryptoStub *)user;
  (void)der;
  (void)len;
  (void)isCa;
  ++s->certCalls;
  return !s->refuseCert;
}

static int stubKey(void *user, const uint8_t *der, uint16_t len) {
  CryptoStub *s = (CryptoStub *)user;
  (void)der;
  (void)len;
  ++s->keyCalls;
  return !s->refuseKey;
}

static int stubMatch(void *user, const uint8_t *keyDer, uint16_t keyLen,
                     const uint8_t *certDer, uint16_t certLen) {
  CryptoStub *s = (CryptoStub *)user;
  (void)keyDer;
  (void)keyLen;
  (void)certDer;
  (void)certLen;
  ++s->matchCalls;
  return !s->refuseMatch;
}

static void caseHandlerCrypto(void) {
  FakeFlash f;
  PanelProvFlash seam = fakeSeam(&f);
  PanelProvStore store;
  PanelProv prov;
  CryptoStub stub;
  PanelProvCrypto crypto;
  Ack ack;
  int writesBefore;

  memset(&f, 0, sizeof(f));
  memset(f.sector, 0xff, sizeof(f.sector));
  panelProvStoreLoad(&store, &seam);
  panelProvInit(&prov);
  memset(&stub, 0, sizeof(stub));
  crypto.validCert = stubCert;
  crypto.validKey = stubKey;
  crypto.keyMatchesCert = stubMatch;
  crypto.user = &stub;

  stageFullSet(&prov, &store, &seam, 1u, 600u);
  stub.refuseKey = 1;
  writesBefore = f.writes;
  ack = drive(&prov, &store, &seam, &crypto, false, PANEL_PROV_COMMIT, 1u,
              0u, NULL, 0u);
  CHECK(ack.status == PANEL_PROVST_CRYPTO_INVALID,
        "X1: refused key -> crypto-invalid");
  CHECK(f.writes == writesBefore, "X1: refused commit never touched flash");
  CHECK(stub.certCalls == 2, "X1: both certs were validated first");

  stub.refuseKey = 0;
  memset(&stub, 0, sizeof(stub));
  ack = drive(&prov, &store, &seam, &crypto, false, PANEL_PROV_COMMIT, 1u,
              0u, NULL, 0u);
  CHECK(ack.status == PANEL_PROVST_OK && stub.certCalls == 2 &&
            stub.keyCalls == 1 && stub.matchCalls == 1,
        "X2: accepting seam sees ca+cert, key, and match exactly once");
}

int main(void) {
  caseStoreBoot();
  caseStoreTornWrite();
  caseHandlerStaging();
  caseHandlerCommit();
  caseHandlerCrypto();
  if (gFails != 0) {
    printf("panelProvSubsysTest: %d FAILURE(S)\n", gFails);
    return 1;
  }
  printf("panelProvSubsysTest: all passed\n");
  return 0;
}
