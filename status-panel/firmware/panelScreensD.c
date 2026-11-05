/*
 * panelScreensD.c — Theme D · RESOURCES: how hard the hosts are working.
 */

#include <math.h>
#include <stdio.h>
#include "panelScreens.h"

/* mem/disk come from agents; a host without one reports >100 (CONTRACT-V2 §3). */
#define PCT_VALID(p) ((p) <= 100u)

/* Usage hue: the metric's own until it is a problem. */
static const float *usageColor(unsigned pct, const float *own) {
  if (pct >= 90u) return cCrit;
  if (pct >= 75u) return cWarn;
  return own;
}

/* Letter plus value: C45, up to D100 (15 px). Input: position, letter, pct. */
static void metric(int x, int y, const char *letter, unsigned pct, const float *own) {
  char num[6];
  x = panelText(x, y, letter, cInk, 1.0f);
  if (PCT_VALID(pct)) snprintf(num, sizeof num, "%u", pct);
  else                snprintf(num, sizeof num, "--");
  panelText(x, y, num, PCT_VALID(pct) ? usageColor(pct, own) : cUnknown, 1.0f);
}

/* ---- D0 · Host Detail -----------------------------------------------------
 * One host a page, worst first: name and state, then CPU, memory and disk.   */
void panelScreenD0(const PanelEnv *env, float t, float dt) {
  (void)t;
  const PanelSnapshot *s = &env->snap;
  if (s->systemCount == 0) return;
  int pages = panelScreenPages(env, 12u);
  int page = panelPageTick(12u, dt, pages);
  __typeof__(s->systems[0]) const *sys = &s->systems[page];
  const float *hue = panelStateColor(sys->state);
  const char *word;
  switch (sys->state) {
    case PANEL_ST_OK:       word = "OK"; break;
    case PANEL_ST_DEGRADED: word = "DEG"; break;
    case PANEL_ST_DOWN:     word = "DOWN"; break;
    case PANEL_ST_MAINT:    word = "MNT"; break;
    default:                word = "?"; break;
  }
  panelTextFit(0, PANEL_ROW_TOP, sys->name, 8, cInk, 1.0f);
  panelTextRight(PANEL_W - 1, PANEL_ROW_TOP, word, hue, 1.0f);
  if (sys->state == PANEL_ST_DOWN || sys->state == PANEL_ST_UNKNOWN) {
    panelTextCentre(PANEL_ROW_BOT, "NO READINGS", cUnknown, 1.0f);
  } else {
    metric(0, PANEL_ROW_BOT, "C", sys->loadPct, cAzure);
    unsigned mem = s->hasSysx ? s->sysx[page].memPct : 0xFFu;
    unsigned disk = s->hasSysx ? s->sysx[page].diskPct : 0xFFu;
    metric(19, PANEL_ROW_BOT, "M", mem, cMaint);
    metric(38, PANEL_ROW_BOT, "D", disk, cOk);
  }
  panelPageBar(page, pages);
}

/* ---- D1 · CPU and Memory --------------------------------------------------
 * A butterfly: CPU rises from the centre, memory falls from it, the centre row
 * carries each host's state. Columns take whatever width the fleet leaves.   */
void panelScreenD1(const PanelEnv *env, float t, float dt) {
  (void)t; (void)dt;
  const PanelSnapshot *s = &env->snap;
  int n = s->systemCount;
  if (n <= 0) return;
  if (n > PANEL_W) n = PANEL_W;
  int gap = PANEL_W >= n * 2 ? 1 : 0;
  for (int i = 0; i < n; i++) {
    int x0 = i * (PANEL_W + gap) / n;
    int x1 = (i + 1) * (PANEL_W + gap) / n - gap;
    __typeof__(s->systems[0]) const *sys = &s->systems[i];
    const float *hue = panelStateColor(sys->state);
    unsigned mem = s->hasSysx ? s->sysx[i].memPct : 0xFFu;
    int measured = sys->state != PANEL_ST_DOWN && sys->state != PANEL_ST_UNKNOWN;
    int up = measured ? (sys->loadPct * 5 + 50) / 100 : 0;
    int dn = measured && PCT_VALID(mem) ? (int)(mem * 5u + 50u) / 100 : 0;
    /* Any real reading shows at least one row: idle is not the same as absent. */
    if (measured && up < 1 && sys->loadPct > 0u) up = 1;
    if (dn < 1 && measured && PCT_VALID(mem) && mem > 0u) dn = 1;
    for (int x = x0; x < x1; x++) {
      panelFbSet(x, 5, hue, 1.0f);
      for (int r = 1; r <= 5; r++) {
        if (r <= up) panelFbSet(x, 5 - r, usageColor(sys->loadPct, cAzure), 1.0f);
        if (r <= dn) panelFbSet(x, 5 + r, usageColor(mem, cMaint), 1.0f);
      }
    }
  }
}

/* One 3-row trend strip: real samples, newest on the right, dark before they
 * exist. `level` maps a ring value to 0..1. */
