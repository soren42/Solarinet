/*
 * panelScreensA.c — Theme A · ABSTRACT (no glyphs).
 * DESIGN-BRIEF "Theme A · Abstract"; ported from the Turn 3 prototype's
 * sA0/sA1/sA2 (Themes.dc.html lines 264-351).
 */

#include <math.h>
#include <string.h>
#include "panelScreens.h"

/* Prototype rng(seed): s = (s*1664525 + 1013904223) >>> 0; return s / 2^32. */
void panelRngSeed(PanelRng *r, uint32_t seed) { r->s = seed; }
float panelRngNext(PanelRng *r) {
  r->s = r->s * 1664525u + 1013904223u;
  return (float)r->s / 4294967296.0f;
}

static const PanelPoolView kEmptyPool = { "", 3, 0, 0, 0, 0, 0, 1, 0.0f };

const PanelPoolView *panelPoolLane(const PanelEnv *env, int wanted) {
  if (env->poolCount <= 0) return &kEmptyPool;
  if (wanted >= env->poolCount) wanted = env->poolCount - 1;
  if (wanted < 0) wanted = 0;
  return &env->pools[wanted];
}

/* ---- A0 · State lattice -------------------------------------------------
 * DESIGN-BRIEF: fills the entire 53x11 grid, one cell per system bucket.
 *   ok    quiet, (0.1 + load*0.14) * (0.6 + heat*0.5) + small sine flicker
 *   deg   warn,  pulsing at ~2 rad/s
 *   down  crit,  0.3 + 0.68*heat*(...), tier-0 downs pulse at 3 rad/s
 *   maint maint, 0.2 + 0.16*heat plus a 1.4 Hz on/off dash
 * heat = [1, 0.62, 0.4, 0.26][tier] — tier 0 brightest.
 * Slots with no system behind them stay black, which is how the prototype
 * degrades at small fleet sizes too.                                        */
void panelScreenA0(const PanelEnv *env, float t, float dt) {
  static const float heatByTier[4] = { 1.0f, 0.62f, 0.4f, 0.26f };
  for (int i = 0; i < PANEL_LATTICE_SLOTS; i++) {
    if (env->latticeOwner[i] == 0xFF) continue;
    const PanelLatticeCell *s = &env->lattice[i];
    int x = i % PANEL_W, y = i / PANEL_W;
    float heat  = heatByTier[s->tier & 3];
    float phase = fmodf((float)i * 0.41f, 6.283f);
    switch (s->state) {
      case PANEL_ST_OK:
        panelFbSet(x, y, cQuiet,
                   (0.1f + s->load * 0.14f) * (0.6f + heat * 0.5f)
                   + sinf(t * 0.6f + phase) * 0.015f);
        break;
      case PANEL_ST_DEGRADED:
        panelFbSet(x, y, cWarn,
                   (0.3f + 0.5f * heat) * (0.92f + 0.08f * sinf(t * 2.0f + phase)));
        break;
      case PANEL_ST_DOWN:
        panelFbSet(x, y, cCrit,
                   0.3f + 0.68f * heat *
                   (s->tier == 0 ? 0.7f + 0.3f * fabsf(sinf(t * 3.0f)) : 1.0f));
        break;
      case PANEL_ST_UNKNOWN:
        /* DEVIATION: the mockup never emits UNKNOWN. DESIGN-BRIEF Ambiguity #2
         * reserves #7C8AA0 for "no data from this host"; it is rendered with
         * the maint screen's dash rate so it reads as "not reporting" rather
         * than "healthy". */
        panelFbSet(x, y, cUnknown,
                   0.18f + 0.14f * heat
                   + (float)(((int)floorf(t * 1.4f + phase)) & 1) * 0.08f);
        break;
      default: /* PANEL_ST_MAINT */
        panelFbSet(x, y, cMaint,
                   0.2f + 0.16f * heat
                   + (float)(((int)floorf(t * 1.4f + phase)) & 1) * 0.1f);
        break;
    }
  }
}

