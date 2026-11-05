/* panelScreensATest.c — every screen at every population, under ASAN + UBSAN.
 *
 * CONTRACT-V2 §5 and §2 as executable rules: at fleets of 0..255 hosts and 0..12
 * links, each screen runs through all its pages with the widest names and values
 * the wire allows, and
 *   - no pixel leaves the matrix,
 *   - small text stands only on rows 0, 3 or 6 and never runs off either edge,
 *   - no two text runs in one frame share pixels.
 * The baseline panel broke every one of these; they stay broken-proof here. */
#include <stdio.h>
#include <string.h>

#include "../panelScreens.h"
#include "../panelHelpOverlay.h"

static int gFailures = 0;
static const char *gCase = "";
static char gNote[80];

static void failf(const char *what, int a, int b, const char *s) {
  if (gFailures++ < 25)
    printf("FAIL %s: %s (%d,%d) \"%s\"\n", gCase, what, a, b, s ? s : "");
}

void panelHwSetPixel(int x, int y, unsigned char r, unsigned char g,
                     unsigned char b) {
  (void)r; (void)g; (void)b;
  if (x < 0 || x >= PANEL_W || y < 0 || y >= PANEL_H)
    failf("pixel out of bounds", x, y, NULL);
}

/* One frame's text footprint: which cells text has claimed. */
static unsigned char gClaim[PANEL_H][PANEL_W];

static void claim(int x0, int x1, int y0, int y1, const char *s) {
  for (int y = y0; y < y1 && y < PANEL_H; y++)
    for (int x = x0; x < x1; x++) {
      if (x < 0 || x >= PANEL_W || y < 0) continue;
      if (gClaim[y][x]) { failf("text overlaps text", x, y, s); return; }
      gClaim[y][x] = 1u;
    }
}

int __real_panelText(int x, int y, const char *s, PanelColor c, float b);
int __wrap_panelText(int x, int y, const char *s, PanelColor c, float b) {
  int w = panelTextW(s);
  if (y != PANEL_ROW_TOP && y != PANEL_ROW_ONE && y != PANEL_ROW_BOT)
    failf("text off the text rows", x, y, s);
  if (x < 0 || x + w > PANEL_W) failf("text runs off the panel", x, y, s);
  claim(x, x + w, y, y + 5, s);
  return __real_panelText(x, y, s, c, b);
}

int __real_panelBig(int x, int y, const char *s, PanelColor c, float b);
int __wrap_panelBig(int x, int y, const char *s, PanelColor c, float b) {
  claim(x, x + (int)strlen(s) * 5 - 1, y, y + 7, s);
  return __real_panelBig(x, y, s, c, b);
}

static const PanelScreenFn kScreens[PANEL_SCREENS] = {
  panelScreenA0, panelScreenA1, panelScreenA2, panelScreenA3,
  panelScreenB0, panelScreenB1, panelScreenB2, panelScreenB3,
  panelScreenC0, panelScreenC1, panelScreenC2, panelScreenC3,
  panelScreenD0, panelScreenD1, panelScreenD2, panelScreenD3
};

/* The widest legal snapshot of a given size: long names, peak numbers, every
 * state in the mix, plus a parent chain that loops (UniFi really reports them). */