static void trendStrip(const PanelEnv *env, const float *ring, const float *ring2,
                       float peak, int y, const float *hue) {
  for (int a = 0; a < PANEL_W; a++) {
    float v = panelHistAge(env, ring, a);
    if (ring2) {
      float v2 = panelHistAge(env, ring2, a);
      if (v2 > 0.0f) v = (v > 0.0f ? v : 0.0f) + v2;
    }
    if (v < 0.0f) continue;
    float level = v / peak;
    if (level > 1.0f) level = 1.0f;
    float rows = level * 3.0f;
    int x = PANEL_W - 1 - a;
    for (int r = 0; r < 3; r++) {
      float cover = rows - (float)r;
      if (cover <= 0.0f) break;
      panelFbSet(x, y + 2 - r, hue, cover >= 1.0f ? 1.0f : 0.25f + 0.75f * cover);
    }
  }
}

/* ---- D2 · Trends ----------------------------------------------------------
 * Last 106 s: internet traffic, mean CPU, ping. Each strip scales to its own
 * window, with a floor (CPU 40%, ping 10 ms) so that jitter on a calm network
 * has visible shape without reading as a crisis.                              */
void panelScreenD2(const PanelEnv *env, float t, float dt) {
  (void)t; (void)dt;
  float wanPeak = 1.0f, rttPeak = 10.0f, cpuPeak = 0.4f;
  for (int a = 0; a < PANEL_W; a++) {
    float dn = panelHistAge(env, env->wanDown, a), up = panelHistAge(env, env->wanUp, a);
    float total = (dn > 0.0f ? dn : 0.0f) + (up > 0.0f ? up : 0.0f);
    float r = panelHistAge(env, env->rttHist, a);
    if (total > wanPeak) wanPeak = total;
    float cpu = panelHistAge(env, env->cpuHist, a);
    if (r > rttPeak) rttPeak = r;
    if (cpu > cpuPeak) cpuPeak = cpu;
  }
  trendStrip(env, env->wanDown, env->wanUp, wanPeak, 0, cInk);
  trendStrip(env, env->cpuHist, NULL, cpuPeak, 4, cAzure);
  trendStrip(env, env->rttHist, NULL, rttPeak, 8, cMaint);
}

/* ---- D3 · Aurora ----------------------------------------------------------
 * The ambient screen, driven by measurements. Each host owns a band of columns:
 * its curtain's height is its load, its hue its state, and the drift speed is
 * the network's real traffic. A failed host is a red pillar. The rays are only
 * texture; they carry no reading.                                            */
void panelScreenD3(const PanelEnv *env, float t, float dt) {
  static float phase;
  (void)t;
  int n = env->tileCount;
  if (n <= 0) return;
  const PanelSnapshot *s = &env->snap;
  float total = 0.0f;
  if (s->hasTotals) {
    if (s->wanDownKbps != PANEL_RATE_NONE) total += (float)s->wanDownKbps;
    if (s->wanUpKbps != PANEL_RATE_NONE) total += (float)s->wanUpKbps;
    if (s->lanDownKbps != PANEL_RATE_NONE) total += (float)s->lanDownKbps;
    if (s->lanUpKbps != PANEL_RATE_NONE) total += (float)s->lanUpKbps;
  }
  phase = fmodf(phase + (0.15f + 2.2f * panelRateLevel(total)) * dt, 6.2831853f * 16.0f);

  for (int x = 0; x < PANEL_W; x++) {
    /* Neighbouring bands blend so the curtain has no hard seams. */
    float pos = ((float)x + 0.5f) * (float)n / (float)PANEL_W - 0.5f;
    int a = (int)floorf(pos), bIdx = a + 1;
    float mix = pos - (float)a;
    if (a < 0) a = 0;
    if (bIdx >= n) bIdx = n - 1;
    const PanelTile *ta = &env->tiles[a], *tb = &env->tiles[bIdx];
    const PanelTile *own = mix < 0.5f ? ta : tb;
    if (own->state == PANEL_ST_DOWN) {
      for (int y = 0; y < PANEL_H; y++) panelFbSet(x, y, cCrit, 1.0f);
      continue;
    }
    float load = ta->load + (tb->load - ta->load) * mix;
    const float *hue = own->state == PANEL_ST_OK ? cAzure : panelStateColor(own->state);
    const float *top = own->state == PANEL_ST_OK ? cOk : hue;
    float height = 4.0f + load * 7.0f;
    float wave = sinf(phase + (float)x * 0.28f) * 1.3f +
                 sinf(phase * 0.61f + (float)x * 0.11f) * 0.9f;
    float crest = (float)PANEL_H - height + wave;
    float ray = 0.62f + 0.38f * sinf(phase * 2.3f + (float)x * 1.35f);
    for (int y = 0; y < PANEL_H; y++) {
      float d = (float)y - crest;
      if (d < -0.5f) continue;
      float fall = 1.0f / (1.0f + d * 0.28f);
      if (d < 1.0f) panelFbSet(x, y, top, 1.0f);
      else          panelFbSet(x, y, hue, ray * fall);
    }
  }
}
