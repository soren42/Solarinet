/*
 * panelProvTest.c — codec tests for the SW5 provisioning frame pair
 * (CONTRACT-SW §4 as amended by §13 D1/D2/D4).
 *
 * Scope is the shared codec ONLY: TLV structural encode/decode round-trips,
 * bounds, and parser dispatch of the two new frame types. Item semantics
 * (bounds enforcement, DER validation, generation matching) belong to the
 * firmware provisioning handler and are covered when that ships (P2).
 *
 * Build and run:   make -C firmware/test
 * Exit code 0 = all cases pass. Any failure prints the case and returns 1.
 */

#include <stdio.h>
#include <string.h>

#include "../../protocol.h"

static int gFailures = 0;

/* check — record and report one assertion.
 * Input: condition, printf-style label. Output: none (increments gFailures).  */
static void check(int ok, const char *what) {
  if (!ok) {
    printf("FAIL: %s\n", what);
    gFailures++;
  } else {
    printf("ok:   %s\n", what);
  }
}

/* Case 0 — every normative constant pinned to its CONTRACT-SW literal.
 * These are wire values: a renumbered enum or resized bound is a protocol
 * break even if every round-trip below still passes. */
static void caseContractLiterals(void) {
  check(PANEL_FT_PROVISION == 0x05, "case 0: PANEL_FT_PROVISION is 0x05");
  check(PANEL_FT_PROVACK == 0x86, "case 0: PANEL_FT_PROVACK is 0x86");

  check(PANEL_PROV_SSID == 1 && PANEL_PROV_PSK == 2 &&
        PANEL_PROV_SERVERHOST == 3 && PANEL_PROV_SERVERPORT == 4 &&
        PANEL_PROV_CACERT == 5 && PANEL_PROV_CLIENTCERT == 6 &&
        PANEL_PROV_CLIENTKEY == 7 && PANEL_PROV_COMMIT == 8 &&
        PANEL_PROV_WIPE == 9 && PANEL_PROV_NTPHOST == 10,
        "case 0: item ids 1..10 per §13 D1/D3/D7");

  check(PANEL_PROVST_OK == 0 && PANEL_PROVST_FORMAT_INVALID == 1 &&
        PANEL_PROVST_STORAGE_FULL == 2 && PANEL_PROVST_REJECTED_WIFI == 3 &&
        PANEL_PROVST_COMMIT_INVALID == 4 && PANEL_PROVST_OFFSET_MISMATCH == 5 &&
        PANEL_PROVST_FLASH_IO == 6 && PANEL_PROVST_CRYPTO_INVALID == 7 &&
        PANEL_PROVST_BUSY == 8,
        "case 0: status codes 0..8 per §13 D4");

  check(PANEL_PROV_HDR_SIZE == 6u && PANEL_PROVACK_SIZE == 5u,
        "case 0: header 6 bytes, PROVACK 5 bytes per §13 D1");
  check(PANEL_PROV_MAX_SSID == 32u && PANEL_PROV_MIN_PSK == 8u &&
        PANEL_PROV_MAX_PSK == 63u && PANEL_PROV_MAX_HOST == 253u &&
        PANEL_PROV_MAX_CACERT == 2048u && PANEL_PROV_MAX_CLIENTCERT == 4096u &&
        PANEL_PROV_MAX_CLIENTKEY == 2048u,
        "case 0: D3 item bounds");
  check(PANEL_PROV_CHUNK == 192u && PANEL_MAX_PAYLOAD == 2048u,
        "case 0: 192-byte chunk choice, 2048 normative frame bound (D2)");
}

/* Case 1 — PROVISION round-trip at the wire offsets, with data. */
static void caseProvisionRoundTrip(void) {
  uint8_t payload[PANEL_MAX_PAYLOAD];
  uint8_t data[PANEL_PROV_CHUNK];
  uint8_t itemId, generation;
  uint16_t offset, dataLen;
  const uint8_t *out = NULL;
  size_t n, i;

  for (i = 0u; i < sizeof data; ++i) data[i] = (uint8_t)(i * 7u + 3u);
  n = panelEncodeProvision(PANEL_PROV_CLIENTCERT, 0x42u, 0x0180u, data,
                           (uint16_t)sizeof data, payload, sizeof payload);
  check(n == PANEL_PROV_HDR_SIZE + PANEL_PROV_CHUNK,
        "case 1: encode returns header + chunk bytes");
  check(payload[0] == PANEL_PROV_CLIENTCERT && payload[1] == 0x42u,
        "case 1: itemId/generation at offsets 0/1");
  check(payload[2] == 0x80u && payload[3] == 0x01u,
        "case 1: offset LE at bytes 2-3");
  check(payload[4] == (uint8_t)PANEL_PROV_CHUNK && payload[5] == 0u,
        "case 1: len LE at bytes 4-5");

  check(panelDecodeProvision(payload, n, &itemId, &generation, &offset,
                             &out, &dataLen) == 0,
        "case 1: decode succeeds");
  check(itemId == PANEL_PROV_CLIENTCERT && generation == 0x42u &&
        offset == 0x0180u && dataLen == PANEL_PROV_CHUNK,
        "case 1: decoded fields round-trip");
  check(out == payload + PANEL_PROV_HDR_SIZE &&
        memcmp(out, data, sizeof data) == 0,
        "case 1: data aliases the payload, bytes intact");
}

