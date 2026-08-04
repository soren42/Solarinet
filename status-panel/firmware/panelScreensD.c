/*
 * panelScreensD.c — Theme D · INSTRUMENTS.
 * DESIGN-BRIEF "Theme D · Instruments"; ported from the Turn 3 prototype's
 * sD0/sample/sD1/sD2 (Themes.dc.html lines 465-541).
 */

#include <math.h>
#include <stdio.h>
#include <string.h>
#include "panelScreens.h"

/* ---- D0 · Reachability --------------------------------------------------
 * DESIGN-BRIEF: full-grid waterfall, 11 rows (one per probe target),
 * scrolling right-to-left, one new sample column every 0.42 s (shift+push).
 * Cell: crit at b=0.85 for loss, warn at b=0.5 for slow, else azure at
 * brightness 0.06 + rtt*0.26.
 *
 * DEVIATION (row identity only): the mockup's 11 rows are named probe targets
 * with per-row synthetic loss. protocol.h carries only fleet-aggregate
 * rttTenthMs / lossPermille, so each row tracks one of the 11 worst-first
 * systems instead (panelHist.c builds env->probeRow). A DOWN system samples
 * as loss; DEGRADED or UNKNOWN samples as slow at the aggregate loss rate;
 * everything else samples ok with the aggregate RTT jittered per column so the
 * waterfall keeps its texture. Cadence, geometry and colours are unchanged.  */
#define D0_LOSS 0
#define D0_SLOW 1
#define D0_OK   2

typedef struct { uint8_t st; float rtt; } D0Cell;
static D0Cell   gWf[PANEL_H][PANEL_W];
static PanelRng gWfRng;
static float    gWfStep = 0.0f;
static int      gWfReady = 0;

/* d0Sample — one waterfall cell for row i. Mirrors the prototype's sample()
 * shape: a bad row mostly loses, a soft row mostly slows, otherwise an ok
 * cell whose brightness carries the RTT. */
static D0Cell d0Sample(const PanelEnv *env, int i) {
  D0Cell c = { D0_OK, 0.15f };
  uint8_t si = (i < env->probeRows) ? env->probeRow[i] : 0xFF;
  uint8_t st = PANEL_ST_UNKNOWN;
  if (si != 0xFF && si < PANEL_MAX_SYSTEMS) st = env->snap.systems[si].state;

  float lossRate = env->loss / 100.0f;          /* 0..1 */
  float r = panelRngNext(&gWfRng);
  if (st == PANEL_ST_DOWN) {
    if (r > 0.25f) { c.st = D0_LOSS; return c; }
    c.st = D0_SLOW; return c;
  }
  if (st == PANEL_ST_DEGRADED || st == PANEL_ST_UNKNOWN) {
    if (r > 0.5f) { c.st = D0_SLOW; return c; }
  } else if (lossRate > 0.0f && r < lossRate) {
    c.st = D0_LOSS; return c;
  }
  /* rtt normalised over a 100 ms full scale, jittered +/-20% for texture. */
  float base = env->rtt / 100.0f;
  if (base < 0.05f) base = 0.05f;
  if (base > 1.0f)  base = 1.0f;
  c.rtt = base * (0.8f + panelRngNext(&gWfRng) * 0.4f);
  if (c.rtt > 1.0f) c.rtt = 1.0f;
  return c;
}

void panelScreenD0(const PanelEnv *env, float t, float dt) {
  if (!gWfReady) {
    panelRngSeed(&gWfRng, 4211u);              /* prototype rng(4211) */
    for (int y = 0; y < PANEL_H; y++)
      for (int x = 0; x < PANEL_W; x++) gWf[y][x] = d0Sample(env, y);
    gWfStep = t;
    gWfReady = 1;
  }
  if (t - gWfStep > 0.42f) {                   /* DESIGN-BRIEF: 0.42 s */
    gWfStep = t;
    for (int y = 0; y < PANEL_H; y++) {
      memmove(&gWf[y][0], &gWf[y][1], (PANEL_W - 1) * sizeof(D0Cell));
      gWf[y][PANEL_W - 1] = d0Sample(env, y);
    }
  }
  for (int y = 0; y < PANEL_H; y++) {
    for (int x = 0; x < PANEL_W; x++) {
      const D0Cell *v = &gWf[y][x];
      if (v->st == D0_LOSS)      panelFbSet(x, y, cCrit, 0.85f);
      else if (v->st == D0_SLOW) panelFbSet(x, y, cWarn, 0.5f);
      else                       panelFbSet(x, y, cAzure, 0.06f + v->rtt * 0.26f);
    }
  }
}

