/*
 * panelScreensB.c — Theme B · TRAFFIC: real rates, never derived particles.
 *
 * Every rate reaches the eye through panelRateLevel's shared log scale, and the
 * link kinds keep their own hues, so a busy WAN cannot hide a quiet LAN. An
 * unmeasured rate is drawn as a dotted line: it is no data, not zero traffic.
 */

#include <stdio.h>
#include <string.h>
#include <math.h>
#include "panelScreens.h"

static const float *kindColor(uint8_t kind) {
  switch (kind) {
    case PANEL_LK_WAN:   return cInk;
    case PANEL_LK_TRUNK: return cAzure;
    case PANEL_LK_WIFI:  return cMaint;
    default:               return cOk;
  }
}

/* Spacing and speed for a measured rate. Input: kbps. Output: pixels, px/s. */
static void flowShape(uint32_t kbps, float *spacing, float *speed) {
  float level = panelRateLevel((float)kbps);
  *spacing = 14.0f - 11.0f * level;
  *speed   = 2.0f + 22.0f * level;
}

/* One direction of a lane. dir +1 runs right, -1 left. phase is this stream's
 * integrated distance, so a rate change does not jump the dots.              */
static void laneRow(int x0, int x1, int y, uint32_t kbps, float *phase, int dir,
                    const float *hue, float dt) {
  if (kbps == PANEL_RATE_NONE) {
    for (int x = x0; x <= x1; x += 4) panelFbSet(x, y, cUnknown, 0.35f);
    return;
  }
  if (kbps == 0u) {
    for (int x = x0; x <= x1; x++) panelFbSet(x, y, hue, 0.06f);
    return;
  }
  float spacing, speed;
  flowShape(kbps, &spacing, &speed);
  *phase = fmodf(*phase + speed * dt, spacing * 64.0f);
  for (int x = x0; x <= x1; x++) panelFbSet(x, y, hue, 0.08f);
  for (float p = fmodf(*phase, spacing) - spacing; p <= (float)(x1 - x0); p += spacing) {
    int d = (int)floorf(p);
    if (d < 0 || d > x1 - x0) continue;
    int head = dir > 0 ? x0 + d : x1 - d;
    panelFbSet(head, y, hue, 1.0f);
    int tail = head - dir;
    if (tail >= x0 && tail <= x1) panelFbSet(tail, y, hue, 0.4f);
  }
}

/* Short lane label: a WAN link reads WAN or WAN2 rather than a cut-off word. */
static const char *laneLabel(const PanelSnapshot *s, int i) {
  __typeof__(s->links[0]) const *link = &s->links[i];
  if (link->kind == PANEL_LK_WAN)
    return strncmp(link->name, "BACKUP", 6) == 0 ? "WAN2" : "WAN";
  return link->name;
}

/* ---- B0 · Swim Lanes ------------------------------------------------------
 * Two labelled links a page; upper stream away from the internet, lower toward
 * it. Busier = denser and faster on the shared log scale.                    */
void panelScreenB0(const PanelEnv *env, float t, float dt) {
  enum { LANE_X = 21 };
  static float phase[PANEL_MAX_LINKS][2];
  (void)t;
  const PanelSnapshot *s = &env->snap;
  if (!s->hasLinks || s->linkCount == 0) {
    panelTextCentre(PANEL_ROW_ONE, "NO LINK DATA", cAzure, 1.0f);
    return;
  }
  int pages = panelScreenPages(env, 4u);
  int page = panelPageTick(4u, dt, pages);
  for (int slot = 0; slot < 2; slot++) {
    int i = page * 2 + slot;
    int y = slot ? PANEL_ROW_BOT : PANEL_ROW_TOP;
    if (i >= s->linkCount) break;
    __typeof__(s->links[0]) const *link = &s->links[i];
    int bad = link->state == PANEL_ST_DOWN;
    panelTextFit(0, y, laneLabel(s, i), 5, bad ? cCrit : cInk, 1.0f);
    if (bad) {
      for (int x = LANE_X; x < PANEL_W; x++) {
        panelFbSet(x, y + 0, cCrit, 0.6f);
        panelFbSet(x, y + 4, cCrit, 0.6f);
      }
      continue;
    }
    const float *hue = kindColor(link->kind);
    laneRow(LANE_X, PANEL_W - 1, y,     link->downKbps, &phase[i][0], -1, hue, dt);
    laneRow(LANE_X, PANEL_W - 1, y + 1, link->downKbps, &phase[i][0], -1, hue, 0.0f);
    laneRow(LANE_X, PANEL_W - 1, y + 3, link->upKbps,   &phase[i][1],  1, hue, dt);
    laneRow(LANE_X, PANEL_W - 1, y + 4, link->upKbps,   &phase[i][1],  1, hue, 0.0f);
  }
  panelPageBar(page, pages);
}

/* A stacked two-direction sparkline of real samples, newest on the right,
 * normalised to its own window; unsampled columns stay dark.                 */