/* ---- A1 · Flow gates ----------------------------------------------------
 * DESIGN-BRIEF: 4 lanes at y = 1,3,5,7; trunk quiet x=0..33 at b=0.045;
 * internet gate bar at x=34 (azure, 0.4 at y=5 else 0.09-0.14); 90 particles
 * travelling left-to-right, converging to y=5 past the gate, speed scaled by
 * pool state (down 0.1x, deg 0.5x, ok 1x); WAN ramps from x=44..52 on rows
 * y=3 (upload) and y=7 (download); row y=9 is the loss/RTT indicator.       */
#define A1_PARTICLES 90
typedef struct { float x, v; uint8_t lane, out; } A1Particle;
static A1Particle gA1[A1_PARTICLES];
static int gA1Ready = 0;

void panelScreenA1(const PanelEnv *env, float t, float dt) {
  const int gateX = 34, wanX = 44;
  static const int laneY[4] = { 1, 3, 5, 7 };
  /* Prototype lanes are pools[0],[1],[2],[5] (CORE/APPS/DMZ/WKS). Generalised
   * to the first three pools plus pool 5, each clamped to the pools present. */
  static const int laneWant[4] = { 0, 1, 2, 5 };

  if (!gA1Ready) {
    PanelRng r;
    panelRngSeed(&r, 77u);                     /* prototype rng(77) */
    for (int i = 0; i < A1_PARTICLES; i++) {
      gA1[i].x    = panelRngNext(&r) * 30.0f;
      gA1[i].lane = (uint8_t)(i % 4);
      gA1[i].v    = 0.25f + panelRngNext(&r) * 0.5f;
      gA1[i].out  = 0;
    }
    gA1Ready = 1;
  }

  for (int i = 0; i < 4; i++) {
    int y = laneY[i];
    for (int x = 0; x < gateX; x++) panelFbSet(x, y, cQuiet, 0.045f);
    const PanelPoolView *p = panelPoolLane(env, laneWant[i]);
    PanelColor *col = p->down ? &cCrit : p->degraded ? &cWarn : &cAzure;
    float b = p->down ? 0.9f : p->degraded ? 0.55f : 0.2f;
    panelFbSet(0, y, *col, b);
    panelFbSet(1, y, *col, b * 0.4f);
  }
  for (int y = 0; y < PANEL_H; y++)
    panelFbSet(gateX, y, cAzure, y == 5 ? 0.4f : 0.09f + (float)(y % 2) * 0.05f);

  for (int i = 0; i < A1_PARTICLES; i++) {
    A1Particle *pt = &gA1[i];
    const PanelPoolView *p = panelPoolLane(env, laneWant[pt->lane]);
    float speed = pt->v * (0.5f + env->meanLoad)
                  * (p->down ? 0.1f : p->degraded ? 0.5f : 1.0f) * 14.0f;
    pt->x += speed * dt;
    if (!pt->out && pt->x >= (float)gateX) pt->out = 1;
    if (pt->x > (float)(PANEL_W + 2)) { pt->x = 0.0f; pt->out = 0; }
    int y = pt->out ? 5 : laneY[pt->lane];
    PanelColor *col = p->down ? &cCrit : p->degraded ? &cWarn : &cAzure;
    float b = pt->out ? 0.5f : 0.4f;
    int px = (int)(pt->x + 0.5f);
    panelFbAdd(px, y, *col, b);
    panelFbAdd(px - 1, y, *col, b * 0.3f);
  }

  /* WAN ramps. DEVIATION (see panelHist.c): the prototype's fixed gbps/25 and
   * gbps/18 divisors become fractions of the adaptive observed peak, upload
   * from txKbps and download from rxKbps — the wire splits the directions the
   * mockup could only approximate with a 0.6 factor.
   *
   * These read the newest sample of the already-normalised egress/thru rings
   * rather than recomputing anything locally. An earlier revision used each
   * direction's SHARE of the current total (tx/(rx+tx)), which is a ratio, not
   * a magnitude: a 1 Kbps one-way flow painted a full-scale ramp. Both ramps
   * must be fractions of the same adaptive peak that B1 and the ribbons use,
   * or the three screens disagree about what "full" means. */
  float up = panelHistAt(env, env->egress, PANEL_HIST_LEN - 1);
  float dn = panelHistAt(env, env->thru,   PANEL_HIST_LEN - 1);
  for (int x = wanX; x < PANEL_W; x++) {
    float k = (float)(x - wanX) / (float)(PANEL_W - wanX);
    panelFbSet(x, 3, k < up ? cAzure : cQuiet, k < up ? 0.45f : 0.05f);
    panelFbSet(x, 7, k < dn ? cAzure : cQuiet, k < dn ? 0.3f : 0.05f);
    panelFbSet(x, 9,
               env->loss > 1.0f ? cCrit : env->rtt > 30.0f ? cWarn : cOk,
               env->loss > 1.0f ? 0.7f : 0.22f);
  }
}