/* ---- D1 · Pool bars + ticker -------------------------------------------
 * DESIGN-BRIEF: up to 6 pools on rows y=0..5; columns 0-1 the 2 px status key;
 * from column 3 a proportional stacked bar (down/deg/maint/idle as
 * crit/warn/maint/quiet) whose length is the pool size relative to the largest
 * pool, minimum 6 px; row 6 a faint dotted line every 2 columns with the alert
 * ticker scrolling over it at 13 px/s in ink b=0.48.                         */
void panelScreenD1(const PanelEnv *env, float t, float dt) {
  int big = 1;
  for (int i = 0; i < env->poolCount; i++)
    if (env->pools[i].total > big) big = env->pools[i].total;

  for (int i = 0; i < env->poolCount && i <= 5; i++) {
    /* Worst-first, matching C2: with only 6 of up to 8 rows visible, wire order
     * can push the one pool that is actually down off the bottom of the panel. */
    const PanelPoolView *p = &env->pools[env->poolWorstFirst[i]];
    int y = i;
    /* Severity order is down > degraded > unknown > maint (DEVIATION #8):
     * a pool nobody can reach outranks one that is down for planned work. */
    PanelColor *worst = p->down ? &cCrit : p->degraded ? &cWarn
                      : p->unknown ? &cUnknown : p->maint ? &cMaint : &cQuiet;
    float kb = p->down ? 0.9f : p->degraded ? 0.6f : p->unknown ? 0.4f
             : p->maint ? 0.3f : 0.14f;
    panelFbSet(0, y, *worst, kb);
    panelFbSet(1, y, *worst, kb * 0.5f);

    int len = (int)(50.0f * (float)p->total / (float)big + 0.5f);
    if (len < 6) len = 6;
    /* seg(n) = max(n>0 ? 1 : 0, round(len*n/total)) — a single down host is
     * always worth at least one lit pixel. */
    int tot = p->total ? p->total : 1;
    int nd = (int)((float)len * p->down / tot + 0.5f);
    int ng = (int)((float)len * p->degraded / tot + 0.5f);
    int nu = (int)((float)len * p->unknown / tot + 0.5f);
    int nm = (int)((float)len * p->maint / tot + 0.5f);
    if (p->down && nd < 1) nd = 1;
    if (p->degraded && ng < 1) ng = 1;
    if (p->unknown && nu < 1) nu = 1;
    if (p->maint && nm < 1) nm = 1;
    /* Stacked worst-first from the right edge: down, degraded, unknown, maint,
     * then the idle shimmer. The unknown band was missing entirely before —
     * unreachable hosts silently rendered as idle. */
    for (int k = 0; k < len; k++) {
      int x = 3 + k;
      if (k >= len - nd)                     panelFbSet(x, y, cCrit, 0.9f);
      else if (k >= len - nd - ng)           panelFbSet(x, y, cWarn, 0.58f);
      else if (k >= len - nd - ng - nu)      panelFbSet(x, y, cUnknown, 0.42f);
      else if (k >= len - nd - ng - nu - nm) panelFbSet(x, y, cMaint, 0.36f);
      else panelFbSet(x, y, cQuiet,
                      0.09f + 0.08f * (0.5f + 0.5f * sinf(t * 0.9f
                                       + (float)k * 0.35f + (float)i)));
    }
  }
  for (int x = 0; x < PANEL_W; x += 2) panelFbSet(x, 6, cQuiet, 0.05f);
  panelScroll(t, 6, env->ticker, cInk, 0.48f, 13.0f);
}