static void buildSnapshot(PanelSnapshot *s, int hosts, int links, int mode) {
  static const uint8_t states[5] = {
    PANEL_ST_DOWN, PANEL_ST_DEGRADED, PANEL_ST_UNKNOWN, PANEL_ST_MAINT, PANEL_ST_OK
  };
  int named = hosts > (int)PANEL_MAX_SYSTEMS ? (int)PANEL_MAX_SYSTEMS : hosts;
  memset(s, 0, sizeof *s);
  s->systemCount = (uint8_t)named;
  s->poolCount = hosts ? 5u : 0u;
  s->hasSysx = mode != 2;
  s->hasAlerts = mode != 2;
  s->hasProbes = mode != 2;
  s->hasHeat = mode != 2;
  s->hasTotals = mode != 2;
  s->hasLinks = mode != 2;
  s->hasTopAlert = 1u;
  s->alarmActive = 1u;
  s->dataStale = mode == 1;
  s->meanLoadPct = 100u;
  snprintf(s->topAlert.subject, sizeof s->topAlert.subject, "WWWWWWWWWWWWWWWWWWWW");
  snprintf(s->topAlert.detail, sizeof s->topAlert.detail, "WWWWWWWWWWWWWWWWWWWWWWWWW");
  for (int i = 0; i < named; i++) {
    uint8_t st = mode == 0 ? states[i % 5] : (uint8_t)PANEL_ST_OK;
    snprintf(s->systems[i].name, sizeof s->systems[i].name, "WWWWWWWWWWWW");
    s->systems[i].state = st;
    s->systems[i].loadPct = (uint8_t)(i % 2 ? 100 : 0);
    s->sysx[i].memPct = (uint8_t)(i % 3 == 0 ? 100 : (i % 3 == 1 ? 0 : 0xFF));
    s->sysx[i].diskPct = (uint8_t)(i % 3 == 0 ? 100 : 0xFF);
    s->sysx[i].rxKbps = i % 3 == 2 ? PANEL_RATE_NONE : 4000000000u;
    s->sysx[i].txKbps = i % 3 == 2 ? PANEL_RATE_NONE : 200000000u;
    s->stateRoll[st]++;
  }
  for (int i = named; i < hosts; i++) {
    uint8_t st = states[i % 5];
    s->pop[s->popCount++] = (uint8_t)((st << 5) | (i % 32));
    s->stateRoll[st]++;
  }
  for (int i = 0; i < (int)s->poolCount; i++) {
    snprintf(s->pools[i].name, sizeof s->pools[i].name, "WWWWWWWW");
    s->pools[i].total = (uint8_t)(hosts > 255 ? 255 : hosts);
    s->pools[i].ok = s->pools[i].total;
    s->pools[i].loadPct = 100u;
  }
  s->linkCount = (uint8_t)links;
  for (int i = 0; i < links; i++) {
    snprintf(s->links[i].name, sizeof s->links[i].name, "WWWWWWWWWWWW");
    s->links[i].kind = (uint8_t)(i % 4);
    s->links[i].state = i % 5 == 4 ? PANEL_ST_DOWN : PANEL_ST_OK;
    /* 0 is the root, 2<->3 is a loop, the rest chain. */
    s->links[i].parent = i == 0 ? 0xFFu : (i == 2 ? 3u : (i == 3 ? 2u : (uint8_t)(i - 1)));
    s->links[i].upKbps = i % 3 == 0 ? PANEL_RATE_NONE : 4000000000u;
    s->links[i].downKbps = i % 3 == 1 ? 0u : 1u;
  }
  s->alertTotal = 255u;
  s->alertCount = PANEL_MAX_ALERTS;
  for (int i = 0; i < (int)PANEL_FAULT_CODES; i++) s->faultCounts[i] = 255u;
  for (int i = 0; i < (int)PANEL_MAX_ALERTS; i++) {
    s->alerts[i].severity = (uint8_t)(i % 3);
    s->alerts[i].ageMin = 65535u;
    snprintf(s->alerts[i].host, sizeof s->alerts[i].host, "WWWWWWWWWWWW");
    snprintf(s->alerts[i].msg, sizeof s->alerts[i].msg, "WWWWWWWWWWWWWWWWWW");
  }
  s->probeCount = PANEL_MAX_PROBES;
  for (int i = 0; i < (int)PANEL_MAX_PROBES; i++) {
    snprintf(s->probes[i].name, sizeof s->probes[i].name, "P%u", (unsigned)i % 100u);
    s->probes[i].rttTenthMs = (uint16_t)(i * 6000);
    s->probes[i].lossPermille = i % 3 == 0 ? 1000u : 0u;
    s->probes[i].state = i % 4 == 3 ? PANEL_ST_UNKNOWN : PANEL_ST_OK;
  }
  for (int i = 0; i < (int)PANEL_HEAT_LEN; i++) {
    s->heatWarn[i] = 255u;
    s->heatCrit[i] = (uint8_t)(i % 2 ? 255 : 0);
  }
  s->wanUpKbps = s->wanDownKbps = 4000000000u;
  s->lanUpKbps = PANEL_RATE_NONE;
  s->lanDownKbps = 4000000000u;
  /* Mode 3 is the healthy, silent network: the empty branches draw their own text. */
  if (mode == 3) {
    s->alertTotal = s->alertCount = 0u;
    s->probeCount = 0u;
    s->hasTopAlert = s->alarmActive = 0u;
    memset(s->faultCounts, 0, sizeof s->faultCounts);
    memset(s->heatWarn, 0, sizeof s->heatWarn);
    memset(s->heatCrit, 0, sizeof s->heatCrit);
  }
}

