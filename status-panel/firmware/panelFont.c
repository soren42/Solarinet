/*
 * panelFont.c — bitmap fonts, ported verbatim from the Turn 3 prototype.
 *
 * Encoding: the small font packs 5 rows x 3 bits into a uint16 (row 0 in the
 * most significant bits, leftmost pixel first); the big font packs 7 rows x
 * 4 bits into a uint32. Tables were generated mechanically from the
 * prototype's string tables, not hand-typed.
 */

#include <math.h>
#include <string.h>
#include "panelFont.h"

typedef struct { char ch; uint16_t rows; } PanelGlyph;
typedef struct { char ch; uint32_t rows; } PanelBigGlyph;

static const PanelGlyph gFont[] = {
  { '0', 0x7B6Fu }, { '1', 0x2C97u }, { '2', 0x73E7u }, { '3', 0x73CFu },
  { '4', 0x5BC9u }, { '5', 0x79CFu }, { '6', 0x79EFu }, { '7', 0x7292u },
  { '8', 0x7BEFu }, { '9', 0x7BCFu }, { 'A', 0x7BEDu }, { 'B', 0x6BAEu },
  { 'C', 0x7927u }, { 'D', 0x6B6Eu }, { 'E', 0x79E7u }, { 'F', 0x79E4u },
  { 'G', 0x796Fu }, { 'H', 0x5BEDu }, { 'I', 0x7497u }, { 'J', 0x126Fu },
  { 'K', 0x5BADu }, { 'L', 0x4927u }, { 'M', 0x5FEDu }, { 'N', 0x6B6Du },
  { 'O', 0x7B6Fu }, { 'P', 0x7BE4u }, { 'Q', 0x7B79u }, { 'R', 0x7BF5u },
  { 'S', 0x79CFu }, { 'T', 0x7492u }, { 'U', 0x5B6Fu }, { 'V', 0x5B6Au },
  { 'W', 0x5BFDu }, { 'X', 0x5AADu }, { 'Y', 0x5A92u }, { 'Z', 0x72A7u },
  { '/', 0x12A4u }, { ':', 0x0410u }, { '.', 0x0002u }, { '-', 0x01C0u },
  { '%', 0x52A5u }, { '+', 0x05D0u }, { '>', 0x4454u }, { '!', 0x2482u },
  { ' ', 0x0000u },
};

static const PanelBigGlyph gBigFont[] = {
  { '0', 0x6999996u }, { '1', 0x2622227u }, { '2', 0x691248Fu },
  { '3', 0xE11611Eu }, { '4', 0x26AAF22u }, { '5', 0xF88E196u },
  { '6', 0x688E996u }, { '7', 0xF122444u }, { '8', 0x6996996u },
  { '9', 0x6997116u }, { '.', 0x0000006u }, { '%', 0x9922499u },
  { 'G', 0x788B996u }, { 'M', 0x9FF9999u }, { 'S', 0x788611Eu },
  { ' ', 0x0000000u },
};

/* Uppercasing is done here so callers can pass mixed case, exactly as the
 * prototype's String(str).toUpperCase() did. */
static char upper(char c) {
  return (c >= 'a' && c <= 'z') ? (char)(c - 'a' + 'A') : c;
}

static uint16_t glyphSmall(char c) {
  c = upper(c);
  for (unsigned i = 0; i < sizeof(gFont) / sizeof(gFont[0]); i++)
    if (gFont[i].ch == c) return gFont[i].rows;
  return 0u;   /* unknown -> space, per (F[ch] || F[' ']) */
}

static uint32_t glyphBig(char c) {
  c = upper(c);
  for (unsigned i = 0; i < sizeof(gBigFont) / sizeof(gBigFont[0]); i++)
    if (gBigFont[i].ch == c) return gBigFont[i].rows;
  return 0u;
}

int panelTextW(const char *s) { return (int)strlen(s) * 4 - 1; }
int panelBigW(const char *s)  { return (int)strlen(s) * 5 - 1; }

/* Emit one 3x5 glyph at (cx,y). */
static void drawSmall(int cx, int y, char ch, PanelColor c, float b) {
  uint16_t g = glyphSmall(ch);
  for (int ry = 0; ry < 5; ry++)
    for (int rx = 0; rx < 3; rx++)
      if (g & (1u << (14 - (ry * 3 + rx)))) panelFbSet(cx + rx, y + ry, c, b);
}

int panelText(int x, int y, const char *s, PanelColor c, float b) {
  int cx = x;
  for (const char *p = s; *p; p++) { drawSmall(cx, y, *p, c, b); cx += 4; }
  return cx;
}

int panelTextOver(int x, int y, const char *s, PanelColor c, float b) {
  int cx = x;
  for (const char *p = s; *p; p++) {
    uint16_t g = glyphSmall(*p);
    for (int ry = 0; ry < 5; ry++) {
      for (int rx = 0; rx < 3; rx++) {
        if (!(g & (1u << (14 - (ry * 3 + rx))))) continue;
        for (int dy = -1; dy <= 1; dy++)
          for (int dx = -1; dx <= 1; dx++)
            panelFbDim(cx + rx + dx, y + ry + dy, 0.12f);
      }
    }
    cx += 4;
  }
  return panelText(x, y, s, c, b);
}

int panelBig(int x, int y, const char *s, PanelColor c, float b) {
  int cx = x;
  for (const char *p = s; *p; p++) {
    uint32_t g = glyphBig(*p);
    for (int ry = 0; ry < 7; ry++)
      for (int rx = 0; rx < 4; rx++)
        if (g & (1u << (27 - (ry * 4 + rx)))) panelFbSet(cx + rx, y + ry, c, b);
    cx += 5;
  }
  return cx;
}

void panelScroll(float t, int y, const char *s, PanelColor c, float b, float speed) {
  int total = panelTextW(s) + PANEL_W;
  if (total <= 0) return;
  float travelled = t * speed;
  int off = (int)fmodf(travelled, (float)total);
  if (off < 0) off += total;
  int cx = PANEL_W - off;
  for (const char *p = s; *p; p++) {
    if (cx > -4 && cx < PANEL_W) drawSmall(cx, y, *p, c, b);
    cx += 4;
  }
}
