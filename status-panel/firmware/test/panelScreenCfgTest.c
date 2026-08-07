/* panelScreenCfgTest.c — host checks for CONTRACT-AW rotation/persistence. */
#include <stdio.h>
#include <string.h>

#include "../panelScreenCfg.h"
#include "../../protocol.h"

static int gFailures = 0;

/* Purpose: print one plain host-suite assertion result. Input: result/label. Output: none. */
static void check(int ok, const char *label) {
  if (ok) printf("ok:   %s\n", label);
  else { printf("FAIL: %s\n", label); ++gFailures; }
}

typedef struct { uint8_t bytes[19]; int writes; } FlashMock;

/* Purpose: emulate reading the last flash-sector record. Input: mock/output. Output: read success. */
static bool mockRead(void *user, uint8_t *out, size_t len) {
  FlashMock *mock = (FlashMock *)user;
  if (len > sizeof(mock->bytes)) return false;
  memcpy(out, mock->bytes, len);
  return true;
}

/* Purpose: emulate erase/programming one record and count flash wear. Input: mock/record. Output: write success. */
static bool mockWrite(void *user, const uint8_t *data, size_t len) {
  FlashMock *mock = (FlashMock *)user;
  if (len > sizeof(mock->bytes)) return false;
  memcpy(mock->bytes, data, len);
  ++mock->writes;
  return true;
}

typedef struct { int snapshot, state, config, control; } ParserCounts;

/* Purpose: count parser-path delivery of every AW2 frame class. Input: frame callback fields. Output: counters updated. */
static void receive(uint8_t type, const uint8_t *payload, size_t len, void *user) {
  ParserCounts *counts = (ParserCounts *)user;
  PanelSnapshot snap;
  uint8_t config[PANEL_SCREEN_COUNT], flags, kind, arg;
  uint32_t command;
  if (type == PANEL_FT_SNAPSHOT && panelDecodeSnapshot(payload, len, &snap) == 0) ++counts->snapshot;
  if (type == PANEL_FT_STATE) ++counts->state;
  if (type == PANEL_FT_CONFIG && panelDecodeConfig(payload, len, config, &flags) == 0) ++counts->config;
  if (type == PANEL_FT_CONTROL && panelDecodeControl(payload, len, &command, &kind, &arg) == 0) ++counts->control;
}

/* Purpose: feed one encoded frame bytewise through the real parser. Input: type/payload/counters. Output: none. */
static void feedFrame(uint8_t type, const uint8_t *payload, size_t len, ParserCounts *counts) {
  uint8_t frame[PANEL_HDR_SIZE + PANEL_MAX_PAYLOAD + PANEL_CRC_SIZE];
  PanelParser parser;
  size_t frameLen = panelEncodeFrame(type, payload, len, frame, sizeof(frame));
  size_t i;
  panelParserInit(&parser);
  for (i = 0u; i < frameLen; ++i) panelParserFeed(&parser, frame + i, 1u, (uint32_t)i, receive, counts);
}

/* Purpose: run a one-second synthetic rotation clock to its next advance.
 * Input: config/index/base dwell. Output: elapsed seconds at advance. */
static unsigned int advanceAt(const PanelScreenCfg *cfg, uint8_t index,
                              float baseSec) {
  float elapsed = 0.0f;
  float dwell = panelScreenCfgDwellSec(cfg, index, baseSec);
  while (elapsed < dwell) elapsed += 1.0f;
  return (unsigned int)elapsed;
}

/* Review fix: the A2 rescale cases now exercise the SHIPPED implementation
 * (panelScreenCfgRescaleDwell in panelScreenCfg.c, called by main.c) — the
 * previous test-local reimplementation asserted itself, catching nothing. */
#define rescaleDwell panelScreenCfgRescaleDwell

