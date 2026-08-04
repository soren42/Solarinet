/*
 * panelFont.h — the two bitmap fonts and the text primitives.
 *
 * DESIGN-BRIEF "Typography":
 *   small font F   3x5 px glyphs, 4 px pitch, textW(s) = len*4 - 1.
 *                  charset 0-9 A-Z and / : . - % + > ! and space.
 *   large font BIG 4x7 px glyphs, 5 px pitch, bigW(s) = len*5 - 1.
 *                  charset 0-9 and . % G M S and space.
 * Glyph bitmaps are machine-transcribed from the Turn 3 prototype's F/BIG
 * tables (Themes.dc.html lines 7-34) so they are pixel-identical, not redrawn.
 */

#ifndef SOLARI_PANEL_FONT_H
#define SOLARI_PANEL_FONT_H

#include "panelFb.h"

/* panelTextW / panelBigW — rendered pixel width of a string. */
int panelTextW(const char *s);
int panelBigW(const char *s);

/* panelText — draw 3x5 text at (x,y). Output: x of the next glyph cell
 * (i.e. x + 4*len), matching the prototype's return value which callers
 * use to pack labels left-to-right. */
int panelText(int x, int y, const char *s, PanelColor c, float b);

/* panelTextOver — DESIGN-BRIEF "watermark technique": knock a 1 px halo
 * (x0.12) around every lit pixel of the string, then draw the string, so it
 * stays legible over a BIG watermark numeral. Same return as panelText. */
int panelTextOver(int x, int y, const char *s, PanelColor c, float b);

/* panelBig — draw 4x7 headline numerals. Output: x of the next glyph cell. */
int panelBig(int x, int y, const char *s, PanelColor c, float b);

/* panelScroll — horizontally scrolling single line, wrapping over
 * (textW(s) + PANEL_W). DESIGN-BRIEF "Animation": 13 px/s default, 11 px/s
 * for the B1/B2/D1 ticker lines and the inlay detail.
 * Input: t = seconds since boot, speed = px/s. */
void panelScroll(float t, int y, const char *s, PanelColor c, float b, float speed);

#endif /* SOLARI_PANEL_FONT_H */