/* Run one screen through every page, one 25 Hz tick at a time. */
static void exercise(const PanelEnv *env, int screen) {
  int ticks = (int)(panelScreenMinDwell(env, (unsigned)screen) * 25.0f) + 100;
  /* Long fleets are capped on dwell, so page through them all regardless. */
  ticks += panelScreenPages(env, (unsigned)screen) * 80;
  for (int f = 0; f < ticks; f++) {
    float t = (float)f * 0.04f;
    panelFbClear();
    memset(gClaim, 0, sizeof gClaim);
    kScreens[screen](env, t, 0.04f);
    panelFbFlush();
  }
}

/* Purpose: long dwell caps and the tour are arithmetic, so assert them directly.
 * Input: env. Output: none. */
static void checkPager(const PanelEnv *env) {
  for (unsigned i = 0u; i < PANEL_SCREENS; i++) {
    float need = panelScreenMinDwell(env, i);
    int pages = panelScreenPages(env, i);
    if (pages < 1) failf("page count below 1", (int)i, pages, NULL);
    if (need > 19.0f) failf("dwell above its cap", (int)i, pages, NULL);
    if (strlen(panelScreenName(i)) > 13u) failf("title too long", (int)i, 0, panelScreenName(i));
  }
}

int main(void) {
  static PanelEnv env;
  static PanelSnapshot snap;
  const int fleets[] = { 0, 1, 2, 3, 9, 16, 53, 64, 65, 200, 255 };
  const int linkCounts[] = { 0, 1, 2, 3, 12 };

  for (int mode = 0; mode < 4; mode++) {
    for (size_t f = 0; f < sizeof fleets / sizeof fleets[0]; f++) {
      for (size_t l = 0; l < sizeof linkCounts / sizeof linkCounts[0]; l++) {
        panelEnvInit(&env);
        buildSnapshot(&snap, fleets[f], linkCounts[l], mode);
        /* A short history and a full one fail differently. */
        for (int rep = 0; rep < (f % 2 ? 60 : 3); rep++) panelEnvApply(&env, &snap);
        checkPager(&env);
        for (int s = 0; s < (int)PANEL_SCREENS; s++) {
          /* A fleet of zero never reaches a screen (main.c shows No Data), but
           * nothing below that gate may crash if it ever does. */
          snprintf(gNote, sizeof gNote, "mode %d, %d hosts, %d links, screen %d",
                   mode, fleets[f], linkCounts[l], s);
          gCase = gNote;
          exercise(&env, s);
        }
      }
    }
  }

  gCase = "overlays";
  panelEnvInit(&env);
  buildSnapshot(&snap, 9, 3, 0);
  panelEnvApply(&env, &snap);
  for (int f = 0; f < 50; f++) {
    float t = (float)f * 0.04f;
    panelFbClear(); memset(gClaim, 0, sizeof gClaim);
    panelInlay(&env, t);
    panelFbFlush();
    panelFbClear(); memset(gClaim, 0, sizeof gClaim);
    panelInlayPower(t);
    panelFbFlush();
    panelFbClear(); memset(gClaim, 0, sizeof gClaim);
    panelScreenLinkLost(t);
    panelFbFlush();
    panelFbClear(); memset(gClaim, 0, sizeof gClaim);
    panelScreenNoData(f % 2, t);
    panelFbFlush();
  }
  for (unsigned i = 0u; i < PANEL_SCREENS; i++) {
    panelFbClear(); memset(gClaim, 0, sizeof gClaim);
    panelHelpOverlayDraw(i, 0.0f);
    panelFbFlush();
    panelFbClear(); memset(gClaim, 0, sizeof gClaim);
    panelTitleCard(i, 0.5f);
    panelFbFlush();
  }

  printf(gFailures ? "panelScreensATest: %d failures\n" : "panelScreensATest: all clean\n", gFailures);
  return gFailures ? 1 : 0;
}
