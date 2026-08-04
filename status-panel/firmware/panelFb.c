/*
 * panelFb.c — framebuffer primitives. Direct port of Themes.dc.html's
 * clear/set/add/dim/dimAll/paint helpers.
 */

#include <math.h>
#include "panelFb.h"

static float gFb[PANEL_W * PANEL_H * 3];

PanelColor cOk      = { 61.0f, 214.0f, 140.0f };
PanelColor cWarn    = { 245.0f, 166.0f, 35.0f };
PanelColor cCrit    = { 255.0f, 77.0f, 94.0f };
PanelColor cMaint   = { 155.0f, 135.0f, 255.0f };
PanelColor cAzure   = { 34.0f, 184.0f, 240.0f };
PanelColor cQuiet   = { 78.0f, 107.0f, 126.0f };
PanelColor cInk     = { 223.0f, 232.0f, 242.0f };
PanelColor cUnknown = { 124.0f, 138.0f, 160.0f };

void panelFbClear(void) {
  for (int i = 0; i < PANEL_W * PANEL_H * 3; i++) gFb[i] = 0.0f;
}

void panelFbSet(int x, int y, PanelColor c, float b) {
  if (x < 0 || x >= PANEL_W || y < 0 || y >= PANEL_H) return;
  int i = (y * PANEL_W + x) * 3;
  gFb[i] = c[0] * b; gFb[i + 1] = c[1] * b; gFb[i + 2] = c[2] * b;
}

void panelFbSetRgb(int x, int y, float r, float g, float bl, float b) {
  if (x < 0 || x >= PANEL_W || y < 0 || y >= PANEL_H) return;
  int i = (y * PANEL_W + x) * 3;
  gFb[i] = r * b; gFb[i + 1] = g * b; gFb[i + 2] = bl * b;
}

void panelFbAdd(int x, int y, PanelColor c, float b) {
  if (x < 0 || x >= PANEL_W || y < 0 || y >= PANEL_H) return;
  int i = (y * PANEL_W + x) * 3;
  gFb[i] += c[0] * b; gFb[i + 1] += c[1] * b; gFb[i + 2] += c[2] * b;
}

void panelFbDim(int x, int y, float k) {
  if (x < 0 || x >= PANEL_W || y < 0 || y >= PANEL_H) return;
  int i = (y * PANEL_W + x) * 3;
  gFb[i] *= k; gFb[i + 1] *= k; gFb[i + 2] *= k;
}

void panelFbDimAll(float k) {
  for (int i = 0; i < PANEL_W * PANEL_H * 3; i++) gFb[i] *= k;
}

/* Prototype paint(): R = min(255, round(fb[i] * g)). The global scalar g is
 * applied downstream by the LED driver, so only the clamp happens here. */
static uint8_t chan(float v) {
  int n = (int)(v + 0.5f);
  if (n < 0) n = 0;
  if (n > 255) n = 255;
  return (uint8_t)n;
}

void panelFbFlush(void) {
  for (int y = 0; y < PANEL_H; y++) {
    for (int x = 0; x < PANEL_W; x++) {
      int i = (y * PANEL_W + x) * 3;
      panelHwSetPixel(x, y, chan(gFb[i]), chan(gFb[i + 1]), chan(gFb[i + 2]));
    }
  }
}
