/*
 * panelScreensC.c — Theme C · CHARTS.
 * DESIGN-BRIEF "Theme C · Charts"; ported from the Turn 3 prototype's
 * sC0/sC1/sC2 (Themes.dc.html lines 403-462).
 *
 * DEVIATION common to C0 and C1: the prototype scrolls a STATIC 53-sample
 * waveform with an animation offset (`off = floor(t*2.2) % W`, line 413, and
 * `floor(t*2.4) % W`, line 424) because its data never changed. Here the rings
 * genuinely advance one sample per snapshot, so the traces are indexed
 * chronologically — oldest at the left edge, newest at the right — and the
 * synthetic offset is dropped. Without this the display would scroll real
 * history past itself twice over and read as noise.
 */

#include <math.h>
#include "panelScreens.h"

/* ---- C0 · Load distribution --------------------------------------------
 * DESIGN-BRIEF: 35-bucket CPU-load histogram over x=0..34, bar height
 * max(1, round(share^0.62 * 7)) rising from row 7, colour quiet below the 75th
 * percentile bucket, warn 75-90%, crit above 90%, tip pixel x1.5; row 8 tick
 * every 7th column; dim dashed divider at x=36; right block x=38..52 is a
 * 15-sample throughput ribbon, ingress growing up from y=4, egress down
 * from y=6.                                                                 */
void panelScreenC0(const PanelEnv *env, float t, float dt) {
  for (int b = 0; b < PANEL_LOAD_BUCKETS; b++) {
    float share = env->loadHist[b];
    float pct = ((float)b + 0.5f) / (float)PANEL_LOAD_BUCKETS;
    int h = share <= 0.0f ? 0 : (int)(powf(share, 0.62f) * 7.0f + 0.5f);
    if (share > 0.0f && h < 1) h = 1;
    PanelColor *col = pct > 0.9f ? &cCrit : pct > 0.75f ? &cWarn : &cQuiet;
    float base = pct > 0.9f ? 0.8f : pct > 0.75f ? 0.5f : 0.12f + pct * 0.15f;
    for (int k = 0; k < h; k++)
      panelFbSet(b, 7 - k, *col, base * (k == h - 1 ? 1.5f : 1.0f));
  }
  for (int x = 0; x < PANEL_LOAD_BUCKETS; x++)
    panelFbSet(x, 8, cQuiet, (x % 7) == 0 ? 0.12f : 0.03f);

  /* 15 newest samples, oldest of the 15 at x=38. */
  for (int k = 0; k < 15; k++) {
    int idx = PANEL_HIST_LEN - 15 + k;
    float v = panelHistAt(env, env->thru, idx);
    float o = panelHistAt(env, env->egress, idx);
    int x = 38 + k;
    int hi = (int)(v * 4.0f + 0.5f); if (hi < 1) hi = 1;
    int ho = (int)(o * 4.0f + 0.5f); if (ho < 1) ho = 1;
    for (int j = 0; j < hi; j++)
      panelFbSet(x, 4 - j, v > 0.85f ? cWarn : cAzure, j == hi - 1 ? 0.55f : 0.14f);
    for (int j = 0; j < ho; j++)
      panelFbSet(x, 6 + j, cAzure, j == ho - 1 ? 0.38f : 0.11f);
  }
  for (int y = 0; y < PANEL_H; y++)
    panelFbSet(36, y, cQuiet, (y % 2) ? 0.05f : 0.1f);
}

/* ---- C1 · Live traces ---------------------------------------------------
 * DESIGN-BRIEF: three stacked 3-row traces across all 53 columns —
 * throughput y=0..2 (azure, hot above 0.85), CPU y=4..6 (ok, hot above 0.8),
 * RTT y=8..10 (azure, hot above 0.7). Bar height round(v*3), tip brighter;
 * faint separator dots every 3 columns at y=3 and y=7.                      */
void panelScreenC1(const PanelEnv *env, float t, float dt) {
  struct { const float *data; int y0; PanelColor *col; float hot; } traces[3] = {
    { env->thru,    0, &cAzure, 0.85f },
    { env->cpuHist, 4, &cOk,    0.80f },
    { env->rttHist, 8, &cAzure, 0.70f }
  };
  for (int ti = 0; ti < 3; ti++) {
    for (int x = 0; x < PANEL_W; x++) {
      float v = panelHistAt(env, traces[ti].data, x);
      int h = (int)(v * 3.0f + 0.5f); if (h < 1) h = 1;
      PanelColor *col = v > traces[ti].hot ? &cWarn : traces[ti].col;
      for (int k = 0; k < h; k++) {
        int y = traces[ti].y0 + 3 - 1 - k;
        panelFbSet(x, y, *col, k == h - 1 ? 0.5f : 0.12f);
      }
    }
  }
  for (int x = 0; x < PANEL_W; x += 3) {
    panelFbAdd(x, 3, cQuiet, 0.05f);
    panelFbAdd(x, 7, cQuiet, 0.05f);
  }
}

/* ---- C2 · Pool load -----------------------------------------------------
 * DESIGN-BRIEF: up to 7 pools worst-first, one row each from y=2 (rows past
 * y=8 dropped); column 0 is the pool status key; columns 2..47 a 46-wide load
 * bar coloured azure/warn/crit at the 70%/85% thresholds with a brighter tip;
 * tier-0 pools get a crit marker at x=49; row 0 a faint tick every 4th column;
 * "BY POOL" at (2,9) in quiet b=0.28.                                       */
void panelScreenC2(const PanelEnv *env, float t, float dt) {
  int n = env->poolCount > 7 ? 7 : env->poolCount;
  for (int i = 0; i < n; i++) {
    const PanelPoolView *p = &env->pools[env->poolWorstFirst[i]];
    int y = i + 2;
    if (y > 8) break;
    PanelColor *key = p->down ? &cCrit : p->degraded ? &cWarn
                    : p->unknown ? &cUnknown : p->maint ? &cMaint : &cQuiet;
    /* Severity order is down > degraded > unknown > maint (DEVIATION #8),
     * the same order D1 uses — the two pool screens must not disagree. */
    float kb = p->down ? 0.9f : p->degraded ? 0.6f : p->unknown ? 0.4f
             : p->maint ? 0.3f : 0.12f;
    panelFbSet(0, y, *key, kb);
    int len = (int)(p->load * 46.0f + 0.5f);
    for (int k = 0; k < 46; k++) {
      int on = k < len;
      float frac = (float)k / 46.0f;
      PanelColor *col = frac > 0.85f ? &cCrit : frac > 0.7f ? &cWarn : &cAzure;
      panelFbSet(2 + k, y, on ? *col : cQuiet,
                 on ? (k == len - 1 ? 0.6f : 0.22f) : 0.03f);
    }
    if (p->tier == 0) panelFbSet(49, y, cCrit, 0.18f);
  }
  for (int x = 0; x < PANEL_W; x++)
    panelFbSet(x, 0, cQuiet, (x % 4) == 0 ? 0.08f : 0.02f);
  /* DOC1 review: a 5-row label at y=9 clipped to two broken rows on an
   * 11-row panel. The pool rows themselves identify the screen; the label
   * only ever rendered as noise. Removed. */
}
