/*
 * panelInlay.c — the universal alert inlay and the two link/data treatments.
 */

#include <math.h>
#include <stdio.h>
#include <string.h>
#include "panelScreens.h"

/* ---- Universal alert inlay ----------------------------------------------
 * DESIGN-BRIEF "Universal alert inlay (all 4 themes, function inlay())",
 * restated verbatim in FINAL DECISIONS. Overlays whatever screen is active:
 *   1. dimAll(0.1)                        — running screen drops to 10%
 *   2. both edge columns x=0 and x=52, all 11 rows, crit, pulsing
 *      0.55 + 0.45*|sin(t*2.2)|
 *   3. subject truncated to 12 chars, centred on row y=1, crit, b=0.95
 *   4. detail scrolling on row y=6, ink, b=0.55, 11 px/s
 * The tone (item 5) and acknowledge (item 6) are the alarm state machine in
 * main.c, not part of the paint.                                            */
void panelInlay(const PanelEnv *env, float t) {
  panelFbDimAll(0.1f);

  float pulse = 0.55f + 0.45f * fabsf(sinf(t * 2.2f));
  for (int y = 0; y < PANEL_H; y++) {
    panelFbSet(0, y, cCrit, pulse);
    panelFbSet(PANEL_W - 1, y, cCrit, pulse);
  }

  /* DESIGN-BRIEF: subject truncated to 12 chars. The truncation is the spec,
   * so copy exactly rather than let snprintf warn about a clipped %s. */
  char subj[13];
  size_t sn = strlen(env->subject);
  if (sn > 12) sn = 12;
  memcpy(subj, env->subject, sn);
  subj[sn] = '\0';
  int x = (PANEL_W - panelTextW(subj)) / 2;
  if (x < 2) x = 2;
  panelText(x, 1, subj, cCrit, 0.95f);

  char detail[PANEL_ALERT_DETAIL + 8];
  snprintf(detail, sizeof(detail), "%s" PANEL_TAIL, env->detail);
  panelScroll(t, 6, detail, cInk, 0.55f, 11.0f);
}

/* ---- LINK LOST ----------------------------------------------------------
 * CONTRACT §4: more than 15 s without a valid frame. DESIGN-BRIEF specifies no
 * screen for this, so the treatment is deliberately minimal and deliberately
 * NOT the alarm: warn amber rather than crit red, no edge rails, no tone —
 * the alarm's red-rail vocabulary must stay reserved for real fleet faults,
 * and a dead cable is a panel fault, not a fleet fault. A single pixel sweeps
 * row 9 so a frozen render is distinguishable from a live one at a glance.   */
void panelScreenLinkLost(float t) {
  panelFbDimAll(0.15f);
  const char *msg = "LINK LOST";
  int tw = panelTextW(msg), tx = (PANEL_W - tw) / 2;
  for (int y = 2; y <= 8; y++)
    for (int x = tx - 2; x <= tx + tw + 1; x++) panelFbDim(x, y, 0.1f);
  panelText(tx, 3, msg, cWarn, 0.55f + 0.35f * fabsf(sinf(t * 1.6f)));
  int sweep = ((int)(t * 12.0f)) % PANEL_W;
  panelFbSet(sweep, 9, cQuiet, 0.35f);
}

/* ---- NO DATA ------------------------------------------------------------
 * CONTRACT §9: a zero-node fleet is valid data, not an error. Also used before
 * the first snapshot arrives, so the panel never shows a blank board at boot. */
void panelScreenNoData(float t) {
  const char *msg = "NO DATA";
  int tw = panelTextW(msg), tx = (PANEL_W - tw) / 2;
  panelText(tx, 3, msg, cQuiet, 0.28f + 0.14f * (0.5f + 0.5f * sinf(t * 0.8f)));
  for (int x = 0; x < PANEL_W; x += 4) panelFbSet(x, 9, cQuiet, 0.05f);
}
