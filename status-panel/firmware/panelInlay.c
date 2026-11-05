/*
 * panelInlay.c — the universal alert inlay and the two link/data treatments.
 */

#include <math.h>
#include <stdio.h>
#include <string.h>
#include "panelScreens.h"

/* ---- Universal alert inlay ----------------------------------------------
 * Overlays whatever screen is active, CONTRACT-V2 §2 and §6:
 *   1. the running screen drops to 10%
 *   2. both edge columns x=0 and x=52, crit, pulsing
 *   3. the host, in crit, on the top row
 *   4. what is wrong, in ink, on the bottom row. It stands still to be read.
 * The tone and acknowledge are the alarm state machine in main.c, not the paint. */
static void inlayBody(const char *subject, const char *detail, float t) {
  panelFbDimAll(0.1f);

  float pulse = 0.55f + 0.45f * fabsf(sinf(t * 2.2f));
  for (int y = 0; y < PANEL_H; y++) {
    panelFbSet(0, y, cCrit, pulse);
    panelFbSet(PANEL_W - 1, y, cCrit, pulse);
  }
  /* Knock out what is left of the screen behind the words. */
  for (int y = 0; y < PANEL_H; y++)
    for (int x = 1; x < PANEL_W - 1; x++) panelFbDim(x, y, 0.2f);

  /* 12 chars clears the rail and the beacon; the second row may use all 13. */
  char subj[13];
  size_t sn = strlen(subject);
  if (sn > 12u) sn = 12u;
  memcpy(subj, subject, sn);
  subj[sn] = '\0';
  panelTextCentre(PANEL_ROW_TOP, subj, cCrit, 1.0f);
  panelTextCentre(PANEL_ROW_BOT, detail, cInk, 1.0f);
}

void panelInlay(const PanelEnv *env, float t) {
  inlayBody(env->subject, env->detail, t);
}

/* Power-loss inlay — same vocabulary as a server alert, fixed text (D16).
 * Only reached when NO live server topAlert exists; main.c arbitrates. */
void panelInlayPower(float t) {
  inlayBody("POWER LOSS", "ON BATTERY", t);
}

/* ---- On-battery glyph ---------------------------------------------------
 * DESIGN-BRIEF-P4 §2: 4x5 battery outline, cols 49-52 x rows 6-10, nub at
 * cols 50-51 row 6, interior dark (no charge telemetry exists — state is
 * carried by hue, not fill). Steady warn amber at 0.55: blinking belongs to
 * the alarm beacon's vocabulary, and steady-amber reads as "condition
 * present, noise handled". A 1 px knockout halo (the panelTextOver
 * watermark technique) lets ticker text pass behind it legibly.           */
void panelPowerGlyph(void) {
  for (int y = 5; y <= 10; y++)
    for (int x = 48; x <= 52; x++) panelFbDim(x, y, 0.15f);
  panelFbSet(50, 6, cWarn, 0.55f);   /* nub */
  panelFbSet(51, 6, cWarn, 0.55f);
  for (int x = 49; x <= 52; x++) {   /* body top + bottom */
    panelFbSet(x, 7, cWarn, 0.55f);
    panelFbSet(x, 10, cWarn, 0.55f);
  }
  for (int y = 8; y <= 9; y++) {     /* body sides; interior stays dark */
    panelFbSet(49, y, cWarn, 0.55f);
    panelFbSet(52, y, cWarn, 0.55f);
  }
}

/* ---- Unacknowledged-episode beacon --------------------------------------
 * OPERATOR AMENDMENT 2026-08-04 (supersedes nothing in the DESIGN-BRIEF; the
 * brief has no beacon). Four pixels in the top-right corner — x = 51,52 by
 * y = 0,1 — flash crit red at ~1 Hz from the rising edge of an episode until
 * that episode is acknowledged. It is deliberately NOT tied to the tone: after
 * the 5-minute auto-silence the tone stops and the beacon keeps flashing, which
 * is the whole point of the overnight case.
 *
 * Painted LAST, after the active screen and after the inlay, so it overrides
 * both — the inlay's own right-hand rail runs through x=52 and would otherwise
 * mask it. Also painted in the sleep branch, where the framebuffer is otherwise
 * blank, so a dark panel still carries the fault.
 *
 * Square 1 Hz, 50% duty, at full framebuffer brightness (b=1.0). The remaining
 * scale is the LED driver's global brightness, which floors at 0.25
 * (panelHw.cpp:70) — so the beacon cannot be dimmed out, only dimmed down, and
 * at any setting it is the brightest thing on a sleeping panel.
 * Input:  t, the animation clock in seconds. Output: none (paints).         */
void panelBeacon(float t) {
  if (fmodf(t, 1.0f) >= 0.5f) return;
  for (int y = 0; y <= 1; y++)
    for (int x = PANEL_W - 2; x <= PANEL_W - 1; x++) panelFbSet(x, y, cCrit, 1.0f);
}

/* ---- LINK LOST ----------------------------------------------------------
 * CONTRACT §4: more than 15 s without a valid frame. Deliberately NOT the alarm:
 * warn amber, no rails, no tone. The red-rail vocabulary stays reserved for real
 * fleet faults, and a dead feed is a panel fault. The words say what it means:
 * nothing is arriving, so whatever is behind them is old. A pixel sweeps the gap
 * row so a frozen render is distinguishable from a live one at a glance.      */
void panelScreenLinkLost(float t) {
  panelFbDimAll(0.06f);
  panelTextCentre(PANEL_ROW_TOP, "NO DATA FEED", cWarn, 1.0f);
  panelTextCentre(PANEL_ROW_BOT, "SCREEN IS OLD", cInk, 1.0f);
  int sweep = ((int)(t * 12.0f)) % PANEL_W;
  panelFbSet(sweep, 5, cWarn, 0.6f);
}

/* ---- NO DATA ------------------------------------------------------------
 * Before the first snapshot the panel is waiting; afterwards an empty fleet is
 * valid data (CONTRACT §9). The two read differently because they mean
 * different things. A slow pulse shows the render loop is alive.             */
void panelScreenNoData(bool haveData, float t) {
  panelTextCentre(PANEL_ROW_ONE, haveData ? "NO HOSTS" : "AWAITING DATA", cAzure,
                  0.6f + 0.4f * (0.5f + 0.5f * sinf(t * 0.8f)));
}
