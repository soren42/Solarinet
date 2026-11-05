/*
 * panelScreens.h — the sixteen screen renderers plus the universal overlays.
 *
 * CONTRACT-V2.md §1 is the authority. Themes group screens by the question they
 * answer; flattened index is theme*4+slot:
 *
 *   A FLEET      A0 Host Tiles   A1 Roster       A2 Pools        A3 Head Count
 *   B TRAFFIC    B0 Swim Lanes   B1 WAN and LAN  B2 Top Talkers  B3 Flow Map
 *   C TROUBLE    C0 Alerts       C1 Fault Board  C2 Reachability C3 Alert History
 *   D RESOURCES  D0 Host Detail  D1 CPU and Mem  D2 Trends       D3 Aurora
 *
 * Every renderer paints into the shared framebuffer (panelFb.h) and owns no
 * data: the environment is read-only. Motion state and page clocks are
 * file-static and advance only by dt, so a screen resumes where it left off.
 *
 * Input to every renderer: env (last applied snapshot + history), t (seconds
 * since boot), dt (seconds since the previous 25 Hz tick).
 */

#ifndef SOLARI_PANEL_SCREENS_H
#define SOLARI_PANEL_SCREENS_H

#include "panelHist.h"
#include "panelFb.h"
#include "panelFont.h"

#define PANEL_SCREENS 16u
/* Text rows, CONTRACT-V2 §2. */
#define PANEL_ROW_TOP 0
#define PANEL_ROW_BOT 6
#define PANEL_ROW_ONE 3
#define PANEL_PAGE_SEC 3.0f

typedef void (*PanelScreenFn)(const PanelEnv *env, float t, float dt);

void panelScreenA0(const PanelEnv *env, float t, float dt);
void panelScreenA1(const PanelEnv *env, float t, float dt);
void panelScreenA2(const PanelEnv *env, float t, float dt);
void panelScreenA3(const PanelEnv *env, float t, float dt);
void panelScreenB0(const PanelEnv *env, float t, float dt);
void panelScreenB1(const PanelEnv *env, float t, float dt);
void panelScreenB2(const PanelEnv *env, float t, float dt);
void panelScreenB3(const PanelEnv *env, float t, float dt);
void panelScreenC0(const PanelEnv *env, float t, float dt);
void panelScreenC1(const PanelEnv *env, float t, float dt);
void panelScreenC2(const PanelEnv *env, float t, float dt);
void panelScreenC3(const PanelEnv *env, float t, float dt);
void panelScreenD0(const PanelEnv *env, float t, float dt);
void panelScreenD1(const PanelEnv *env, float t, float dt);
void panelScreenD2(const PanelEnv *env, float t, float dt);
void panelScreenD3(const PanelEnv *env, float t, float dt);

/* ---- shared layout helpers (panelScreensCommon.c) ------------------------ */

/* panelStateColor — the one state-to-hue mapping. */
const float *panelStateColor(uint8_t state);

/* panelScreenName — the on-panel title, at most 13 characters. */
const char *panelScreenName(unsigned index);

/* panelScreenPages — how many pages the screen has right now (1 = not paged). */
int panelScreenPages(const PanelEnv *env, unsigned index);

/* panelScreenMinDwell — seconds needed to show every page once, capped so that
 * a large fleet resumes on the next tour instead of holding the panel. 0 for
 * screens that do not page. */
float panelScreenMinDwell(const PanelEnv *env, unsigned index);

/* panelPageTick — advance one screen's private page clock.
 * Input: screen index, dt, page count. Output: the page to draw, 0..pages-1. */
int panelPageTick(unsigned index, float dt, int pages);

/* panelPageBar — progress marker on the gap row, drawn only when paged. */
void panelPageBar(int page, int pages);

/* panelTextFit — panelText cut to at most maxChars characters. */
int panelTextFit(int x, int y, const char *s, int maxChars, PanelColor c, float b);

/* panelTextRight — small text whose last pixel lands on column xEnd. */
void panelTextRight(int xEnd, int y, const char *s, PanelColor c, float b);

/* panelTextCentre — small text centred on the panel, cut to 13 characters. */
void panelTextCentre(int y, const char *s, PanelColor c, float b);

/* panelRateLevel — shared log scale, 1 kbps..1 Gbps onto 0..1, so a busy WAN
 * cannot drown a quiet LAN. Input: kbps. Output: 0..1. */
float panelRateLevel(float kbps);

/* panelTitleCard — names the screen for its first second, dimming the frame
 * (CONTRACT-V2 §2). Input: flattened index, seconds since it came up. */
void panelTitleCard(unsigned index, float age);

/* panelStaleMark — two amber corner pixels: the server says its own upstream
 * data is old, so the picture may be behind. */
void panelStaleMark(void);

/* ---- overlays (panelInlay.c) --------------------------------------------- */

/* panelInlay — the universal alert overlay. Drawn on top of whatever screen is
 * active whenever the alarm is armed and unacknowledged. */
void panelInlay(const PanelEnv *env, float t);

/* panelInlayPower — CONTRACT-SW §7/D16: the alert inlay carrying the local
 * power-loss episode. Drawn only when the alarm is armed and NO live server
 * topAlert exists — a server alert always wins the inlay text. */
void panelInlayPower(float t);

/* panelPowerGlyph — DESIGN-BRIEF-P4 §2: steady warn-amber 4x5 battery outline,
 * bottom-right (cols 49-52, rows 6-10), with a 1 px knockout halo. Shown
 * WHENEVER on battery, alarm state notwithstanding — including in sleep. */
void panelPowerGlyph(void);

/* panelBeacon — OPERATOR AMENDMENT: 4-pixel top-right red flasher, live from an
 * episode's rising edge until it is acknowledged, independent of tone state.
 * Painted after everything else, including in sleep. */
void panelBeacon(float t);

/* panelScreenLinkLost — CONTRACT §4: >15 s with no valid frame. Amber, never the
 * alarm: a dead cable is a panel fault, not a fleet fault. */
void panelScreenLinkLost(float t);

/* panelScreenNoData — before the first snapshot, or a fleet of zero hosts, which
 * CONTRACT §9 treats as valid data. Input: whether a snapshot ever arrived, t. */
void panelScreenNoData(bool haveData, float t);

#endif /* SOLARI_PANEL_SCREENS_H */
