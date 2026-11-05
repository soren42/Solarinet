/*
 * panelHist.c — builds PanelEnv from snapshots plus locally accumulated history.
 */

#include <stdio.h>
#include <string.h>
#include "panelHist.h"

static float clamp01(float v) { return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v); }

/* Push one sample onto a 53-deep ring held oldest-first. Shift-on-push is
 * cheap here: it runs once per snapshot (0.5 Hz), not per frame. */
static void ringPush(float *ring, float v) {
  memmove(ring, ring + 1, (PANEL_HIST_LEN - 1) * sizeof(float));
  ring[PANEL_HIST_LEN - 1] = v;
}

float panelHistAge(const PanelEnv *env, const float *ring, int age) {
  if (age < 0 || age >= env->histFill || age >= PANEL_HIST_LEN)
    return PANEL_HIST_NONE;
  return ring[PANEL_HIST_LEN - 1 - age];
}

void panelRateLabel(uint32_t kbps, char *out, size_t cap) {
  if (kbps == PANEL_RATE_NONE) { snprintf(out, cap, "--"); return; }
  if (kbps < 1000u) { snprintf(out, cap, "%uK", (unsigned)kbps); return; }
  float mbps = (float)kbps / 1000.0f;
  if (mbps < 9.95f)        snprintf(out, cap, "%.1fM", (double)mbps);
  else if (mbps < 999.5f)  snprintf(out, cap, "%dM", (int)(mbps + 0.5f));
  else if (mbps < 9950.0f) snprintf(out, cap, "%.1fG", (double)(mbps / 1000.0f));
  else                     snprintf(out, cap, "%dG", (int)(mbps / 1000.0f + 0.5f));
}

void panelEnvInit(PanelEnv *env) {
  memset(env, 0, sizeof(*env));
  memset(env->probeHist, 0xFF, sizeof(env->probeHist));
}

static float rateSample(uint32_t kbps) {
  return kbps == PANEL_RATE_NONE ? PANEL_HIST_NONE : (float)kbps;
}

/* Purpose: rx+tx of one system's measured rates. Input: sysx record. Output:
 * total kbps, or -1 when neither direction was measured. */
static float sysxRate(const PanelSnapshot *s, int i) {
  float total = -1.0f;
  if (s->sysx[i].rxKbps != PANEL_RATE_NONE) total = (float)s->sysx[i].rxKbps;
  if (s->sysx[i].txKbps != PANEL_RATE_NONE)
    total = (total < 0.0f ? 0.0f : total) + (float)s->sysx[i].txKbps;
  return total;
}

/* Purpose: fold the per-target probe results into their waterfall rows.
 * Input: env, snapshot. Output: none. */
static void applyProbes(PanelEnv *env, const PanelSnapshot *snap) {
  int n = snap->hasProbes ? snap->probeCount : 0;
  if (n > (int)PANEL_MAX_PROBES) n = (int)PANEL_MAX_PROBES;
  for (int p = 0; p < (int)PANEL_MAX_PROBES; p++) {
    uint16_t cell = PANEL_PROBE_EMPTY;
    if (p < n) {
      if (strncmp(env->probeName[p], snap->probes[p].name, 8) != 0) {
        memset(env->probeHist[p], 0xFF, sizeof(env->probeHist[p]));
        snprintf(env->probeName[p], sizeof(env->probeName[p]), "%.8s",
                 snap->probes[p].name);
      }
      cell = snap->probes[p].lossPermille >= 500u ? PANEL_PROBE_LOST
                                                  : snap->probes[p].rttTenthMs;
      if (cell >= PANEL_PROBE_LOST && snap->probes[p].lossPermille < 500u)
        cell = PANEL_PROBE_LOST - 1u;
    } else {
      env->probeName[p][0] = '\0';
    }
    memmove(env->probeHist[p], env->probeHist[p] + 1,
            (PANEL_HIST_LEN - 1) * sizeof(uint16_t));
    env->probeHist[p][PANEL_HIST_LEN - 1] = cell;
  }
}

void panelEnvApply(PanelEnv *env, const PanelSnapshot *snap) {
  env->snap = *snap;
  env->haveData = true;
  env->alarmActive = snap->alarmActive != 0;
  env->dataStale   = snap->dataStale != 0;
  env->score       = snap->score;

  /* stateRoll order is fixed by protocol.h: ok, degraded, down, unknown, maint. */
  env->up      = snap->stateRoll[0];
  env->deg     = snap->stateRoll[1];
  env->down    = snap->stateRoll[2];
  env->unknown = snap->stateRoll[3];
  env->maint   = snap->stateRoll[4];
  env->total   = env->up + env->deg + env->down + env->unknown + env->maint;
  env->meanLoad = clamp01(snap->meanLoadPct / 100.0f);

  /* History. Totals not on the wire are no sample, not a zero. */
  ringPush(env->wanDown, snap->hasTotals ? rateSample(snap->wanDownKbps) : PANEL_HIST_NONE);
  ringPush(env->wanUp,   snap->hasTotals ? rateSample(snap->wanUpKbps)   : PANEL_HIST_NONE);
  ringPush(env->lanDown, snap->hasTotals ? rateSample(snap->lanDownKbps) : PANEL_HIST_NONE);
  ringPush(env->lanUp,   snap->hasTotals ? rateSample(snap->lanUpKbps)   : PANEL_HIST_NONE);
  ringPush(env->cpuHist, env->meanLoad);
  ringPush(env->rttHist, snap->lossPermille >= 1000u ? PANEL_HIST_NONE
                                                     : snap->rttTenthMs / 10.0f);
  if (env->histFill < PANEL_HIST_LEN) env->histFill++;
  applyProbes(env, snap);

  /* One tile per host: the named systems, worst first as sent, then the tail. */
  int sysN = snap->systemCount > PANEL_MAX_SYSTEMS ? (int)PANEL_MAX_SYSTEMS
                                                   : snap->systemCount;
  int tiles = 0;
  for (int i = 0; i < sysN; i++) {
    env->tiles[tiles].state = snap->systems[i].state;
    env->tiles[tiles].load  = clamp01(snap->systems[i].loadPct / 100.0f);
    tiles++;
  }
  for (int i = 0; i < snap->popCount && tiles < PANEL_TILE_MAX; i++) {
    env->tiles[tiles].state = (uint8_t)(snap->pop[i] >> 5);
    env->tiles[tiles].load  = (float)(snap->pop[i] & 0x1Fu) / 31.0f;
    tiles++;
  }
  env->tileCount = tiles;

  /* Busiest measured hosts first, for Top Talkers. */
  env->talkerCount = 0;
  if (snap->hasSysx) {
    for (int i = 0; i < sysN; i++) {
      float rate = sysxRate(snap, i);
      if (rate <= 0.0f) continue;
      int j = env->talkerCount++;
      while (j > 0 && sysxRate(snap, env->talkers[j - 1]) < rate) {
        env->talkers[j] = env->talkers[j - 1];
        j--;
      }
      env->talkers[j] = (uint8_t)i;
    }
  }

  /* Alert text, read by the inlay. */
  env->hasAlert = snap->hasTopAlert != 0;
  if (env->hasAlert) {
    snprintf(env->subject, sizeof(env->subject), "%s", snap->topAlert.subject);
    snprintf(env->detail,  sizeof(env->detail),  "%s", snap->topAlert.detail);
  } else {
    env->subject[0] = '\0';
    env->detail[0]  = '\0';
  }
}