/* Case 2 — empty-data items (commit/wipe) and encode bounds. */
static void caseProvisionBounds(void) {
  uint8_t payload[PANEL_MAX_PAYLOAD];
  uint8_t big[PANEL_MAX_PAYLOAD];
  uint8_t itemId, generation;
  uint16_t offset, dataLen;
  const uint8_t *out = NULL;
  size_t n;

  n = panelEncodeProvision(PANEL_PROV_COMMIT, 7u, 0u, NULL, 0u,
                           payload, sizeof payload);
  check(n == PANEL_PROV_HDR_SIZE, "case 2: commit encodes header-only");
  check(panelDecodeProvision(payload, n, &itemId, &generation, &offset,
                             &out, &dataLen) == 0 && dataLen == 0u,
        "case 2: header-only TLV decodes with dataLen 0");

  check(panelEncodeProvision(1u, 0u, 0u, big,
                             (uint16_t)(PANEL_MAX_PAYLOAD - PANEL_PROV_HDR_SIZE),
                             payload, sizeof payload) ==
        PANEL_MAX_PAYLOAD,
        "case 2: largest TLV exactly fills PANEL_MAX_PAYLOAD");
  check(panelEncodeProvision(1u, 0u, 0u, big,
                             (uint16_t)(PANEL_MAX_PAYLOAD - PANEL_PROV_HDR_SIZE + 1u),
                             payload, sizeof payload) == 0u,
        "case 2: one byte over PANEL_MAX_PAYLOAD refused");
  check(panelEncodeProvision(1u, 0u, 0u, NULL, 4u, payload, sizeof payload) == 0u,
        "case 2: NULL data with nonzero len refused");
  check(panelEncodeProvision(1u, 0u, 0u, big, 64u, payload, 32u) == 0u,
        "case 2: short output buffer refused");
}

/* Case 3 — PROVISION decode rejects malformed input. */
static void caseProvisionDecodeRejects(void) {
  uint8_t payload[64];
  uint8_t itemId, generation;
  uint16_t offset, dataLen;
  const uint8_t *out = NULL;
  size_t n;

  n = panelEncodeProvision(PANEL_PROV_SSID, 1u, 0u, (const uint8_t *)"lab", 3u,
                           payload, sizeof payload);
  check(n == PANEL_PROV_HDR_SIZE + 3u, "case 3: reference TLV encoded");

  check(panelDecodeProvision(payload, PANEL_PROV_HDR_SIZE - 1u, &itemId,
                             &generation, &offset, &out, &dataLen) == -1,
        "case 3: short header rejected");
  check(panelDecodeProvision(payload, n - 1u, &itemId, &generation, &offset,
                             &out, &dataLen) == -1,
        "case 3: truncated data rejected (declared len > remaining)");
  check(panelDecodeProvision(payload, n + 1u, &itemId, &generation, &offset,
                             &out, &dataLen) == -1,
        "case 3: trailing garbage rejected (declared len < remaining)");
  check(panelDecodeProvision(NULL, n, &itemId, &generation, &offset,
                             &out, &dataLen) == -1,
        "case 3: NULL payload rejected");
  check(panelDecodeProvision(payload, PANEL_MAX_PAYLOAD + 1u, &itemId,
                             &generation, &offset, &out, &dataLen) == -1,
        "case 3: payload over PANEL_MAX_PAYLOAD rejected");
}

