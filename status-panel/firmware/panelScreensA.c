/*
 * panelScreensA.c — Theme A · FLEET: what is up, at any fleet size.
 */

#include <stdio.h>
#include "panelScreens.h"

/* Best-fit grid for n tiles on the 53x11 canvas: the largest tile area wins,
 * gutters break ties. A 1 px gutter needs at least 1 px of tile in that axis.
 * Input: tile count. Output: columns, rows, gutters. */
static void tileGrid(int n, int *cols, int *rows, int *gx, int *gy) {
  int bestArea = -1, bestGut = -1;
  *cols = 1; *rows = 1; *gx = 0; *gy = 0;
  for (int r = 1; r <= PANEL_H; r++) {
    int c = (n + r - 1) / r;
    if (c < 1) c = 1;
    if (c > PANEL_W) continue;
    for (int g = 0; g < 4; g++) {
      int ux = g & 1, uy = g >> 1;
      int tw = (PANEL_W - (c - 1) * ux) / c;
      int th = (PANEL_H - (r - 1) * uy) / r;
      if (tw < 1 || th < 1) continue;
      int area = tw * th, gut = ux + uy;
      if (area > bestArea || (area == bestArea && gut > bestGut)) {
        bestArea = area; bestGut = gut;
        *cols = c; *rows = r; *gx = ux; *gy = uy;
      }
    }
  }
}

/* ---- A0 · Host Tiles ------------------------------------------------------
 * The panel is always full. Every host owns a tile: hue is state, fill is load.
 * Nine hosts are tall bars; two hundred are a two-pixel mosaic. Worst first, so
 * trouble reads from the left.                                               */
void panelScreenA0(const PanelEnv *env, float t, float dt) {
  (void)t; (void)dt;
  int n = env->tileCount, cols, rows, gx, gy;
  if (n <= 0) return;
  tileGrid(n, &cols, &rows, &gx, &gy);
  for (int i = 0; i < n; i++) {
    const PanelTile *tile = &env->tiles[i];
    int col = i % cols, row = i / cols;
    int x0 = col * (PANEL_W + gx) / cols;
    int x1 = (col + 1) * (PANEL_W + gx) / cols - gx;
    int y0 = row * (PANEL_H + gy) / rows;
    int y1 = (row + 1) * (PANEL_H + gy) / rows - gy;
    int th = y1 - y0;
    const float *hue = panelStateColor(tile->state);
    /* A host that is down or unmeasured has no load to show: flat tile. */
    int flat = tile->state == PANEL_ST_DOWN || tile->state == PANEL_ST_UNKNOWN ||
               tile->state == PANEL_ST_MAINT;
    if (flat) {
      float b = tile->state == PANEL_ST_DOWN ? 1.0f : 0.35f;
      for (int y = y0; y < y1; y++)
        for (int x = x0; x < x1; x++) panelFbSet(x, y, hue, b);
    } else if (th >= 3) {
      int lit = (int)(tile->load * (float)th + 0.5f);
      if (lit < 1) lit = 1;
      for (int y = y0; y < y1; y++)
        for (int x = x0; x < x1; x++)
          panelFbSet(x, y, hue, y >= y1 - lit ? 1.0f : 0.28f);
    } else {
      float b = 0.2f + 0.8f * tile->load;
      for (int y = y0; y < y1; y++)
        for (int x = x0; x < x1; x++) panelFbSet(x, y, hue, b);
    }
  }
}

/* One roster row: 2x5 state block, name, load (or the state word). */
static void rosterRow(int y, const PanelSnapshot *s, int i) {
  __typeof__(s->systems[0]) const *sys = &s->systems[i];
  const float *hue = panelStateColor(sys->state);
  char right[6];
  for (int r = 0; r < 5; r++) {
    panelFbSet(0, y + r, hue, 1.0f);
    panelFbSet(1, y + r, hue, 1.0f);
  }
  panelTextFit(3, y, sys->name, 8, cInk, 1.0f);
  switch (sys->state) {
    case PANEL_ST_DOWN:    snprintf(right, sizeof right, "DOWN"); break;
    case PANEL_ST_MAINT:   snprintf(right, sizeof right, "MNT"); break;
    case PANEL_ST_UNKNOWN: snprintf(right, sizeof right, "?"); break;
    default:               snprintf(right, sizeof right, "%u%%", sys->loadPct); break;
  }
  panelTextRight(PANEL_W - 1, y, right,
                 sys->state == PANEL_ST_OK ? cAzure : hue, 1.0f);
}

/* ---- A1 · Roster ----------------------------------------------------------
 * Hosts by name, worst first, two a page.                                    */
