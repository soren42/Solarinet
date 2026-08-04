/*
 * panelScreensB.c — Theme B · TEXT.
 * DESIGN-BRIEF "Theme B · Text"; ported from the Turn 3 prototype's
 * sB0/sB1/sB2 (Themes.dc.html lines 354-400).
 */

#include <math.h>
#include <stdio.h>
#include "panelScreens.h"

/* ---- B0 · Condition counts ---------------------------------------------
 * DESIGN-BRIEF: BIG UP count watermark at (1,2) in quiet b=0.3, "UP" label at
 * (1,0) in ok via textOver; at x = max(22, 2+bigW(up)+3) the DOWN count and
 * label, then a tier flag word ("TIER 0" crit if any down system is tier 0,
 * else "TIER 3", else "OK"); row y=6 carries DEG then MNT.                  */
void panelScreenB0(const PanelEnv *env, float t, float dt) {
  char up[8], dn[8], dg[8], mn[8];
  snprintf(up, sizeof(up), "%d", env->up);
  snprintf(dn, sizeof(dn), "%d", env->down);
  snprintf(dg, sizeof(dg), "%d", env->deg);
  snprintf(mn, sizeof(mn), "%d", env->maint);

  panelBig(1, 2, up, cQuiet, 0.3f);
  panelTextOver(1, 0, "UP", cOk, 0.5f);

  int x = 2 + panelBigW(up) + 3;
  if (x < 22) x = 22;

  /* HUE TIER (2026-08-04 hardware pass, see panelFont.c panelTextInk): text
   * hierarchy is carried by colour, not brightness — white for a primary
   * readout, azure for a secondary label. cQuiet is a desaturated slate that
   * disappears against the unlit packages' cream reflection at ANY duty, so it
   * is no longer used for small-font text anywhere. Condition colours
   * (ok/warn/crit/maint) are untouched: a zero count reads azure, a real one
   * reads its condition colour, and the hue is what tells them apart. */
  int cx = panelTextOver(x, 0, dn, env->down ? cCrit : cAzure,
                         env->down ? 0.9f : 0.16f);
  cx = panelTextOver(cx + 1, 0, "DOWN", cAzure, 0.3f);

  /* Prototype tests env.systems for a tier-0 down. Pool aggregates are the
   * better source here: CONTRACT §9 computes them server-side over the FULL
   * fleet, whereas the systems array is capped at 64 and could miss the very
   * host this flag exists to announce. */
  int tierZeroDown = 0;
  for (int i = 0; i < env->poolCount; i++)
    if (env->pools[i].tier == 0 && env->pools[i].down > 0) tierZeroDown = 1;
  const char *tierMsg = env->down ? (tierZeroDown ? "TIER 0" : "TIER 3") : "OK";
  panelTextOver(cx + 3, 0, tierMsg, tierZeroDown ? cCrit : cAzure, 0.4f);

  cx = panelTextOver(x, 6, dg, env->deg ? cWarn : cAzure,
                     env->deg ? 0.65f : 0.16f);
  cx = panelTextOver(cx + 1, 6, "DEG", cAzure, 0.3f);
  cx = panelTextOver(cx + 3, 6, mn, env->maint ? cMaint : cAzure,
                     env->maint ? 0.5f : 0.16f);
  panelTextOver(cx + 1, 6, "MNT", cAzure, 0.3f);
}

/* ---- B1 · Throughput ----------------------------------------------------
 * DESIGN-BRIEF: BIG aggregate label at (1,2) quiet b=0.3, "AGGREGATE" at (1,0)
 * azure; from x0 = max(26, bigW+4) a 24-cell utilisation bar on row y=1
 * (b 0.5/0.05) echoed on y=2 (b 0.18/0.03), cell colour azure < 70%,
 * warn 70-85%, crit above; percent + "LINK" at y=4; ticker at y=6, 11 px/s. */
void panelScreenB1(const PanelEnv *env, float t, float dt) {
  panelBig(1, 2, env->gLabel, cQuiet, 0.3f);
  panelTextOver(1, 0, "AGGREGATE", cAzure, 0.5f);

  int x0 = panelBigW(env->gLabel) + 4;
  if (x0 < 26) x0 = 26;

  /* Utilisation is the newest ingress+egress sample against the adaptive peak
   * (DEVIATION documented in panelHist.c); thresholds are the design's. */
  float util = panelHistAt(env, env->thru, PANEL_HIST_LEN - 1)
             + panelHistAt(env, env->egress, PANEL_HIST_LEN - 1);
  if (util > 1.0f) util = 1.0f;

  for (int k = 0; k < 24; k++) {
    float frac = (float)k / 24.0f;
    int on = frac < util;
    PanelColor *col = frac > 0.85f ? &cCrit : frac > 0.7f ? &cWarn : &cAzure;
    panelFbSet(x0 + k, 1, on ? *col : cQuiet, on ? 0.5f : 0.05f);
    panelFbSet(x0 + k, 2, on ? *col : cQuiet, on ? 0.18f : 0.03f);
  }

  char line[24];
  snprintf(line, sizeof(line), "%d%% LINK", (int)(util * 100.0f + 0.5f));
  panelTextOver(x0, 4, line, util > 0.85f ? cWarn : cInk, 0.5f);

  /* Prototype ticker synthesised OUT as 0.6x and PEAK as 1.4x of a single
   * aggregate figure; the wire carries the real split, so IN/OUT are the real
   * directions and PEAK is the adaptive reference the bar is scaled against. */
  char inLabel[12], outLabel[12], ticker[80];
  panelFormatRate(env->rxGbps * 1000000.0f, inLabel, sizeof(inLabel));
  panelFormatRate(env->txGbps * 1000000.0f, outLabel, sizeof(outLabel));
  snprintf(ticker, sizeof(ticker),
           "IN %s/S" PANEL_SEP "OUT %s/S" PANEL_SEP "LINK %d%%" PANEL_TAIL,
           inLabel, outLabel, (int)(util * 100.0f + 0.5f));
  panelScroll(t, 6, ticker, cInk, 0.45f, 11.0f);
}