/* Case 4 — PROVACK round-trip at the wire offsets, and rejects. */
static void caseProvAck(void) {
  uint8_t payload[16];
  uint8_t itemId, status, generation;
  uint16_t nextOffset;
  size_t n;

  n = panelEncodeProvAck(PANEL_PROV_CACERT, PANEL_PROVST_OFFSET_MISMATCH,
                         0x42u, 0x0240u, payload, sizeof payload);
  check(n == PANEL_PROVACK_SIZE, "case 4: encode returns PANEL_PROVACK_SIZE");
  check(payload[0] == PANEL_PROV_CACERT &&
        payload[1] == PANEL_PROVST_OFFSET_MISMATCH && payload[2] == 0x42u,
        "case 4: itemId/status/generation at offsets 0/1/2");
  check(payload[3] == 0x40u && payload[4] == 0x02u,
        "case 4: nextOffset LE at bytes 3-4");

  check(panelDecodeProvAck(payload, n, &itemId, &status, &generation,
                           &nextOffset) == 0 &&
        itemId == PANEL_PROV_CACERT &&
        status == PANEL_PROVST_OFFSET_MISMATCH &&
        generation == 0x42u && nextOffset == 0x0240u,
        "case 4: decoded fields round-trip");
  check(panelDecodeProvAck(payload, PANEL_PROVACK_SIZE - 1u, &itemId, &status,
                           &generation, &nextOffset) == -1,
        "case 4: short payload rejected");
  payload[PANEL_PROVACK_SIZE] = 0xEEu;
  check(panelDecodeProvAck(payload, PANEL_PROVACK_SIZE + 1u, &itemId, &status,
                           &generation, &nextOffset) == 0 &&
        nextOffset == 0x0240u,
        "case 4: trailing byte tolerated (additive-extension convention)");
  check(panelEncodeProvAck(1u, 0u, 0u, 0u, payload, PANEL_PROVACK_SIZE - 1u) == 0u,
        "case 4: short output buffer refused");
}

/* Case 5 — the parser dispatches both new frame types end to end. */
static uint8_t gCbType;
static size_t gCbLen, gCbCount;
static uint8_t gCbPayload[64];

static void frameCb(uint8_t type, const uint8_t *payload, size_t len,
                    void *user) {
  (void)user;
  gCbType = type;
  gCbLen = len;
  gCbCount++;
  if (len <= sizeof gCbPayload) memcpy(gCbPayload, payload, len);
}

static void caseParserDispatch(void) {
  PanelParser parser;
  uint8_t payload[64], frame[128];
  uint8_t itemId, status, generation;
  uint16_t nextOffset;
  size_t n, total;

  panelParserInit(&parser);

  n = panelEncodeProvision(PANEL_PROV_PSK, 3u, 0u,
                           (const uint8_t *)"hunter22", 8u,
                           payload, sizeof payload);
  total = panelEncodeFrame(PANEL_FT_PROVISION, payload, n, frame, sizeof frame);
  check(total == PANEL_HDR_SIZE + n + PANEL_CRC_SIZE,
        "case 5: PROVISION frame encodes");
  gCbCount = 0u;
  panelParserFeed(&parser, frame, total, 0u, frameCb, NULL);
  check(gCbCount == 1u && gCbType == PANEL_FT_PROVISION && gCbLen == n,
        "case 5: parser dispatches PROVISION (0x05 is a known type)");

  n = panelEncodeProvAck(PANEL_PROV_PSK, PANEL_PROVST_OK, 3u, 8u,
                         payload, sizeof payload);
  total = panelEncodeFrame(PANEL_FT_PROVACK, payload, n, frame, sizeof frame);
  gCbCount = 0u;
  panelParserFeed(&parser, frame, total, 0u, frameCb, NULL);
  check(gCbCount == 1u && gCbType == PANEL_FT_PROVACK && gCbLen == n,
        "case 5: parser dispatches PROVACK (0x86 is a known type)");
  check(panelDecodeProvAck(gCbPayload, gCbLen, &itemId, &status, &generation,
                           &nextOffset) == 0 &&
        itemId == PANEL_PROV_PSK && status == PANEL_PROVST_OK &&
        nextOffset == 8u,
        "case 5: dispatched PROVACK decodes");

  /* A still-unknown type must stay skipped — the additive rule that makes
   * SW5 safe under version 0x01 has to keep holding for the NEXT type. */
  total = panelEncodeFrame(0x06u, NULL, 0u, frame, sizeof frame);
  gCbCount = 0u;
  panelParserFeed(&parser, frame, total, 0u, frameCb, NULL);
  check(gCbCount == 0u, "case 5: unknown type 0x06 still skipped, no desync");
  total = panelEncodeFrame(PANEL_FT_PING, NULL, 0u, frame, sizeof frame);
  panelParserFeed(&parser, frame, total, 0u, frameCb, NULL);
  check(gCbCount == 1u && gCbType == PANEL_FT_PING,
        "case 5: parser healthy after skipping unknown type");
}

int main(void) {
  caseContractLiterals();
  caseProvisionRoundTrip();
  caseProvisionBounds();
  caseProvisionDecodeRejects();
  caseProvAck();
  caseParserDispatch();

  if (gFailures != 0) {
    printf("panelProvTest: %d FAILURE(S)\n", gFailures);
    return 1;
  }
  printf("panelProvTest: all cases passed\n");
  return 0;
}
