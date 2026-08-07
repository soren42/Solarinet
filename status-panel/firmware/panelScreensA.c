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
 * CONTRACT-AW.md §4 / Amendment A-1: AP 2..16, hub 20..34, switch 38..44,
 * router 46..48, internet x=51 and wan backup x=52. Gates communicate state
 * by hue and a down-state discontinuity; cQuiet is only connective texture,
 * never the sole state distinction (firmware/README.md "Text legibility"). */
#define A1_PARTICLES 90
typedef struct { float progress, v; uint8_t source; } A1Particle;
typedef struct { int gear, x, y0, height; } A1Gate;
static A1Particle gA1[A1_PARTICLES];
static int gA1Ready = 0;

/* Purpose: choose a gate colour from CONTRACT-AW's gear state values.
 * Input: gear state. Output: theme accent, warning, or critical colour. */
static PanelColor *a1GateColor(uint8_t state) {
  /* Gear state is the §9 fixture's own compact status: 0 down, 1 up,
   * 2 degraded; it is intentionally not the PanelState wire enum. */
  return state == 0u ? &cCrit : state == 2u ? &cWarn : &cAzure;
}

/* Purpose: paint one vertical flow gate with a visibly broken down state.
 * Input: x/y extent/state. Output: gate pixels on the framebuffer. */
static void a1Gate(int x, int y0, int height, uint8_t state, float brightness) {
  PanelColor *color = a1GateColor(state);
  int y;
  for (y = y0; y < y0 + height; ++y) {
    if (state == 0u && ((y - y0) & 1) != 0) continue;
    panelFbSet(x, y, *color, brightness);
  }
}

/* Purpose: spread gate positions over an inclusive horizontal band.
 * Input: band/count/ordinal. Output: one deterministic integer x coordinate. */
static int a1BandX(int low, int high, int count, int ordinal) {
  if (count <= 1) return (low + high) / 2;
  return low + (ordinal * (high - low) + (count - 1) / 2) / (count - 1);
}

/* Purpose: paint an A1 traffic leg; its trunk reflects the source RX level.
 * Input: source/destination gates and source receive level. Output: none. */
static void a1Leg(const A1Gate *source, const A1Gate *destination,
                  uint8_t rxLevel) {
  int x;
  int fromY = source->y0 + source->height / 2;
  int toY = destination->y0 + destination->height / 2;
  int span = destination->x - source->x;
  float brightness = 0.04f + (float)rxLevel * 0.06f;
  if (span <= 0) return;
  for (x = source->x + 1; x < destination->x; ++x) {
    int y = fromY + ((toY - fromY) * (x - source->x) + span / 2) / span;
    panelFbSet(x, y, cQuiet, brightness);
  }
}

