/*
 * panelScreensCommon.c — names, pagers and text helpers shared by the screens.
 */

#include <math.h>
#include <string.h>
#include "panelScreens.h"

static const char *const kNames[PANEL_SCREENS] = {
  "HOST TILES", "ROSTER",      "POOLS",        "HEAD COUNT",
  "SWIM LANES", "WAN AND LAN", "TOP TALKERS",  "FLOW MAP",
  "ALERTS",     "FAULT BOARD", "REACHABILITY", "ALERT HISTORY",
  "HOST DETAIL", "CPU AND MEM", "TRENDS",      "AURORA"
};

const char *panelScreenName(unsigned index) {
  return index < PANEL_SCREENS ? kNames[index] : "";
}

const float *panelStateColor(uint8_t state) {
  switch (state) {
    case PANEL_ST_OK:       return cOk;
    case PANEL_ST_DEGRADED: return cWarn;
    case PANEL_ST_DOWN:     return cCrit;
    case PANEL_ST_MAINT:    return cMaint;
    default:                return cUnknown;
  }
}

/* Two-row screens show two items a page. */
static int pagesOf(int items, int perPage) {
  int pages = (items + perPage - 1) / perPage;
  return pages < 1 ? 1 : pages;
}

int panelScreenPages(const PanelEnv *env, unsigned index) {
  const PanelSnapshot *s = &env->snap;
  switch (index) {
    case 1u:  return pagesOf(s->systemCount, 2);
    case 2u:  return pagesOf(s->poolCount, 2);
    case 4u:  return pagesOf(s->hasLinks ? s->linkCount : 0, 2);
    case 6u:  return pagesOf(env->talkerCount < 8 ? env->talkerCount : 8, 2);
    case 8u:
      if (!s->hasAlerts) return 1;
      return pagesOf(s->alertCount, 1) + (s->alertTotal > s->alertCount ? 1 : 0);
    case 9u:
      return s->hasAlerts && s->faultCounts[PANEL_FAULT_OTHER] > 0u ? 2 : 1;
    case 12u: return pagesOf(s->systemCount, 1);
    default:  return 1;
  }
}

float panelScreenMinDwell(const PanelEnv *env, unsigned index) {
  int pages = panelScreenPages(env, index);
  float need;
  if (pages <= 1) return 0.0f;
  need = (float)pages * PANEL_PAGE_SEC + 1.0f;
  return need > 19.0f ? 19.0f : need;
}

int panelPageTick(unsigned index, float dt, int pages) {
  static float clocks[PANEL_SCREENS];
  if (index >= PANEL_SCREENS) return 0;
  clocks[index] += dt;
  if (pages < 1) pages = 1;
  /* fmod keeps the clock small enough for float precision over long uptimes. */
  clocks[index] = fmodf(clocks[index], PANEL_PAGE_SEC * 256.0f);
  return ((int)(clocks[index] / PANEL_PAGE_SEC)) % pages;
}

void panelPageBar(int page, int pages) {
  if (pages <= 1) return;
  int x0 = page * PANEL_W / pages, x1 = (page + 1) * PANEL_W / pages;
  for (int x = x0; x < x1; x++) panelFbSet(x, 5, cAzure, 0.45f);
}

int panelTextFit(int x, int y, const char *s, int maxChars, PanelColor c, float b) {
  char buf[16];
  size_t n = strlen(s);
  if (maxChars > 15) maxChars = 15;
  if (n > (size_t)maxChars) n = (size_t)maxChars;
  memcpy(buf, s, n);
  /* A name cut to fit must not leave a dangling space. */
  while (n > 0u && buf[n - 1u] == ' ') n--;
  buf[n] = '\0';
  return panelText(x, y, buf, c, b);
}

void panelTextRight(int xEnd, int y, const char *s, PanelColor c, float b) {
  panelText(xEnd - panelTextW(s) + 1, y, s, c, b);
}

void panelTextCentre(int y, const char *s, PanelColor c, float b) {
  char buf[14];
  size_t n = strlen(s);
  if (n > 13u) n = 13u;
  memcpy(buf, s, n);
  buf[n] = '\0';
  panelText((PANEL_W - panelTextW(buf)) / 2, y, buf, c, b);
}

float panelRateLevel(float kbps) {
  if (kbps <= 1.0f) return 0.0f;
  float level = log10f(kbps) / 6.0f;
  return level > 1.0f ? 1.0f : level;
}

void panelTitleCard(unsigned index, float age) {
  if (age < 0.0f || age >= 1.0f) return;
  panelFbDimAll(0.08f);
  panelTextCentre(PANEL_ROW_ONE, panelScreenName(index), cInk, 1.0f);
}

void panelStaleMark(void) {
  panelFbSet(0, 10, cWarn, 1.0f);
  panelFbSet(1, 10, cWarn, 1.0f);
}