int main(void) {
  FlashMock mock;
  PanelScreenCfgFlash flash;
  PanelScreenCfg cfg, reboot;
  uint8_t config[PANEL_SCREEN_COUNT], payload[PANEL_MAX_PAYLOAD];
  PanelSnapshot sent, got;
  ParserCounts counts = { 0, 0, 0, 0 };
  size_t len;
  memset(&mock, 0xff, sizeof(mock));
  mock.writes = 0;
  flash.read = mockRead; flash.writeSector = mockWrite; flash.user = &mock;

  panelScreenCfgLoad(&cfg, &flash, 0u);
  check(cfg.screenCfg[0] == 0x05u && cfg.screenCfg[11] == 0x05u,
        "A4: absent flash selects enabled 1x defaults");
  check(!cfg.persisted, "A4: defaults with no flash record are not cfgPersisted");
  check(panelScreenCfgSet(&cfg, 4u, 0x09u, 10u) &&
        panelScreenCfgSet(&cfg, 4u, 0x0bu, 1000u) &&
        !panelScreenCfgSaveDue(&cfg, &flash, 2999u) &&
        panelScreenCfgSaveDue(&cfg, &flash, 3001u) && mock.writes == 1,
        "A4: rapid changes debounce to one flash write");
  panelScreenCfgLoad(&reboot, &flash, 4000u);
  check(reboot.screenCfg[4] == 0x0bu && reboot.persisted,
        "A4: persisted config survives simulated reboot and reports cfgPersisted");
  mock.bytes[0] ^= 1u; panelScreenCfgLoad(&reboot, &flash, 5000u);
  check(reboot.screenCfg[4] == 0x05u, "A4: bad magic falls back safely");
  memset(&mock, 0xff, sizeof(mock)); panelScreenCfgLoad(&reboot, &flash, 6000u);
  check(reboot.screenCfg[0] == 0x05u, "A4: corrupt or absent config falls back safely");
  /* A torn page has valid magic/version but erased tail bytes; its CRC must fail. */
  panelScreenCfgLoad(&cfg, &flash, 6100u);
  (void)panelScreenCfgSet(&cfg, 0u, 0x09u, 6200u);
  (void)panelScreenCfgSaveDue(&cfg, &flash, 8200u);
  mock.bytes[16] = 0xffu; mock.bytes[17] = 0xffu; mock.bytes[18] = 0xffu;
  panelScreenCfgLoad(&reboot, &flash, 8300u);
  check(reboot.screenCfg[0] == 0x05u && !reboot.persisted,
        "A4: torn flash tail fails CRC and falls back to unpersisted defaults");

  panelScreenCfgLoad(&cfg, NULL, 0u);
  cfg.screenCfg[0] = 0x09u; cfg.screenCfg[1] = 0x03u; cfg.screenCfg[2] = 0x05u;
  check(advanceAt(&cfg, 0u, 30.0f) >= 149u && advanceAt(&cfg, 0u, 30.0f) <= 151u &&
        advanceAt(&cfg, 1u, 30.0f) >= 14u && advanceAt(&cfg, 1u, 30.0f) <= 16u &&
        advanceAt(&cfg, 2u, 30.0f) >= 29u && advanceAt(&cfg, 2u, 30.0f) <= 31u,
        "A2: synthetic clock advances at 150/15/30 seconds (+/-1)");
  cfg.screenCfg[1] &= (uint8_t)~1u;
  check(panelScreenCfgNext(&cfg, 0u, 0u) == 2u,
        "A3: rotation skips a disabled screen");
  cfg.screenCfg[0] &= (uint8_t)~1u; cfg.screenCfg[2] &= (uint8_t)~1u;
  check(panelScreenCfgEnabled(&cfg, 0u, 1u) && panelScreenCfgNext(&cfg, 0u, 1u) == 2u,
        "A3: all-disabled active theme falls back to all enabled");
  {
    int advanced;
    float elapsed = rescaleDwell(20.0f, 30.0f, 7.5f, &advanced);
    check(elapsed > 4.99f && elapsed < 5.01f && !advanced,
          "A2: 20/30*7.5 = 5 seconds, no immediate advance");
    /* A delayed control can arrive after the current dwell passed: 8/7.5
     * * 1.875 = 2, so the handler must advance in that same control step. */
    elapsed = rescaleDwell(8.0f, 7.5f, 1.875f, &advanced);
    check(elapsed == 0.0f && advanced,
          "A2: rescaled elapsed at new dwell advances immediately");
  }

  memset(&sent, 0, sizeof(sent));
  sent.gearCount = 9u;
  for (int i = 0; i < 9; ++i) { sent.gear[i].role = (uint8_t)(i % 5); sent.gear[i].state = (uint8_t)(i % 3); sent.gear[i].rxLevel = (uint8_t)i; sent.gear[i].txLevel = (uint8_t)(i % 8); }
  len = panelEncodeSnapshot(&sent, payload, sizeof(payload));
  check(len != 0u && panelDecodeSnapshot(payload, len, &got) == 0 && got.gearCount == 9u &&
        got.gear[8].txLevel == 0u, "A5: v1 additive nine-gear snapshot round trip");
  check(panelDecodeSnapshot(payload, len - 37u, &got) == 0 && got.gearCount == 0u,
        "A6: legacy payload with no gear section decodes as no gear");
  payload[len - 37u] = 13u;
  check(panelDecodeSnapshot(payload, len, &got) == 0 && got.gearCount == 0u,
        "A6: oversized additive gear count decodes as no gear");
  /* Review M2: a PRESENT but SHORT gear trailer must be no-data, never a
   * partial/OOB read — this is the case that catches deleting the
   * `len < off + 4*gears` guard (mutation (b) previously survived). */
  payload[len - 37u] = 9u;
  check(panelDecodeSnapshot(payload, len - 4u, &got) == 0 && got.gearCount == 0u,
        "A6: short additive gear trailer decodes as no gear");
  memset(config, 0x05, sizeof(config));
  check(panelEncodeConfig(config, 1u, payload, sizeof(payload)) == PANEL_CONFIG_SIZE &&
        panelDecodeConfig(payload, PANEL_CONFIG_SIZE, config, &payload[20]) == 0 && payload[20] == 1u,
        "A5: CONFIG round trip");
  payload[0] = 0x15u;
  check(panelDecodeConfig(payload, PANEL_CONFIG_SIZE, config, &payload[20]) == -1,
        "A5: CONFIG rejects nonzero screenCfg reserved bits");
  (void)panelEncodeConfig(config, 1u, payload, sizeof(payload)); payload[13] = 1u;
  check(panelDecodeConfig(payload, PANEL_CONFIG_SIZE, config, &payload[20]) == -1,
        "A5: CONFIG rejects nonzero reserved payload bytes");
  feedFrame(PANEL_FT_SNAPSHOT, payload, 0u, &counts); /* establishes empty SNAPSHOT dispatch path separately below */
  len = panelEncodeSnapshot(&sent, payload, sizeof(payload));
  feedFrame(PANEL_FT_SNAPSHOT, payload, len, &counts);
  (void)panelEncodeConfig(config, 1u, payload, sizeof(payload));
  feedFrame(PANEL_FT_CONFIG, payload, PANEL_CONFIG_SIZE, &counts);
  (void)panelEncodeControl(1u, PANEL_CTL_SCREENEN, (uint8_t)((11u << 1) | 1u), payload, sizeof(payload));
  feedFrame(PANEL_FT_CONTROL, payload, PANEL_CONTROL_SIZE, &counts);
  (void)panelEncodeControl(2u, PANEL_CTL_SCREENWT, (uint8_t)((11u << 3) | 5u), payload, sizeof(payload));
  feedFrame(PANEL_FT_CONTROL, payload, PANEL_CONTROL_SIZE, &counts);
  check(counts.snapshot == 1 && counts.config == 1 && counts.control == 2,
        "A5: parser dispatches v1 additive SNAPSHOT, CONFIG, and kinds 8/9");
  {
    PanelParser parser;
    uint8_t frame[PANEL_HDR_SIZE + PANEL_MAX_PAYLOAD + PANEL_CRC_SIZE];
    size_t frameLen, j;
    ParserCounts boundary = { 0, 0, 0, 0 };
    panelParserInit(&parser);
    (void)panelEncodeState(0u, 0u, 1u, 1u, 0u, 0u, 0u, 0u, 0u, 0u,
                           payload, sizeof(payload));
    frameLen = panelEncodeFrame(PANEL_FT_STATE, payload, PANEL_STATE_SIZE, frame, sizeof(frame));
    for (j = 0u; j < frameLen; ++j) panelParserFeed(&parser, frame + j, 1u, (uint32_t)j, receive, &boundary);
    frameLen = panelEncodeFrame(0x86u, payload, 0u, frame, sizeof(frame));
    for (j = 0u; j < frameLen; ++j) panelParserFeed(&parser, frame + j, 1u, (uint32_t)(100u + j), receive, &boundary);
    (void)panelEncodeConfig(config, 1u, payload, sizeof(payload));
    frameLen = panelEncodeFrame(PANEL_FT_CONFIG, payload, PANEL_CONFIG_SIZE, frame, sizeof(frame));
    for (j = 0u; j < frameLen; ++j) panelParserFeed(&parser, frame + j, 1u, (uint32_t)(200u + j), receive, &boundary);
    check(boundary.state == 1 && boundary.config == 1 && boundary.snapshot == 0 &&
          boundary.control == 0, "A5: 0x84/0x85 dispatch; valid 0x86 skips without desync");
  }
  return gFailures == 0 ? 0 : 1;
}