/* ---- B2 · Load and latency ---------------------------------------------
 * DESIGN-BRIEF: BIG mean-load percent at (1,2) (warn above 75%), "MEAN LOAD"
 * at (1,0) ink b=0.45; at x0 = max(26, bigW+4) row y=0 carries "P95 {rtt}MS"
 * (warn above 35 ms) then loss percent (crit above 1%); row y=6 carries the
 * count of systems over 85% load plus "OVER 85".
 * Both text rows are re-laid-out to fit the 53 px panel — see DEVIATION 13
 * (row 6) and DEVIATION 14 (row 0) below. Colours, thresholds, brightnesses
 * and the BIG numeral are exactly as specified.                             */
void panelScreenB2(const PanelEnv *env, float t, float dt) {
  char pct[8];
  snprintf(pct, sizeof(pct), "%d%%", (int)(env->meanLoad * 100.0f + 0.5f));
  panelBig(1, 2, pct, env->meanLoad > 0.75f ? cWarn : cQuiet,
           env->meanLoad > 0.75f ? 0.22f : 0.3f);
  /* DEVIATION 14 (row y=0 layout). The design's row 0 is over-subscribed: it
   * asks for "MEAN LOAD" (35 px) at x=1 PLUS "P95 {rtt}MS" (31 px) PLUS a loss
   * percent (15 px) starting at x0 = max(26, bigW+4), which is ~84 px of text
   * on a 53 px row. The Turn 3 prototype has the same arrangement. In practice
   * "P95 …" overpainted the tail of "MEAN LOAD" and then ran off the right edge
   * itself, so BOTH strings were rendering truncated.
   *
   * Three things make it fit with nothing clipped and no figure dropped:
   *   - the big numeral's label contracts to "LOAD" (15 px, x=1..15) — with a
   *     percent numeral directly under it, "MEAN" was carrying no information;
   *   - the latency and loss readings alternate on a 5 s slot instead of
   *     sharing the row, the same rotation vocabulary D2 already uses, which
   *     keeps both LABELS intact rather than abbreviating them to symbols;
   *   - the active reading is right-aligned to the panel edge and floored clear
   *     of the "LOAD" label, so it can never overlap or overrun.
   * Both figures are clamped to the widest form that fits (999 ms, 100%). */
  panelTextOver(1, 0, "LOAD", cInk, 0.45f);

  /* 24 bytes, not 16: the compiler cannot see the ms clamp below and warns that
   * "P95 %dMS" could write up to 18 bytes for a full-range int. */
  char reading[24];
  PanelColor *rcol;
  if ((((int)(t / 5.0f)) & 1) == 0) {
    int ms = (int)(env->rtt + 0.5f);
    if (ms > 999) ms = 999;
    snprintf(reading, sizeof(reading), "P95 %dMS", ms);
    rcol = env->rtt > 35.0f ? &cWarn : &cInk;
  } else {
    if (env->loss >= 10.0f)
      snprintf(reading, sizeof(reading), "LOSS %d%%", (int)(env->loss + 0.5f));
    else
      snprintf(reading, sizeof(reading), "LOSS %.1f%%", (double)env->loss);
    rcol = env->loss > 1.0f ? &cCrit : &cInk;
  }
  int rx = PANEL_W - panelTextW(reading);
  int rMinX = panelTextW("LOAD") + 3;
  if (rx < rMinX) rx = rMinX;
  panelTextOver(rx, 0, reading, *rcol, 0.5f);

  /* Counted over the systems on the wire (<= 64, ordered by tier then name).
   * The full-fleet figure would need a per-system aggregate the API does not
   * send; the pool loadPct averages cannot answer "how many are over 85". */
  int hot = 0, n = env->snap.systemCount;
  if (n > (int)PANEL_MAX_SYSTEMS) n = (int)PANEL_MAX_SYSTEMS;
  for (int i = 0; i < n; i++)
    if (env->snap.systems[i].loadPct > 85) hot++;

  /* CONTRACT §9b (two populations): this count is over the panel ROSTER, which
   * includes adopted probe-only assets, while B0's UP/DOWN and D2's "{n} SYS"
   * are the stateRoll AGENT-NODE counts. Printing the denominator makes the
   * different population self-evident, so "3/15" can never be misread as
   * impossible next to a headline of 9.
   *
   * DEVIATION: the design's label is the word "OVER 85". It does not fit and
   * never did — "3 OVER 85" is 35 px starting at x0=26 on a 53 px panel, so the
   * shipped-until-now render was the clipped string "3 OVER". Adding a
   * denominator makes that strictly worse, so the label contracts to ">85",
   * which carries the same threshold in 3 characters. The row is also
   * right-aligned now and floored just clear of the BIG numeral, so it stays at
   * the design's x=26 in the common case and shifts left instead of clipping
   * when the counts reach two digits. */
  char over[20];
  snprintf(over, sizeof(over), "%d/%d>85", hot, n);
  int tx = PANEL_W - panelTextW(over);
  int minX = panelBigW(pct) + 3;
  if (tx < minX) tx = minX;
  panelTextOver(tx, 6, over, hot ? cWarn : cAzure, 0.45f);
}
