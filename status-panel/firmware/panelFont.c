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

/* panelTextInk — LED legibility gamma for SMALL-FONT TEXT ONLY.
 *
 * HARDWARE FINDING 2026-08-04 (Jason, eyes on the live panel): B1's right-hand
 * readout was unreadable on the real LEDs. The design's brightness values were
 * authored against an LCD mockup, where a mid-grey ink at b=0.45 still reads.
 * On the matrix it does not: the panel's own PWM response is far from linear at
 * the bottom, and the driver's global brightness (0.25 at night, and the design
 * WANTS it low then) multiplies on top, so a 0.45 ink lands somewhere around
 * 0.11 of full scale before the LED's own curve eats most of what is left.
 *
 * Applied here rather than by editing the ~20 call sites for two reasons.
 * First, spot values would need re-tuning every time a screen changes, and a
 * future screen would reintroduce the bug by default. Second, and the deciding
 * one: a flat floor (the obvious alternative) collapses the design's own
 * hierarchy — C2's 0.28 "BY POOL" caption and B1's 0.5 primary readout would
 * come out at the same brightness, which throws away information the design is
 * deliberately encoding. A gamma curve is monotonic, so every ink gets lifted
 * while their ORDER and their relative separation survive.
 *
 * REVISED 2026-08-04, second hardware pass (photo: ~/Downloads/unicorn-contrast
 * .jpg). b^0.55 measurably raised the pixels and was still unreadable, because
 * the problem is not the curve — it is a REFLECTION FLOOR. The unlit LED
 * packages are pale cream plastic, and in a lit room they bounce enough ambient
 * to sit at roughly the luminance of a mid-duty lit pixel. The photo shows it
 * plainly: unlit packages read almost white, lit azure pixels only modestly
 * brighter. No gamma exponent can fix that, because the floor is not in our
 * signal path at all. Below it, dim ink of ANY colour is invisible.
 *
 * So text no longer carries hierarchy in luminance. It is mapped into a narrow
 * high band, 0.85 + 0.15*b:
 *   0.16->0.874  0.28->0.892  0.30->0.895  0.45->0.918  0.50->0.925
 *   0.55->0.933  0.60->0.940  0.95->0.993
 * Every ink clears the reflection floor with margin. The mapping is still
 * monotonic, so what remains of the design's ordering is preserved, but it is
 * deliberately a whisper now — hierarchy moved to HUE (see panelScreens*.c:
 * white = primary readout, azure = secondary label) and to size and position.
 * The principle, operator-endorsed: text pixels should be FEW AND BRIGHT, never
 * many and dim.
 *
 * SCOPE — what this deliberately does NOT touch:
 *   - panelBig(). The BIG numerals are watermarks at b=0.3, a background
 *     element that panelTextOver's halo knockout is designed to sit ON TOP of.
 *     Lifting the watermark would cut the contrast that makes the overlaid
 *     label legible — the exact opposite of the fix.
 *   - every non-text primitive: bars, ramps, particles, rails, sparks. Those
 *     are the structural pixels, and their dimness is the design working.
 *   - all colour hues. This scales brightness only; no condition colour moves.
 * Input: b, the caller's design brightness. Output: the corrected brightness. */
static float panelTextInk(float b) {
  if (b <= 0.0f) return 0.0f;
  if (b >= 1.0f) return 1.0f;
  return 0.85f + 0.15f * b;
}

/* Emit one 3x5 glyph at (cx,y). Every small-font path in this file funnels
 * through here — panelText, panelTextOver (which calls panelText) and
 * panelScroll — so it is the one seam where the legibility gamma belongs. */
static void drawSmall(int cx, int y, char ch, PanelColor c, float b) {
  uint16_t g = glyphSmall(ch);
  float ink = panelTextInk(b);
  for (int ry = 0; ry < 5; ry++)
    for (int rx = 0; rx < 3; rx++)
      if (g & (1u << (14 - (ry * 3 + rx)))) panelFbSet(cx + rx, y + ry, c, ink);
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
