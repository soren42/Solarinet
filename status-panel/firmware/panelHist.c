/*
 * panelHist.c — builds PanelEnv from snapshots plus locally accumulated
 * history. This is the firmware's stand-in for the prototype's buildEnv().
 *
 * Where the prototype synthesised a value that the wire cannot carry, the
 * substitution is called out in a comment marked DEVIATION so a reviewer can
 * check it against DESIGN-BRIEF without diffing the JS.
 */

#include <math.h>
#include <stdio.h>
#include <string.h>
#include "panelHist.h"

/* Prototype RANK = {down:3, deg:2, maint:1, ok:0}. UNKNOWN does not exist in
 * the mockup; DESIGN-BRIEF §Colors/Ambiguity #2 says real telemetry will have
 * gaps, so it is ranked between degraded and maint — worse than a planned
 * maintenance window, better than a confirmed degradation. */
static int stateRank(uint8_t st) {
  switch (st) {
    case PANEL_ST_DOWN:     return 4;
    case PANEL_ST_DEGRADED: return 3;
    case PANEL_ST_UNKNOWN:  return 2;
    case PANEL_ST_MAINT:    return 1;
    default:                return 0;
  }
}

/* Adaptive throughput reference.
 * DEVIATION: the prototype normalises against fixed 25 Gbps / 18 Gbps WAN
 * constants (DESIGN-BRIEF A1/B1) because its synthetic fleet was multi-Gbps.
 * Real SolariNet throughput is orders of magnitude smaller, so those constants
 * would flatline every ribbon and gauge at zero. Ribbons and gauges instead
 * normalise against a decaying observed peak, floored at 1 Mbps so a quiet
 * fleet does not paint sensor noise at full scale. Shapes and thresholds
 * (0.7 warn / 0.85 crit) are unchanged. */
#define PANEL_PEAK_FLOOR_KBPS 1000.0f
#define PANEL_PEAK_DECAY      0.997f
static float gPeakKbps = PANEL_PEAK_FLOOR_KBPS;

static float clamp01(float v) { return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v); }

/* Push one sample onto a 53-deep ring held oldest-first. Shift-on-push is
 * cheap here: it runs once per snapshot (0.5 Hz), not per frame. */
static void ringPush(float *ring, float v) {
  memmove(ring, ring + 1, (PANEL_HIST_LEN - 1) * sizeof(float));
  ring[PANEL_HIST_LEN - 1] = v;
}

float panelHistAt(const PanelEnv *env, const float *ring, int i) {
  if (i < 0) i = 0;
  if (i >= PANEL_HIST_LEN) i = PANEL_HIST_LEN - 1;
  int firstReal = PANEL_HIST_LEN - env->histFill;
  if (firstReal < 0) firstReal = 0;
  if (i < firstReal) i = firstReal;
  if (i >= PANEL_HIST_LEN) i = PANEL_HIST_LEN - 1;
  return ring[i];
}

/* Format the headline throughput label.
 * DESIGN-BRIEF §Data requirements: "3.2G" / "14G", no decimal above 10 Gbps.
 * DEVIATION: an M (megabit) tier is added below 1 Gbps for the same reason as
 * the adaptive peak above — the BIG font carries G, M and S, so "340M" renders
 * with the shipped glyph set and no font change. */
void panelFormatRate(float kbps, char *out, size_t cap) {
  float gbps = kbps / 1000000.0f;
  if (gbps >= 10.0f)      snprintf(out, cap, "%dG", (int)(gbps + 0.5f));
  else if (gbps >= 1.0f)  snprintf(out, cap, "%.1fG", (double)gbps);
  else {
    float mbps = kbps / 1000.0f;
    if (mbps >= 10.0f)    snprintf(out, cap, "%dM", (int)(mbps + 0.5f));
    else                  snprintf(out, cap, "%.1fM", (double)mbps);
  }
}

void panelEnvInit(PanelEnv *env) {
  memset(env, 0, sizeof(*env));
  gPeakKbps = PANEL_PEAK_FLOOR_KBPS;
  snprintf(env->gLabel, sizeof(env->gLabel), "0.0M");
  memset(env->latticeOwner, 0xFF, sizeof(env->latticeOwner));
  memset(env->probeRow, 0xFF, sizeof(env->probeRow));
}

/* Order system indices worst-first: state rank desc, then tier asc, then load
 * desc — the prototype's `sorted` comparator (Themes.dc.html line 115). */
static void sortWorstFirst(const PanelSnapshot *s, uint8_t *idx, int n) {
  for (int i = 1; i < n; i++) {
    uint8_t key = idx[i];
    int j = i - 1;
    while (j >= 0) {
      const uint8_t a = idx[j], b = key;
      int ra = stateRank(s->systems[a].state), rb = stateRank(s->systems[b].state);
      int worse = (rb != ra) ? (rb > ra)
                : (s->systems[b].tier != s->systems[a].tier)
                  ? (s->systems[b].tier < s->systems[a].tier)
                  : (s->systems[b].loadPct > s->systems[a].loadPct);
      if (!worse) break;
      idx[j + 1] = idx[j];
      j--;
    }
    idx[j + 1] = key;
  }
}