/* ---- D2 · Ambient field -------------------------------------------------
 * DESIGN-BRIEF: full-grid procedural noise (3 layered sine terms), granularity
 * 0.6 + log10(max(10,total))*1.5, colour interpolating azure -> warn by
 * warmth = clamp((meanLoad-0.35)/0.5, 0, 1); up to 9 crit spark pixels for
 * down systems; a centred text overlay cycling every 5 s through
 * "{total} SYS" / "{meanLoad%} LOAD" / "{gLabel}/S", drawn at y=3 over a
 * 1 px-dimmed knockout box spanning x = tx-2..tx+tw+1, y = 2..8, dim x0.1.   */
void panelScreenD2(const PanelEnv *env, float t, float dt) {
  float totalF = (float)(env->total < 10 ? 10 : env->total);
  float gran = 0.6f + log10f(totalF) * 1.5f;
  float warmth = (env->meanLoad - 0.35f) / 0.5f;
  if (warmth < 0.0f) warmth = 0.0f;
  if (warmth > 1.0f) warmth = 1.0f;

  for (int y = 0; y < PANEL_H; y++) {
    for (int x = 0; x < PANEL_W; x++) {
      float u = (float)x / (float)PANEL_W * gran;
      float v = (float)y / (float)PANEL_H * gran * 0.4f;
      float n = sinf(u * 2.1f + t * 0.5f) * 0.5f
              + sinf(v * 3.3f - t * 0.31f) * 0.3f
              + sinf((u + v) * 1.7f + t * 0.77f) * 0.4f;
      n = (n + 1.2f) / 2.4f;
      if (n < 0.0f) n = 0.0f;
      float b = 0.03f + powf(n, 3.1f) * 0.6f;
      panelFbSetRgb(x, y,
                    cAzure[0] + (cWarn[0] - cAzure[0]) * warmth * n,
                    cAzure[1] + (cWarn[1] - cAzure[1]) * warmth * n,
                    cAzure[2] + (cWarn[2] - cAzure[2]) * warmth * n,
                    b);
    }
  }

  int sparks = env->down > 9 ? 9 : env->down;
  /* Prototype takes the tier of the first down system it finds. */
  int downTier = 3, sysN = env->snap.systemCount;
  if (sysN > (int)PANEL_MAX_SYSTEMS) sysN = (int)PANEL_MAX_SYSTEMS;
  for (int i = 0; i < sysN; i++)
    if (env->snap.systems[i].state == PANEL_ST_DOWN) {
      downTier = env->snap.systems[i].tier & 3; break;
    }
  static const float tierGlow[4] = { 1.0f, 0.7f, 0.45f, 0.3f };
  for (int i = 0; i < sparks; i++) {
    int x = 4 + ((i * 11 + 3) % (PANEL_W - 8));
    int y = (i * 4 + 2) % PANEL_H;
    float puls = (0.35f + 0.65f * tierGlow[downTier])
                 * (0.7f + 0.3f * sinf(t * 3.0f + (float)i));
    panelFbSet(x, y, cCrit, puls);
    panelFbAdd(x - 1, y, cCrit, puls * 0.25f);
    panelFbAdd(x + 1, y, cCrit, puls * 0.25f);
  }

  int slot = ((int)(t / 5.0f)) % 3;
  char msg[20];
  if (slot == 0)      snprintf(msg, sizeof(msg), "%d SYS", env->total);
  else if (slot == 1) snprintf(msg, sizeof(msg), "%d%% LOAD",
                               (int)(env->meanLoad * 100.0f + 0.5f));
  else                snprintf(msg, sizeof(msg), "%s/S", env->gLabel);
  PanelColor *col = (slot == 1 && warmth > 0.6f) ? &cWarn : &cInk;
  int tw = panelTextW(msg), tx = (PANEL_W - tw) / 2;
  for (int y = 2; y <= 8; y++)
    for (int x = tx - 2; x <= tx + tw + 1; x++) panelFbDim(x, y, 0.1f);
  panelText(tx, 3, msg, *col, 0.6f);
}
