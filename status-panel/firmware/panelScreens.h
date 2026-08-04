/*
 * panelScreens.h — the twelve screen renderers plus the universal inlay.
 *
 * Naming follows DESIGN-BRIEF's own sA0..sD2 identifiers so a reviewer can map
 * each function to its section of the brief and to the Turn 3 prototype
 * function of the same name:
 *
 *   A ABSTRACT     A0 State lattice   A1 Flow gates    A2 MQ and SNMP
 *   B TEXT         B0 Condition counts B1 Throughput   B2 Load and latency
 *   C CHARTS       C0 Load distribution C1 Live traces C2 Pool load
 *   D INSTRUMENTS  D0 Reachability    D1 Pool bars     D2 Ambient field
 *
 * Every renderer paints into the shared framebuffer (panelFb.h) and owns no
 * data: the environment is read-only. Renderers that need motion state (A1's
 * particles, D0's waterfall) keep it file-static and seed it deterministically.
 *
 * Input to every renderer: env (last applied snapshot + history), t (seconds
 * since boot), dt (seconds since the previous 25 Hz tick).
 */

#ifndef SOLARI_PANEL_SCREENS_H
#define SOLARI_PANEL_SCREENS_H

#include "panelHist.h"
#include "panelFb.h"
#include "panelFont.h"

typedef void (*PanelScreenFn)(const PanelEnv *env, float t, float dt);

void panelScreenA0(const PanelEnv *env, float t, float dt);
void panelScreenA1(const PanelEnv *env, float t, float dt);
void panelScreenA2(const PanelEnv *env, float t, float dt);
void panelScreenB0(const PanelEnv *env, float t, float dt);
void panelScreenB1(const PanelEnv *env, float t, float dt);
void panelScreenB2(const PanelEnv *env, float t, float dt);
void panelScreenC0(const PanelEnv *env, float t, float dt);
void panelScreenC1(const PanelEnv *env, float t, float dt);
void panelScreenC2(const PanelEnv *env, float t, float dt);
void panelScreenD0(const PanelEnv *env, float t, float dt);
void panelScreenD1(const PanelEnv *env, float t, float dt);
void panelScreenD2(const PanelEnv *env, float t, float dt);

/* panelInlay — the universal alert overlay (DESIGN-BRIEF "Universal alert
 * inlay"). Drawn on top of whatever screen is active whenever the alarm is
 * armed and unacknowledged. */
void panelInlay(const PanelEnv *env, float t);

/* panelScreenLinkLost — CONTRACT §4: >15 s with no valid frame. Not a design
 * screen; the brief only says "renders the design's stale indicator", so this
 * is the smallest treatment that reads as an explicit link fault. */
void panelScreenLinkLost(float t);

/* panelScreenNoData — CONTRACT §9: a zero-node fleet is valid data, not an
 * error, and gets its own NO DATA treatment. */
void panelScreenNoData(float t);

/* panelPoolLane — shared helper: pool view for a lane index, clamped to the
 * pools actually present. Returns a zeroed pool when the fleet is empty. */
const PanelPoolView *panelPoolLane(const PanelEnv *env, int wanted);

/* panelRng — the prototype's LCG (s = s*1664525 + 1013904223, /2^32), used so
 * particle seeds and waterfall jitter match the mockup's texture. */
typedef struct { uint32_t s; } PanelRng;
void  panelRngSeed(PanelRng *r, uint32_t seed);
float panelRngNext(PanelRng *r);

#endif /* SOLARI_PANEL_SCREENS_H */
