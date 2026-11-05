/*
 * panelScreensC.c — Theme C · TROUBLE: what is wrong, in plain words.
 */

#include <math.h>
#include <stdio.h>
#include "panelScreens.h"

static const float *sevColor(uint8_t severity) {
  if (severity == PANEL_SEV_CRIT) return cCrit;
  if (severity == PANEL_SEV_WARN) return cWarn;
  return cAzure;
}

/* Age in the largest whole unit: 5M, 3H, 12D. */
static void ageLabel(unsigned minutes, char *out, size_t cap) {
  if (minutes < 60u)        snprintf(out, cap, "%uM", minutes);
  else if (minutes < 1440u) snprintf(out, cap, "%uH", minutes / 60u);
  else                      snprintf(out, cap, "%uD", minutes / 1440u);
}

/* ---- C0 · Alerts ----------------------------------------------------------
 * One open condition a page: the host and its age, then what is wrong.       */
void panelScreenC0(const PanelEnv *env, float t, float dt) {
  (void)t;
  const PanelSnapshot *s = &env->snap;
  if (!s->hasAlerts) {
    panelTextCentre(PANEL_ROW_ONE, "NO ALERT DATA", cAzure, 1.0f);
    return;
  }
  if (s->alertTotal == 0u || s->alertCount == 0u) {
    panelTextCentre(PANEL_ROW_ONE, "ALL CLEAR", cOk, 1.0f);
    return;
  }
  int pages = panelScreenPages(env, 8u);
  int page = panelPageTick(8u, dt, pages);
  if (page >= s->alertCount) {
    char more[16];
    snprintf(more, sizeof more, "+%d MORE", s->alertTotal - s->alertCount);
    panelTextCentre(PANEL_ROW_TOP, more, cInk, 1.0f);
    panelTextCentre(PANEL_ROW_BOT, "SEE DASHBOARD", cAzure, 1.0f);
  } else {
    __typeof__(s->alerts[0]) const *a = &s->alerts[page];
    char age[8];
    ageLabel(a->ageMin, age, sizeof age);
    panelTextFit(0, PANEL_ROW_TOP, a->host, 9, cInk, 1.0f);
    panelTextRight(PANEL_W - 1, PANEL_ROW_TOP, age, cAzure, 1.0f);
    panelTextCentre(PANEL_ROW_BOT, a->msg, sevColor(a->severity), 1.0f);
  }
  panelPageBar(page, pages);
}

/* One fault cell: label and count, lit in its hue when non-zero. */
static void faultCell(int x, int y, const char *label, unsigned count, const float *hue) {
  char cell[8];
  if (count > 99u) count = 99u;
  snprintf(cell, sizeof cell, "%s %u", label, count);
  panelText(x, y, cell, count ? hue : cOk, 1.0f);
}

/* ---- C1 · Fault Board -----------------------------------------------------
 * Open conditions by kind. Green zeros are information too.                  */
void panelScreenC1(const PanelEnv *env, float t, float dt) {
  (void)t;
  const PanelSnapshot *s = &env->snap;
  if (!s->hasAlerts) {
    panelTextCentre(PANEL_ROW_ONE, "NO ALERT DATA", cAzure, 1.0f);
    return;
  }
  int pages = panelScreenPages(env, 9u);
  int page = panelPageTick(9u, dt, pages);
  if (page == 1) {
    char line[16];
    snprintf(line, sizeof line, "OTHER %u", s->faultCounts[PANEL_FAULT_OTHER]);
    panelTextCentre(PANEL_ROW_TOP, line, cWarn, 1.0f);
    panelTextCentre(PANEL_ROW_BOT, "SEE DASHBOARD", cAzure, 1.0f);
  } else {
    unsigned disk = (unsigned)s->faultCounts[PANEL_FAULT_READONLY] +
                    s->faultCounts[PANEL_FAULT_SMART] +
                    s->faultCounts[PANEL_FAULT_DISKGONE];
    faultCell(0,  PANEL_ROW_TOP, "NET", s->faultCounts[PANEL_FAULT_REACH], cCrit);
    faultCell(28, PANEL_ROW_TOP, "SVC", s->faultCounts[PANEL_FAULT_UNITS], cWarn);
    faultCell(0,  PANEL_ROW_BOT, "DSK", disk, cWarn);
    faultCell(28, PANEL_ROW_BOT, "KRN", s->faultCounts[PANEL_FAULT_KERNEL], cWarn);
  }
  panelPageBar(page, pages);
}