static void rateSpark(const PanelEnv *env, const float *downRing, const float *upRing,
                      int x0, int x1, int y, const float *hue) {
  float peak = 1.0f;
  int cols = x1 - x0 + 1;
  for (int a = 0; a < cols; a++) {
    float dn = panelHistAge(env, downRing, a), up = panelHistAge(env, upRing, a);
    float total = (dn > 0.0f ? dn : 0.0f) + (up > 0.0f ? up : 0.0f);
    if (total > peak) peak = total;
  }
  for (int a = 0; a < cols; a++) {
    float dn = panelHistAge(env, downRing, a), up = panelHistAge(env, upRing, a);
    if (dn < 0.0f && up < 0.0f) continue;
    if (dn < 0.0f) dn = 0.0f;
    if (up < 0.0f) up = 0.0f;
    int x = x1 - a;
    int hDown = (int)(dn / peak * 5.0f + 0.5f);
    int hUp = (int)((dn + up) / peak * 5.0f + 0.5f) - hDown;
    if (hDown < 1 && dn > 0.0f) hDown = 1;
    if (hUp < 0) hUp = 0;
    for (int r = 0; r < hDown && r < 5; r++) panelFbSet(x, y + 4 - r, hue, 1.0f);
    for (int r = hDown; r < hDown + hUp && r < 5; r++)
      panelFbSet(x, y + 4 - r, cMaint, 1.0f);
  }
}

/* One rate row: 3-letter label, current total, sparkline. */
static void rateRow(const PanelEnv *env, int y, const char *label, uint32_t down,
                    uint32_t up, const float *downRing, const float *upRing,
                    const float *hue) {
  char rate[8];
  uint32_t total = PANEL_RATE_NONE;
  if (down != PANEL_RATE_NONE) total = down;
  if (up != PANEL_RATE_NONE) total = (total == PANEL_RATE_NONE ? 0u : total) + up;
  panelRateLabel(total, rate, sizeof rate);
  int x = panelText(0, y, label, cInk, 1.0f);
  panelText(x, y, rate, hue, 1.0f);
  rateSpark(env, downRing, upRing, 33, PANEL_W - 1, y, hue);
}

/* ---- B1 · WAN and LAN -----------------------------------------------------
 * The internet and the local network, side by side but on their own scales.  */
void panelScreenB1(const PanelEnv *env, float t, float dt) {
  (void)t; (void)dt;
  const PanelSnapshot *s = &env->snap;
  if (!s->hasTotals) {
    panelTextCentre(PANEL_ROW_ONE, "NO RATE DATA", cAzure, 1.0f);
    return;
  }
  rateRow(env, PANEL_ROW_TOP, "WAN ", s->wanDownKbps, s->wanUpKbps,
          env->wanDown, env->wanUp, cAzure);
  rateRow(env, PANEL_ROW_BOT, "LAN ", s->lanDownKbps, s->lanUpKbps,
          env->lanDown, env->lanUp, cOk);
}

/* ---- B2 · Top Talkers -----------------------------------------------------
 * The busiest hosts by measured rate, two a page.                            */
void panelScreenB2(const PanelEnv *env, float t, float dt) {
  (void)t;
  const PanelSnapshot *s = &env->snap;
  if (env->talkerCount == 0) {
    panelTextCentre(PANEL_ROW_ONE, "NO HOST RATES", cAzure, 1.0f);
    return;
  }
  int shown = env->talkerCount < 8 ? env->talkerCount : 8;
  int pages = panelScreenPages(env, 6u);
  int page = panelPageTick(6u, dt, pages);
  for (int slot = 0; slot < 2; slot++) {
    int rank = page * 2 + slot;
    int y = slot ? PANEL_ROW_BOT : PANEL_ROW_TOP;
    if (rank >= shown) break;
    int i = env->talkers[rank];
    char rate[8];
    uint32_t rx = s->sysx[i].rxKbps == PANEL_RATE_NONE ? 0u : s->sysx[i].rxKbps;
    uint32_t tx = s->sysx[i].txKbps == PANEL_RATE_NONE ? 0u : s->sysx[i].txKbps;
    panelRateLabel(rx + tx, rate, sizeof rate);
    panelTextFit(0, y, s->systems[i].name, 8, cInk, 1.0f);
    panelTextRight(PANEL_W - 1, y, rate, cAzure, 1.0f);
  }
  panelPageBar(page, pages);
}

/* ---- B3 · Flow Map --------------------------------------------------------
 * The network as it is cabled: the internet on the left, each device at its
 * real depth, dots moving along each uplink at that link's measured rate.    */
#define NO_PARENT 0xFFu

/* A parent chain that loops has no real root (UniFi can report slide<->pipette),
 * so the offending link is treated as a root. Input: links, count, index.
 * Output: depth from the root, 0 for a root. */
static int linkDepth(const PanelSnapshot *s, int i) {
  int depth = 0, at = i;
  while (s->links[at].parent != NO_PARENT && s->links[at].parent < s->linkCount) {
    at = s->links[at].parent;
    if (++depth > s->linkCount) return 0;
  }
  return depth;
}

