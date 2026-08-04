/*
 * panelHist.h — PanelEnv: everything the screens read.
 *
 * The prototype's buildEnv() produced one big derived object from synthetic
 * fleet data. Here the same object is produced from two real sources:
 *   1. the last applied PanelSnapshot (protocol.h) — instantaneous state, and
 *   2. history rings this firmware accumulates itself, one sample per
 *      snapshot arrival. CONTRACT.md §9 is explicit that history (ribbons,
 *      histograms, sparklines) is NOT on the wire and is the firmware's job.
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

/* Ring depth = panel width: DESIGN-BRIEF's ribbons are 53-sample (§Data
 * requirements). At the 2 s snapshot cadence that is ~106 s of history. */
#define PANEL_HIST_LEN  PANEL_W
#define PANEL_LOAD_BUCKETS 35     /* DESIGN-BRIEF C0: 35-bucket histogram */
#define PANEL_LATTICE_SLOTS (PANEL_W * PANEL_H)   /* 583, DESIGN-BRIEF A0 */

/* The prototype's F font has no '·' glyph, so its separators render as blank
 * cells. These ASCII equivalents are pixel-identical and keep the firmware
 * free of multi-byte text. " · " -> 3 cells, "   ·   " -> 7 cells. */
#define PANEL_SEP  "   "
#define PANEL_TAIL "       "

typedef struct {
  uint8_t state;    /* PanelState */
  uint8_t tier;
  float   load;     /* 0..1 */
} PanelLatticeCell;

typedef struct {
  const char *name;
  uint8_t tier, ok, degraded, down, unknown, maint, total;
  float   load;     /* 0..1 */
} PanelPoolView;

typedef struct {
  /* ---- instantaneous, from the snapshot ---- */
  PanelSnapshot snap;     /* last applied snapshot, owned (names point here) */
  bool     haveData;      /* a snapshot has been applied at least once     */
  bool     alarmActive;   /* server-computed; drives the inlay (CONTRACT §9)*/
  bool     dataStale;     /* server says its own upstream data is old       */
  uint16_t score;         /* server-computed; display only                  */
  int      total, up, down, deg, maint, unknown;
  float    meanLoad;      /* 0..1                                           */
  float    rxGbps, txGbps, gbps;
  char     gLabel[12];    /* "3.2G" / "14G", DESIGN-BRIEF §Data requirements */
  float    rtt;           /* ms   */
  float    loss;          /* %    */

  /* ---- derived per snapshot ---- */
  float    loadHist[PANEL_LOAD_BUCKETS];     /* normalised 0..1, C0        */
  PanelPoolView pools[PANEL_MAX_POOLS];      /* wire order (by tier)       */
  int      poolCount;
  uint8_t  poolWorstFirst[PANEL_MAX_POOLS];  /* indices, C2/D1 ordering    */
  uint8_t  latticeOwner[PANEL_LATTICE_SLOTS];/* 0xFF = empty slot          */
  PanelLatticeCell lattice[PANEL_LATTICE_SLOTS];
  uint8_t  probeRow[PANEL_H];   /* system index per D0 waterfall row, 0xFF */
  int      probeRows;

  /* ---- accumulated history rings (index 0 = oldest) ---- */
  float    thru[PANEL_HIST_LEN];     /* ingress, normalised 0..1           */
  float    egress[PANEL_HIST_LEN];   /* egress,  normalised 0..1           */
  float    cpuHist[PANEL_HIST_LEN];  /* mean load 0..1                     */
  float    rttHist[PANEL_HIST_LEN];  /* rtt normalised over 100 ms         */
  int      histFill;                 /* samples accumulated, capped        */

  /* ---- alert text ---- */
  char     subject[PANEL_ALERT_SUBJ + 1];
  char     detail[PANEL_ALERT_DETAIL + 1];
  char     ticker[PANEL_ALERT_SUBJ + PANEL_ALERT_DETAIL + 16];
  bool     hasAlert;
} PanelEnv;

/* panelEnvInit — zero the environment and put it in the "no data yet" state. */
void panelEnvInit(PanelEnv *env);

/* panelEnvApply — fold a freshly decoded snapshot into env, pushing one new
 * sample onto every history ring and recomputing all per-snapshot derivations.
 * Input:  env (mutated in place), snap (already CRC-checked and de-duplicated).
 * Output: none.                                                             */
void panelEnvApply(PanelEnv *env, const PanelSnapshot *snap);

/* panelFormatRate — DESIGN-BRIEF §Data requirements headline rate label:
 * "3.2G" / "14G" (no decimal above 10 Gbps), extended with an M tier below
 * 1 Gbps because the BIG font carries G, M and S but no K.
 * Input: kbps, out buffer + capacity. Output: NUL-terminated label.         */
void panelFormatRate(float kbps, char *out, size_t cap);

/* panelHistAt — read a history ring chronologically: i=0 is the oldest of the
 * PANEL_HIST_LEN retained samples, i=PANEL_HIST_LEN-1 the newest. Slots not
 * yet filled read as the oldest real sample so traces do not flicker at boot. */
float panelHistAt(const PanelEnv *env, const float *ring, int i);

#endif /* SOLARI_PANEL_HIST_H */