/* ---- C2 · Reachability ----------------------------------------------------
 * One row per probe target, newest on the right. Rows share the height, so three
 * targets make thick bands. Green is at the row's own baseline, amber is at least
 * twice it, red is lost. A target that has never answered stays grey.        */
void panelScreenC2(const PanelEnv *env, float t, float dt) {
  (void)t; (void)dt;
  const PanelSnapshot *s = &env->snap;
  int n = s->hasProbes ? s->probeCount : 0;
  if (n == 0) {
    panelTextCentre(PANEL_ROW_ONE, "NO PROBE DATA", cAzure, 1.0f);
    return;
  }
  if (n > (int)PANEL_MAX_PROBES) n = (int)PANEL_MAX_PROBES;
  int gap = n <= 6 ? 1 : 0;
  for (int p = 0; p < n; p++) {
    int y0 = p * (PANEL_H + gap) / n;
    int y1 = (p + 1) * (PANEL_H + gap) / n - gap;
    const uint16_t *row = env->probeHist[p];
    uint16_t base = PANEL_PROBE_LOST;
    for (int c = 0; c < PANEL_HIST_LEN; c++)
      if (row[c] < base && row[c] > 0u) base = row[c];
    int dead = s->probes[p].state == PANEL_ST_UNKNOWN;
    for (int c = 0; c < PANEL_HIST_LEN; c++) {
      uint16_t cell = row[c];
      const float *hue;
      float b;
      if (cell == PANEL_PROBE_EMPTY) continue;
      if (dead)                           { hue = cUnknown; b = 0.25f; }
      else if (cell == PANEL_PROBE_LOST)  { hue = cCrit;    b = 1.0f; }
      else if (base != PANEL_PROBE_LOST && cell >= base * 2u + 5u)
                                          { hue = cWarn;    b = 1.0f; }
      else                                { hue = cOk;      b = 0.4f; }
      for (int y = y0; y < y1; y++) panelFbSet(c, y, hue, b);
    }
  }
}

/* ---- C3 · Alert History ---------------------------------------------------
 * Alert firings per half hour across 26 hours, on a log height so one bad night
 * does not flatten the rest. Critical sits above warning; column 52 marks now. */
void panelScreenC3(const PanelEnv *env, float t, float dt) {
  (void)t; (void)dt;
  const PanelSnapshot *s = &env->snap;
  if (!s->hasHeat) {
    panelTextCentre(PANEL_ROW_ONE, "NO HISTORY", cAzure, 1.0f);
    return;
  }
  int peak = 3;
  for (int i = 0; i < (int)PANEL_HEAT_LEN; i++) {
    int total = s->heatWarn[i] + s->heatCrit[i];
    if (total > peak) peak = total;
  }
  float scale = (float)PANEL_H / log2f((float)peak + 1.0f);
  for (int i = 0; i < (int)PANEL_HEAT_LEN; i++) {
    int warn = s->heatWarn[i], crit = s->heatCrit[i];
    int h = (int)ceilf(log2f((float)(warn + crit) + 1.0f) * scale);
    int hc = crit ? (int)ceilf(log2f((float)crit + 1.0f) * scale) : 0;
    if (h > PANEL_H) h = PANEL_H;
    if (hc > h) hc = h;
    for (int r = 0; r < h; r++)
      panelFbSet(i, PANEL_H - 1 - r, r >= h - hc ? cCrit : cWarn, 1.0f);
    /* A dim tick every six hours gives the empty stretches a scale. */
    if (h == 0 && ((int)PANEL_HEAT_LEN - 1 - i) % 12 == 0)
      panelFbSet(i, PANEL_H - 1, cAzure, 0.3f);
  }
  for (int y = 0; y < PANEL_H; y++) panelFbSet(PANEL_W - 1, y, cAzure, 0.45f);
}