/* ---- A2 · MQ and SNMP ---------------------------------------------------
 * DESIGN-BRIEF: 4 lanes at y = 1,3,6,8 (rates 0.7/0.45/0.9/0.3, base colours
 * azure/azure/ok/ok); trunk quiet x=4..52; a 3-cell backlog indicator at
 * x=0..2; 5 travelling packets per lane over x=5..52; faint dotted centre line
 * at y=5 every 4th column; two azure markers at (0,10) and (1,10).
 *
 * DEVIATION: the prototype's lane backlogs are scenario constants — there is
 * no MQ or SNMP telemetry anywhere in protocol.h. The four lanes are instead
 * driven by the closest real signals the wire does carry, so the screen still
 * reports something true rather than animating a fiction:
 *   lane 0 (MQ in)     probe loss              lossPermille
 *   lane 1 (MQ out)    host data freshness     dataStale
 *   lane 2 (SNMP poll) share of fleet UNKNOWN  (no telemetry returned)
 *   lane 3 (SNMP trap) share of fleet degraded
 * Geometry, rates, colours and thresholds are unchanged.                    */
void panelScreenA2(const PanelEnv *env, float t, float dt) {
  static const int   laneYs[4]    = { 1, 3, 6, 8 };
  static const float laneRates[4] = { 0.7f, 0.45f, 0.9f, 0.3f };
  float denom = env->total > 0 ? (float)env->total : 1.0f;
  float backlog[4] = {
    env->loss / 100.0f * 2.0f,
    env->dataStale ? 0.8f : 0.05f,
    (float)env->unknown / denom,
    (float)env->deg / denom
  };

  for (int i = 0; i < 4; i++) {
    int y = laneYs[i];
    for (int x = 4; x < PANEL_W; x++) panelFbSet(x, y, cQuiet, 0.04f);
    float bl = backlog[i] > 1.0f ? 1.0f : backlog[i];
    int q = (int)(bl * 3.0f + 0.5f);
    for (int k = 0; k < 3; k++)
      panelFbSet(k, y,
                 k < q ? (bl > 0.5f ? cCrit : cWarn) : cQuiet,
                 k < q ? 0.7f : 0.06f);
    PanelColor *base = (i < 2) ? &cAzure : &cOk;
    for (int j = 0; j < 5; j++) {
      float x = 5.0f + fmodf(t * laneRates[i] * 22.0f + (float)j * 10.0f
                             + (float)i * 4.0f, (float)(PANEL_W - 6));
      PanelColor *col = bl > 0.5f ? &cWarn : base;
      int px = (int)(x + 0.5f);
      panelFbAdd(px, y, *col, 0.45f);
      panelFbAdd(px - 1, y, *col, 0.15f);
    }
  }
  for (int x = 0; x < PANEL_W; x++)
    panelFbSet(x, 5, cQuiet, (x % 4) == 0 ? 0.07f : 0.02f);
  panelFbSet(0, 10, cAzure, 0.1f);
  panelFbSet(1, 10, cAzure, 0.1f);
}
