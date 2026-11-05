/*
 * panelHist.h — PanelEnv: everything the screens read.
 *
 * Two sources:
 *   1. the last applied PanelSnapshot (protocol.h) — instantaneous state, and
 *   2. history rings this firmware accumulates itself, one sample per snapshot
 *      arrival. History is not on the wire (CONTRACT.md §9). CONTRACT-V2 §7:
 *      it starts empty and only real samples are ever drawn.
 *
 * Score is NEVER recomputed here: CONTRACT §9 makes the server authoritative
 * for both `score` and `alarmActive`, and the firmware only consumes them.
 */

#ifndef SOLARI_PANEL_HIST_H
#define SOLARI_PANEL_HIST_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "protocol.h"
#include "panelHw.h"

/* Ring depth = panel width. At the 2 s snapshot cadence, ~106 s of history. */
#define PANEL_HIST_LEN  PANEL_W
#define PANEL_TILE_MAX  ((int)(PANEL_MAX_SYSTEMS + PANEL_MAX_POP))

/* A ring value meaning "no measurement in this sample". */
#define PANEL_HIST_NONE (-1.0f)
/* Probe cell values: real RTTs are tenths of a ms, below these. */
#define PANEL_PROBE_EMPTY 0xFFFFu
#define PANEL_PROBE_LOST  0xFFFEu

typedef struct {
  uint8_t state;    /* PanelState */
  float   load;     /* 0..1 */
} PanelTile;

typedef struct {
  /* ---- instantaneous, from the snapshot ---- */
  PanelSnapshot snap;     /* last applied snapshot, owned                   */
  bool     haveData;      /* a snapshot has been applied at least once     */
  bool     alarmActive;   /* server-computed; drives the inlay (CONTRACT §9)*/
  bool     dataStale;     /* server says its own upstream data is old       */
  uint16_t score;         /* server-computed; display only                  */
  int      total, up, down, deg, maint, unknown;
  float    meanLoad;      /* 0..1                                           */

  /* ---- derived per snapshot ---- */
  PanelTile tiles[PANEL_TILE_MAX]; /* named systems, then the POP tail      */
  int      tileCount;
  uint8_t  talkers[PANEL_MAX_SYSTEMS]; /* measured systems, busiest first   */
  int      talkerCount;

  /* ---- accumulated history rings, index 0 = oldest ---- */
  float    wanDown[PANEL_HIST_LEN], wanUp[PANEL_HIST_LEN];  /* kbps or NONE */
  float    lanDown[PANEL_HIST_LEN], lanUp[PANEL_HIST_LEN];  /* kbps or NONE */
  float    cpuHist[PANEL_HIST_LEN];  /* mean load 0..1                      */
  float    rttHist[PANEL_HIST_LEN];  /* ms, NONE when nothing answered      */
  int      histFill;                 /* samples accumulated, capped         */
  /* Per-probe waterfall, [target][column], column 0 = oldest. A target's row
   * resets if the name in its position changes. */
  uint16_t probeHist[PANEL_MAX_PROBES][PANEL_HIST_LEN];
  char     probeName[PANEL_MAX_PROBES][9];

  /* ---- alert text ---- */
  char     subject[PANEL_ALERT_SUBJ + 1];
  char     detail[PANEL_ALERT_DETAIL + 1];
  bool     hasAlert;
} PanelEnv;

/* panelEnvInit — zero the environment and put it in the "no data yet" state. */
void panelEnvInit(PanelEnv *env);

/* panelEnvApply — fold a freshly decoded snapshot into env, pushing one new
 * sample onto every history ring and recomputing all per-snapshot derivations.
 * Input:  env (mutated in place), snap (already CRC-checked and de-duplicated).
 * Output: none.                                                             */
void panelEnvApply(PanelEnv *env, const PanelSnapshot *snap);

/* panelRateLabel — small-font rate, at most 4 characters: 740K, 5.8M, 58M, 1.2G.
 * Input: kbps, out buffer + capacity. Output: NUL-terminated label; PANEL_RATE_NONE
 * reads "--", because unmeasured is not zero (CONTRACT-V2 §3).               */
void panelRateLabel(uint32_t kbps, char *out, size_t cap);

/* panelHistAge — read a ring by age: 0 is the newest sample.
 * Input: env, ring, age. Output: the sample, or PANEL_HIST_NONE when that age
 * has not been sampled yet. Never back-fills (CONTRACT-V2 §7).               */
float panelHistAge(const PanelEnv *env, const float *ring, int age);

#endif /* SOLARI_PANEL_HIST_H */
