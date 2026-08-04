/*
 * panelFb.h — float RGB framebuffer and the primitive paint ops.
 *
 * DESIGN-BRIEF "Display model": the framebuffer is float RGB per pixel
 * (Float32Array(W*H*3) in the prototype), values scaled 0..255 at paint time
 * by a global brightness multiplier. We keep that exactly: every screen writes
 * unclamped floats, and only panelFbFlush() clamps to 0..255. The global
 * brightness scalar lives in the LED driver (panelHwSetBrightness), which
 * applies it at the same point the prototype's `g` did.
 *
 * The primitives are 1:1 ports of the prototype's set/add/dim/dimAll helpers
 * (Themes.dc.html lines 195-211) — including the distinction between set
 * (assign) and add (accumulate), which several screens rely on for glow tails.
 */

#ifndef SOLARI_PANEL_FB_H
#define SOLARI_PANEL_FB_H

#include <stdint.h>
#include "panelHw.h"

/* DESIGN-BRIEF "Colors (exact hex / RGB, Turn 3 canonical palette C)".
 * Stored as float triples because screens scale them by fractional brightness
 * before writing, exactly as the prototype does. */
typedef const float PanelColor[3];

extern PanelColor cOk;      /* 61,214,140  #3DD68C */
extern PanelColor cWarn;    /* 245,166,35  #F5A623 */
extern PanelColor cCrit;    /* 255,77,94   #FF4D5E */
extern PanelColor cMaint;   /* 155,135,255 #9B87FF */
extern PanelColor cAzure;   /* 34,184,240  #22B8F0 */
extern PanelColor cQuiet;   /* 78,107,126  #4E6B7E */
extern PanelColor cInk;     /* 223,232,242 #DFE8F2 */
/* Turn 1/2 only; DESIGN-BRIEF §Colors recommends keeping it defined for the
 * UNKNOWN state that real telemetry produces and the mockup never did. */
extern PanelColor cUnknown; /* 124,138,160 #7C8AA0 */

/* panelFbClear — zero the framebuffer. */
void panelFbClear(void);

/* panelFbSet — assign colour*b to a pixel. Out-of-range coords are dropped. */
void panelFbSet(int x, int y, PanelColor c, float b);

/* panelFbSetRgb — as panelFbSet but with a caller-computed colour triple,
 * used by D2's azure->warn interpolation. */
void panelFbSetRgb(int x, int y, float r, float g, float bl, float b);

/* panelFbAdd — accumulate colour*b onto a pixel (glow tails, particles). */
void panelFbAdd(int x, int y, PanelColor c, float b);

/* panelFbDim — multiply one pixel by k (knockout haloes). */
void panelFbDim(int x, int y, float k);

/* panelFbDimAll — multiply the whole framebuffer by k (alert inlay's 10%). */
void panelFbDimAll(float k);

/* panelFbFlush — clamp/round to 0..255 and push every pixel to the matrix. */
void panelFbFlush(void);

#endif /* SOLARI_PANEL_FB_H */