void panelScreenA1(const PanelEnv *env, float t, float dt) {
  (void)t;
  const PanelSnapshot *s = &env->snap;
  int pages = panelScreenPages(env, 1u);
  int page = panelPageTick(1u, dt, pages);
  if (page * 2 < s->systemCount) rosterRow(PANEL_ROW_TOP, s, page * 2);
  if (page * 2 + 1 < s->systemCount) rosterRow(PANEL_ROW_BOT, s, page * 2 + 1);
  panelPageBar(page, pages);
}

/* One pool row: name, then one block per host in state hue with a thin load bar
 * beneath. Past 20 hosts the blocks become proportional runs. */
static void poolRow(int y, const PanelSnapshot *s, int i) {
  __typeof__(s->pools[0]) const *p = &s->pools[i];
  enum { BAR_X = 33, BAR_W = 20 };
  int total = p->total ? p->total : 1;
  uint8_t states[BAR_W];
  const uint8_t order[5] = { PANEL_ST_DOWN, PANEL_ST_DEGRADED, PANEL_ST_UNKNOWN,
                             PANEL_ST_MAINT, PANEL_ST_OK };
  const int counts[5] = { p->down, p->degraded, p->unknown, p->maint, p->ok };
  int gap = total <= 10 ? 1 : 0;
  int cells = total <= BAR_W ? total : BAR_W;

  panelTextFit(0, y, p->name, 8, cInk, 1.0f);
  /* Cell k shows the state of the host at its proportional position, worst
   * first, and a state with any hosts always shows at least one cell. */
  int run = 0, kind = 0, used = 0;
  for (int k = 0; k < cells; k++) {
    while (kind < 4 && run >= counts[kind]) { kind++; run = 0; }
    states[k] = order[kind];
    used = (k + 1) * total / cells - k * total / cells;
    run += used;
  }
  for (int k = 0; k < cells; k++) {
    int x0 = BAR_X + k * (BAR_W + gap) / cells;
    int x1 = BAR_X + (k + 1) * (BAR_W + gap) / cells - gap;
    const float *hue = panelStateColor(states[k]);
    for (int r = 0; r < 3; r++)
      for (int x = x0; x < x1; x++) panelFbSet(x, y + r, hue, 1.0f);
  }
  int lit = (p->loadPct * BAR_W + 50) / 100;
  if (lit < 1 && p->loadPct > 0u) lit = 1;
  for (int x = 0; x < BAR_W; x++)
    panelFbSet(BAR_X + x, y + 4, cAzure, x < lit ? 1.0f : 0.1f);
}

/* ---- A2 · Pools -----------------------------------------------------------
 * Each pool's hosts and load, two a page.                                    */
void panelScreenA2(const PanelEnv *env, float t, float dt) {
  (void)t;
  const PanelSnapshot *s = &env->snap;
  int pages = panelScreenPages(env, 2u);
  int page = panelPageTick(2u, dt, pages);
  if (page * 2 < s->poolCount) poolRow(PANEL_ROW_TOP, s, page * 2);
  if (page * 2 + 1 < s->poolCount) poolRow(PANEL_ROW_BOT, s, page * 2 + 1);
  panelPageBar(page, pages);
}

/* ---- A3 · Head Count ------------------------------------------------------
 * The one number you read from across the room, and the worst thing behind it.
 * BIG and the small text share rows only in disjoint columns (CONTRACT-V2 §2). */
void panelScreenA3(const PanelEnv *env, float t, float dt) {
  (void)t; (void)dt;
  char big[8], line[16];
  const float *hue = cOk;
  snprintf(big, sizeof big, "%d", env->up);
  if (env->down)         { hue = cCrit;    snprintf(line, sizeof line, "%d DOWN", env->down); }
  else if (env->deg)     { hue = cWarn;    snprintf(line, sizeof line, "%d DEGRADED", env->deg); }
  else if (env->unknown) { hue = cUnknown; snprintf(line, sizeof line, "%d UNKNOWN", env->unknown); }
  else if (env->maint)   { hue = cMaint;   snprintf(line, sizeof line, "%d MAINT", env->maint); }
  else                   {                 snprintf(line, sizeof line, "ALL OK"); }
  int x = panelBig(0, 2, big, hue, 1.0f) + 1;
  int chars = (PANEL_W - x + 1) / 4;
  snprintf(big, sizeof big, "OF %d", env->total);
  panelTextFit(x, PANEL_ROW_TOP, big, chars, cInk, 1.0f);
  panelTextFit(x, PANEL_ROW_BOT, line, chars, hue, 1.0f);
}