/* Lattice order: tier asc, then state rank desc — the prototype's A0
 * comparator (Themes.dc.html line 267), which differs from the one above. */
static void sortLatticeOrder(const PanelSnapshot *s, uint8_t *idx, int n) {
  for (int i = 1; i < n; i++) {
    uint8_t key = idx[i];
    int j = i - 1;
    while (j >= 0) {
      const uint8_t a = idx[j], b = key;
      int before = (s->systems[b].tier != s->systems[a].tier)
                   ? (s->systems[b].tier < s->systems[a].tier)
                   : (stateRank(s->systems[b].state) > stateRank(s->systems[a].state));
      if (!before) break;
      idx[j + 1] = idx[j];
      j--;
    }
    idx[j + 1] = key;
  }
}

void panelEnvApply(PanelEnv *env, const PanelSnapshot *snap) {
  env->snap = *snap;
  env->haveData = true;
  env->alarmActive = snap->alarmActive != 0;
  env->dataStale   = snap->dataStale != 0;
  env->score       = snap->score;

  /* stateRoll order is fixed by protocol.h: ok, degraded, down, unknown,
   * maint. The prototype's `up` is the ok count. */
  env->up      = snap->stateRoll[0];
  env->deg     = snap->stateRoll[1];
  env->down    = snap->stateRoll[2];
  env->unknown = snap->stateRoll[3];
  env->maint   = snap->stateRoll[4];
  env->total   = env->up + env->deg + env->down + env->unknown + env->maint;

  env->meanLoad = clamp01(snap->meanLoadPct / 100.0f);
  env->rtt      = snap->rttTenthMs / 10.0f;
  env->loss     = snap->lossPermille / 10.0f;

  /* Throughput and its adaptive reference. */
  float rxK = (float)snap->rxKbps, txK = (float)snap->txKbps;
  float totalK = rxK + txK;
  gPeakKbps *= PANEL_PEAK_DECAY;
  if (totalK > gPeakKbps) gPeakKbps = totalK;
  if (gPeakKbps < PANEL_PEAK_FLOOR_KBPS) gPeakKbps = PANEL_PEAK_FLOOR_KBPS;
  env->rxGbps = rxK / 1000000.0f;
  env->txGbps = txK / 1000000.0f;
  env->gbps   = totalK / 1000000.0f;
  panelFormatRate(totalK, env->gLabel, sizeof(env->gLabel));

  /* History rings — one sample per snapshot (CONTRACT §9: not on the wire). */
  ringPush(env->thru,    clamp01(rxK / gPeakKbps));
  ringPush(env->egress,  clamp01(txK / gPeakKbps));
  ringPush(env->cpuHist, env->meanLoad);
  /* RTT normalised over a 100 ms full-scale, matching the prototype's rttHist
   * band (~0.3 nominal, ~0.75 during the core-down scenario). */
  ringPush(env->rttHist, clamp01(env->rtt / 100.0f));
  if (env->histFill < PANEL_HIST_LEN) env->histFill++;

  /* Pools, in wire order (already ordered by tier per CONTRACT §3). */
  env->poolCount = snap->poolCount > PANEL_MAX_POOLS ? PANEL_MAX_POOLS
                                                     : snap->poolCount;
  for (int i = 0; i < env->poolCount; i++) {
    env->pools[i].name     = env->snap.pools[i].name;
    env->pools[i].tier     = snap->pools[i].tier;
    env->pools[i].ok       = snap->pools[i].ok;
    env->pools[i].degraded = snap->pools[i].degraded;
    env->pools[i].down     = snap->pools[i].down;
    env->pools[i].unknown  = snap->pools[i].unknown;
    env->pools[i].maint    = snap->pools[i].maint;
    env->pools[i].total    = snap->pools[i].total ? snap->pools[i].total : 1;
    env->pools[i].load     = clamp01(snap->pools[i].loadPct / 100.0f);
    env->poolWorstFirst[i] = (uint8_t)i;
  }
  /* Prototype poolLoad ordering (line 114): (down*9 + deg) desc, load desc. */
  for (int i = 1; i < env->poolCount; i++) {
    uint8_t key = env->poolWorstFirst[i];
    int j = i - 1;
    while (j >= 0) {
      const PanelPoolView *a = &env->pools[env->poolWorstFirst[j]];
      const PanelPoolView *b = &env->pools[key];
      int wa = a->down * 9 + a->degraded, wb = b->down * 9 + b->degraded;
      int worse = (wb != wa) ? (wb > wa) : (b->load > a->load);
      if (!worse) break;
      env->poolWorstFirst[j + 1] = env->poolWorstFirst[j];
      j--;
    }
    env->poolWorstFirst[j + 1] = key;
  }

  int sysN = snap->systemCount > PANEL_MAX_SYSTEMS ? PANEL_MAX_SYSTEMS
                                                   : snap->systemCount;

  /* C0 load histogram: 35 buckets, down and unknown systems excluded (they
   * report loadPct 0 with no telemetry behind it), normalised by the tallest
   * bucket — prototype lines 95-98. Computed over the systems on the wire
   * (<= 64); pool aggregates remain the full-fleet view. */
  int hist[PANEL_LOAD_BUCKETS];
  memset(hist, 0, sizeof(hist));
  for (int i = 0; i < sysN; i++) {
    uint8_t st = snap->systems[i].state;
    if (st == PANEL_ST_DOWN || st == PANEL_ST_UNKNOWN) continue;
    int b = (int)((snap->systems[i].loadPct / 100.0f) * PANEL_LOAD_BUCKETS);
    if (b < 0) b = 0;
    if (b >= PANEL_LOAD_BUCKETS) b = PANEL_LOAD_BUCKETS - 1;
    hist[b]++;
  }
  int hMax = 1;
  for (int b = 0; b < PANEL_LOAD_BUCKETS; b++) if (hist[b] > hMax) hMax = hist[b];
  for (int b = 0; b < PANEL_LOAD_BUCKETS; b++)
    env->loadHist[b] = (float)hist[b] / (float)hMax;

  /* A0 lattice: bucket systems evenly across the 583 slots so the picture
   * holds at any fleet size (DESIGN-BRIEF A0 / "scale-free" final decision). */
  uint8_t order[PANEL_MAX_SYSTEMS];
  for (int i = 0; i < sysN; i++) order[i] = (uint8_t)i;
  sortLatticeOrder(snap, order, sysN);
  memset(env->latticeOwner, 0xFF, sizeof(env->latticeOwner));
  int per = (sysN + PANEL_LATTICE_SLOTS - 1) / PANEL_LATTICE_SLOTS;
  if (per < 1) per = 1;
  for (int slot = 0; slot < PANEL_LATTICE_SLOTS; slot++) {
    int a = slot * per;
    if (a >= sysN) break;
    int worst = 0, tier = 3, n = 0;
    float load = 0.0f;
    for (int k = a; k < sysN && k < a + per; k++) {
      const uint8_t s = order[k];
      if (stateRank(snap->systems[s].state) > stateRank((uint8_t)worst)) {
        worst = snap->systems[s].state;
        tier  = snap->systems[s].tier;
      }
      if (snap->systems[s].tier < tier && worst == PANEL_ST_OK)
        tier = snap->systems[s].tier;
      load += snap->systems[s].loadPct / 100.0f;
      n++;
    }
    env->latticeOwner[slot] = 1;
    env->lattice[slot].state = (uint8_t)worst;
    env->lattice[slot].tier  = (uint8_t)(tier > 3 ? 3 : tier);
    env->lattice[slot].load  = n ? load / (float)n : 0.0f;
  }

  /* D0 waterfall rows.
   * DEVIATION: the prototype's 11 rows are 11 named probe targets with
   * synthetic per-row loss (Themes.dc.html sample(), Turn 1/2's GATEWAY,
   * DNS 1, ... WAN list). protocol.h carries no per-target probe results —
   * only fleet-aggregate rttTenthMs/lossPermille — so the 11 rows are the 11
   * worst-first systems instead. Row structure, cadence (0.42 s) and cell
   * colouring are unchanged; only the row identity is re-sourced. */
  uint8_t worstOrder[PANEL_MAX_SYSTEMS];
  for (int i = 0; i < sysN; i++) worstOrder[i] = (uint8_t)i;
  sortWorstFirst(snap, worstOrder, sysN);
  env->probeRows = sysN < PANEL_H ? sysN : PANEL_H;
  memset(env->probeRow, 0xFF, sizeof(env->probeRow));
  for (int i = 0; i < env->probeRows; i++) env->probeRow[i] = worstOrder[i];

  /* Alert text. The inlay and the D1 ticker read these. */
  env->hasAlert = snap->hasTopAlert != 0;
  if (env->hasAlert) {
    snprintf(env->subject, sizeof(env->subject), "%s", snap->topAlert.subject);
    snprintf(env->detail,  sizeof(env->detail),  "%s", snap->topAlert.detail);
  } else {
    snprintf(env->subject, sizeof(env->subject), "ALL NOMINAL");
    snprintf(env->detail,  sizeof(env->detail),  "NO FINDINGS");
  }
  /* Prototype: ticker = subject + " · " + detail + "   ·   ". */
  snprintf(env->ticker, sizeof(env->ticker), "%s" PANEL_SEP "%s" PANEL_TAIL,
           env->subject, env->detail);
}