static int linkParent(const PanelSnapshot *s, int i) {
  int parent = s->links[i].parent;
  if (parent == NO_PARENT || parent >= s->linkCount) return -1;
  return linkDepth(s, i) == 0 ? -1 : parent;
}

/* Point `d` pixels along the elbow path parent -> child. */
static void pathPoint(int px, int py, int cx, int cy, int d, int *x, int *y) {
  int dy = cy > py ? cy - py : py - cy;
  if (d <= dy) { *x = px; *y = py + (cy > py ? d : -d); }
  else         { *x = px + (d - dy); *y = cy; }
}

void panelScreenB3(const PanelEnv *env, float t, float dt) {
  static float phase[PANEL_MAX_LINKS][2];
  (void)t;
  const PanelSnapshot *s = &env->snap;
  int n = s->hasLinks ? s->linkCount : 0;
  if (n == 0) {
    panelTextCentre(PANEL_ROW_ONE, "NO LINK DATA", cAzure, 1.0f);
    return;
  }
  if (n > (int)PANEL_MAX_LINKS) n = (int)PANEL_MAX_LINKS;

  int depth[PANEL_MAX_LINKS], parent[PANEL_MAX_LINKS];
  int nx[PANEL_MAX_LINKS], ny[PANEL_MAX_LINKS];
  int maxDepth = 0;
  for (int i = 0; i < n; i++) {
    parent[i] = linkParent(s, i);
    depth[i] = parent[i] < 0 ? 0 : linkDepth(s, i);
    if (depth[i] > maxDepth) maxDepth = depth[i];
  }
  /* Rows: walk the tree depth-first so siblings sit together. Each node takes
   * the next leaf row; a parent sits at its first child's row. */
  int order[PANEL_MAX_LINKS], ordered = 0;
  int stack[PANEL_MAX_LINKS * 2], top = 0;
  for (int i = n - 1; i >= 0; i--) if (parent[i] < 0) stack[top++] = i;
  while (top > 0 && ordered < n) {
    int at = stack[--top];
    order[ordered++] = at;
    for (int c = n - 1; c >= 0; c--)
      if (parent[c] == at && top < (int)PANEL_MAX_LINKS * 2) stack[top++] = c;
  }
  int leaves = 0;
  for (int k = 0; k < ordered; k++) {
    int hasChild = 0;
    for (int c = 0; c < n; c++) if (parent[c] == order[k]) hasChild = 1;
    if (!hasChild) leaves++;
  }
  if (leaves < 1) leaves = 1;
  int leaf = 0;
  for (int k = 0; k < ordered; k++) {
    int at = order[k], hasChild = 0;
    for (int c = 0; c < n; c++) if (parent[c] == at) hasChild = 1;
    nx[at] = 5 + depth[at] * 45 / (maxDepth ? maxDepth : 1);
    /* Leaves spread over rows 1..9; 2 px nodes need the spare row. */
    ny[at] = 1 + (2 * leaf + 1) * 9 / (2 * leaves);
    if (!hasChild) leaf++;
  }

  /* Uplinks first, then dots, then nodes on top. */
  for (int k = 0; k < ordered; k++) {
    int i = order[k];
    int px = parent[i] < 0 ? 0 : nx[parent[i]];
    int py = parent[i] < 0 ? ny[i] : ny[parent[i]];
    int len = (ny[i] > py ? ny[i] - py : py - ny[i]) + (nx[i] - px);
    __typeof__(s->links[0]) const *link = &s->links[i];
    const float *hue = link->state == PANEL_ST_DOWN ? cCrit : kindColor(link->kind);
    for (int d = 0; d <= len; d++) {
      int x, y;
      pathPoint(px, py, nx[i], ny[i], d, &x, &y);
      panelFbSet(x, y, hue, link->state == PANEL_ST_DOWN ? 0.6f : 0.22f);
    }
    if (link->state == PANEL_ST_DOWN || len <= 0) continue;
    for (int dir = 0; dir < 2; dir++) {
      uint32_t kbps = dir ? link->upKbps : link->downKbps;
      if (kbps == PANEL_RATE_NONE || kbps == 0u) continue;
      float spacing, speed;
      flowShape(kbps, &spacing, &speed);
      phase[i][dir] = fmodf(phase[i][dir] + speed * dt, spacing * 64.0f);
      /* Long gaps on a short path would show nothing: keep one dot in flight. */
      if (spacing > (float)len) spacing = (float)len + 1.0f;
      for (float p = fmodf(phase[i][dir], spacing); p <= (float)len; p += spacing) {
        int d = (int)p, x, y;
        if (dir) d = len - d;
        pathPoint(px, py, nx[i], ny[i], d, &x, &y);
        panelFbSet(x, y, hue, 1.0f);
      }
    }
  }
  for (int k = 0; k < ordered; k++) {
    int i = order[k];
    const float *hue = panelStateColor(s->links[i].state);
    for (int yy = 0; yy < 2; yy++)
      for (int xx = 0; xx < 2; xx++)
        panelFbSet(nx[i] + xx, ny[i] + yy, hue, 1.0f);
  }
}