void panelScreenA1(const PanelEnv *env, float t, float dt) {
  A1Gate aps[PANEL_MAX_GEAR], hubs[PANEL_MAX_GEAR], switches[PANEL_MAX_GEAR];
  A1Gate router, internet = { -1, 51, 0, 11 }, wan = { -1, 52, 3, 5 };
  int apCount = 0, hubCount = 0, switchCount = 0, haveRouter = 0, haveWan = 0;
  int i;

  /* A same-version v1 snapshot may omit the additive gear trailer. */
  if (env->snap.gearCount == 0u) { panelScreenNoData(t); return; }

  for (i = 0; i < (int)env->snap.gearCount; ++i) {
    switch (env->snap.gear[i].role) {
      case 0u: if (!haveRouter) { router.gear = i; router.x = 47; router.y0 = 0; router.height = 11; haveRouter = 1; } break;
      case 1u: if (switchCount < (int)PANEL_MAX_GEAR) switches[switchCount++].gear = i; break;
      case 2u: if (hubCount < (int)PANEL_MAX_GEAR) hubs[hubCount++].gear = i; break;
      case 3u: if (apCount < (int)PANEL_MAX_GEAR) aps[apCount++].gear = i; break;
      case 4u: if (!haveWan) { wan.gear = i; haveWan = 1; } break;
      default: break;
    }
  }

  if (!gA1Ready) {
    PanelRng r;
    panelRngSeed(&r, 77u);                     /* prototype rng(77) */
    for (int i = 0; i < A1_PARTICLES; i++) {
      gA1[i].progress = panelRngNext(&r);
      gA1[i].source = (uint8_t)i;
      gA1[i].v    = 0.25f + panelRngNext(&r) * 0.5f;
    }
    gA1Ready = 1;
  }

  /* CONTRACT-AW §9 A-1: exact AP -> hub -> switch -> router geometry. */
  for (i = 0; i < apCount; ++i) {
    aps[i].x = a1BandX(2, 16, apCount, i);
    aps[i].y0 = apCount == 1 ? 4 : (i * (PANEL_H - 3) + (apCount - 1) / 2) / (apCount - 1);
    aps[i].height = 3;
    a1Gate(aps[i].x, aps[i].y0, aps[i].height,
           env->snap.gear[aps[i].gear].state, 0.75f);
  }
  for (i = 0; i < hubCount; ++i) {
    hubs[i].x = a1BandX(20, 34, hubCount, i);
    hubs[i].height = (i & 1) == 0 ? 6 : 5;
    hubs[i].y0 = (i & 1) == 0 ? 0 : PANEL_H - hubs[i].height;
    a1Gate(hubs[i].x, hubs[i].y0, hubs[i].height,
           env->snap.gear[hubs[i].gear].state, 0.75f);
  }
  for (i = 0; i < switchCount; ++i) {
    switches[i].x = a1BandX(38, 44, switchCount, i);
    switches[i].y0 = 1; switches[i].height = 8;
    a1Gate(switches[i].x, 1, 8, env->snap.gear[switches[i].gear].state, 0.75f);
  }
  if (haveRouter) a1Gate(router.x, router.y0, router.height,
                          env->snap.gear[router.gear].state, 0.80f);
  /* CONTRACT-AW §9 A-3: the internet column is only reachable THROUGH the
   * router, so it inherits the router's state treatment — never a healthy
   * marker over a dead gateway. No router in the payload -> fail dark. */
  a1Gate(internet.x, internet.y0, internet.height,
         haveRouter ? env->snap.gear[router.gear].state : 0u, 0.60f);
  if (haveWan) a1Gate(wan.x, wan.y0, wan.height,
                       env->snap.gear[wan.gear].state, 0.60f);

  for (i = 0; i < apCount && hubCount > 0; ++i)
    a1Leg(&aps[i], &hubs[i % hubCount], env->snap.gear[aps[i].gear].rxLevel);
  for (i = 0; i < hubCount && switchCount > 0; ++i)
    a1Leg(&hubs[i], &switches[i % switchCount], env->snap.gear[hubs[i].gear].rxLevel);
  for (i = 0; i < switchCount && haveRouter; ++i)
    a1Leg(&switches[i], &router, env->snap.gear[switches[i].gear].rxLevel);
  if (haveRouter) a1Leg(&router, &internet, env->snap.gear[router.gear].rxLevel);

  for (int i = 0; i < A1_PARTICLES; i++) {
    A1Particle *pt = &gA1[i];
    int sourceCount = apCount + hubCount + switchCount + (haveRouter ? 1 : 0);
    const A1Gate *source, *destination;
    int ordinal, gearIndex, px, y;
    if (sourceCount == 0) break;
    ordinal = (int)(pt->source % (uint8_t)sourceCount);
    /* Review M3: validate the band BEFORE subtracting — the old chain let
     * `ordinal` go negative when a band was empty (hubs aging out of the
     * 60 s freshness window is routine) and indexed hubs[-2]: ASAN-proven
     * stack overflow, a bus fault on the RP2350. Empty next band drops the
     * particle instead. */
    if (ordinal < apCount) {
      if (hubCount == 0) continue;
      source = &aps[ordinal]; destination = &hubs[ordinal % hubCount];
    } else if ((ordinal -= apCount) < hubCount) {
      if (switchCount == 0) continue;
      source = &hubs[ordinal]; destination = &switches[ordinal % switchCount];
    } else if ((ordinal -= hubCount) < switchCount) {
      if (!haveRouter) continue;
      source = &switches[ordinal]; destination = &router;
    } else if ((ordinal -= switchCount) == 0 && haveRouter) {
      source = &router; destination = &internet;
    } else {
      continue;
    }
    gearIndex = source->gear;
    uint8_t level = env->snap.gear[gearIndex].txLevel;
    float speed = pt->v * (float)level * 0.18f;
    pt->progress += speed * dt;
    if (pt->progress >= 1.0f) pt->progress = 0.0f;
    if (level == 0u) continue;
    px = source->x + (int)((destination->x - source->x) * pt->progress + 0.5f);
    y = source->y0 + source->height / 2 +
        (int)(((destination->y0 + destination->height / 2 -
               (source->y0 + source->height / 2)) * pt->progress) + 0.5f);
    PanelColor *col = a1GateColor(env->snap.gear[gearIndex].state);
    float b = 0.16f + (float)level * 0.09f;
    panelFbAdd(px, y, *col, b);
    panelFbAdd(px - 1, y, *col, b * 0.3f);
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
