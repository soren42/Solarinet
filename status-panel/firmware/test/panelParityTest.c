/* panelParityTest.c — framebuffer-assertion harness for the status panel.
 *
 * WHY THIS EXISTS
 * ---------------
 * The panel shipped three real rendering defects behind a green test suite,
 * because nothing in that suite ever rendered a screen and looked at the lit
 * pixels:
 *   B1  a y=4 reading row was clipped and overwritten by the bottom ticker;
 *   C2  a 5-row label drawn at y=9 was clipped (rows past y=10 do not exist);
 *   B1  a panelTextOver 1 px halo from a title-row percent dimmed the warn and
 *       crit cells of a bar that had already been painted (paint order).
 * All three are now fixed in the renderers. This harness is the regression
 * floor that stops them coming back, and it is the fixture-driven parity
 * oracle RETURN-AW3 UNVERIFIED #3 asked for.
 *
 * THE ONE RULE
 * ------------
 * This file contains NO rendering logic and NO transcribed geometry. It links
 * the REAL production renderers (panelScreensA-D.c, panelInlay.c,
 * panelHelpOverlay.c) against the REAL framebuffer and font (panelFb.c,
 * panelFont.c) and observes them from outside. Everything it knows about
 * geometry it learns by watching the renderers paint. A previous lane tested a
 * reimplementation and proved nothing; if you ever find yourself typing a band
 * or a row number in here, stop.
 *
 * HOW IT OBSERVES
 * ---------------
 * `ld --wrap` intercepts the framebuffer primitives and the text primitives.
 * The text wrappers set a "current paint tag" for the duration of the call, so
 * every framebuffer write arrives carrying its provenance: structural, small
 * text, watermark-knockout text, BIG headline, or scrolling ticker. That gives
 * per-pixel text-vs-structure classification for free.
 *
 * It matters that the wrappers see the ATTEMPTED coordinates. panelFbSet drops
 * out-of-range writes silently, so a golden image can never show you a label
 * that ran off the bottom of the panel — the pixels simply are not there. The
 * wrapper sees the renderer try, which is what makes invariant T2 possible.
 *
 * Note that --wrap only redirects UNDEFINED references, so panelTextOver's
 * internal call to panelText inside panelFont.c is not intercepted. That is
 * the behaviour we want: the tag set by the outer panelTextOver wrapper stays
 * in force for the glyph pass too.
 *
 * WHAT IT ASSERTS
 * ---------------
 * G   per-screen golden framebuffers, committed under fixtures/golden/.
 *     Regenerate with PARITY_REGEN=1 — and read the diff before you commit it.
 *     Regeneration is a DEVELOPER action and stays one: if CI is also set in
 *     the environment the binary refuses and exits 2 rather than rewriting the
 *     evidence a CI run exists to check. See regenRefusedInCI().
 * T1  no small-font glyph shares a row with the scrolling ticker (the B1
 *     clipped-reading defect). BIG is exempt: the ticker crossing the BIG
 *     watermark is the design's own watermark technique.
 * T2  no glyph is painted outside rows 0..PANEL_H-1 (the C2 defect). Measured
 *     on attempted coordinates, so clipping cannot hide it.
 * T3  a warn/crit cell painted structurally still holds its palette colour
 *     after every text and halo pass has run (the B1 paint-order defect).
 *     Checked against the palette constant and its brightness, not "non-zero".
 *
 * Run: make -C status-panel/firmware/test                                    */

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../panelScreens.h"
#include "../panelHelpOverlay.h"
#include "testJson.h"

#define FIXTURE_PATH "../../fixtures/parity-fixture.json"
#define GOLDEN_DIR   "../../fixtures/golden"

/* Frames are ticked at the firmware's 25 Hz so any integrator the renderers
 * carry (A1 particles, D0 waterfall) advances exactly as it does on hardware,
 * rather than being teleported to a sample time it never passes through. */
#define TICK_DT      0.04f
#define SAMPLE_A     0
#define SAMPLE_B     125   /* t = 5 s  */
#define SAMPLE_C     250   /* t = 10 s */
#define FRAMES       (SAMPLE_C + 1)

static int gPass = 0;
static int gFail = 0;

/* Purpose: record one assertion. Input: condition, name, detail. Output: none. */
static void ok(int cond, const char *name, const char *detail) {
  if (cond) {
    ++gPass;
    printf("ok:   %s\n", name);
  } else {
    ++gFail;
    printf("FAIL: %s%s%s\n", name, detail && *detail ? " — " : "",
           detail ? detail : "");
  }
}

/* ---------------------------------------------------------------- capture -- */

static unsigned char gCap[PANEL_H][PANEL_W][3];
static int gOob = 0;

void panelHwSetPixel(int x, int y, unsigned char r, unsigned char g,
                     unsigned char b) {
  if (x < 0 || x >= PANEL_W || y < 0 || y >= PANEL_H) { ++gOob; return; }
  gCap[y][x][0] = r;
  gCap[y][x][1] = g;
  gCap[y][x][2] = b;
}

/* ------------------------------------------------------ paint provenance -- */

typedef enum {
  TAG_STRUCT = 0,  /* the renderer drew this itself, not through a font call */
  TAG_TEXT,
  TAG_TEXTOVER,
  TAG_BIG,
  TAG_SCROLL,
  TAG__COUNT
} PaintTag;

static PaintTag gTag = TAG_STRUCT;

/* Attempted glyph rows per tag, this frame. Offset so out-of-panel rows above
 * and below are representable — that is the whole point of T2. */
#define ROW_LO (-16)
#define ROW_HI (PANEL_H + 16)
#define ROW_N  (ROW_HI - ROW_LO)
static unsigned char gRowTouch[TAG__COUNT][ROW_N];

/* T2 evidence. */
static int  gOverflow = 0;
static char gOverflowNote[160];

/* T3 guard: pixels last painted structurally in a conditional palette colour. */
/* Leg-path capture, armed only for the A1 cases. See __wrap_panelFbSet. */
static int           gCollectQuiet;
static unsigned char gLegPix[PANEL_H][PANEL_W];

static unsigned char gGuard[PANEL_H][PANEL_W];
static const float  *gGuardCol[PANEL_H][PANEL_W];
static float         gGuardB[PANEL_H][PANEL_W];
static int  gHaloHits = 0;
static char gHaloNote[160];

/* Coverage. An invariant nothing triggers is an invariant that proves nothing,
 * so the run also asserts that the fixture actually drove each mechanism. */
static int gArmed = 0;      /* T3 guards armed this frame                  */

/* Purpose: is this tag a glyph-painting pass? Input: tag. Output: 0/1. */
static int isTextTag(PaintTag tg) { return tg != TAG_STRUCT; }

/* Purpose: start a fresh frame of observations. Input: none. Output: none. */
static void obsReset(void) {
  memset(gRowTouch, 0, sizeof gRowTouch);
  memset(gGuard, 0, sizeof gGuard);
  memset(gGuardCol, 0, sizeof gGuardCol);
  memset(gGuardB, 0, sizeof gGuardB);
  gOverflow = 0;
  gOverflowNote[0] = '\0';
  gHaloHits = 0;
  gHaloNote[0] = '\0';
  gArmed = 0;
  gTag = TAG_STRUCT;
}

/* Purpose: note an attempted glyph row. Input: y. Output: none. */
static void noteGlyphRow(int y) {
  if (y >= ROW_LO && y < ROW_HI) gRowTouch[gTag][y - ROW_LO] = 1u;
  if ((y < 0 || y >= PANEL_H) && gOverflow++ == 0) {
    snprintf(gOverflowNote, sizeof gOverflowNote,
             "glyph pixel attempted at row %d (panel has rows 0..%d)", y,
             PANEL_H - 1);
  }
}

/* Purpose: do two tags share any attempted row? Input: tags. Output: row, -1. */
static int sharedRow(PaintTag a, PaintTag b) {
  for (int i = 0; i < ROW_N; i++)
    if (gRowTouch[a][i] && gRowTouch[b][i]) return i + ROW_LO;
  return -1;
}

/* ------------------------------------------------------------- the wraps -- */

void __real_panelFbSet(int x, int y, PanelColor c, float b);
void __real_panelFbSetRgb(int x, int y, float r, float g, float bl, float b);
void __real_panelFbAdd(int x, int y, PanelColor c, float b);
void __real_panelFbDim(int x, int y, float k);
void __real_panelFbDimAll(float k);
int  __real_panelText(int x, int y, const char *s, PanelColor c, float b);
int  __real_panelTextOver(int x, int y, const char *s, PanelColor c, float b);
int  __real_panelBig(int x, int y, const char *s, PanelColor c, float b);
void __real_panelScroll(float t, int y, const char *s, PanelColor c, float b,
                        float speed);

static int inPanel(int x, int y) {
  return x >= 0 && x < PANEL_W && y >= 0 && y < PANEL_H;
}

void __wrap_panelFbSet(int x, int y, PanelColor c, float b) {
  if (isTextTag(gTag)) noteGlyphRow(y);
  /* Leg-path capture. Inside an A1 render cQuiet has exactly one source —
   * a1Leg() — because gates take their colour from a1GateColor(), which returns
   * cCrit/cWarn/cAzure and never cQuiet, and the particles reuse a1GateColor()
   * too. CONTRACT-AW §4 is what guarantees that stays true: cQuiet is
   * connective texture and never a state carrier. So this records the leg layer
   * by PALETTE IDENTITY, deriving no geometry — the coordinates come from the
   * real a1Leg(). The dump lets tests/dashboard/test_panel_parity.js exempt
   * *these specific pixels* from its diff instead of exempting any quiet pixel
   * anywhere. If cQuiet ever gains a second use inside A1 this over-collects,
   * which is why the JS side also caps the exemption by count. */
  if (gCollectQuiet && !isTextTag(gTag) && c == cQuiet && inPanel(x, y))
    gLegPix[y][x] = 1u;
  if (inPanel(x, y)) {
    /* A structural write in a conditional colour arms the T3 guard; any other
     * write replaces the pixel outright and disarms it. */
    if (!isTextTag(gTag) && (c == cWarn || c == cCrit)) {
      gGuard[y][x] = 1u;
      gGuardCol[y][x] = c;
      gGuardB[y][x] = b;
      ++gArmed;
    } else {
      gGuard[y][x] = 0u;
    }
  }
  __real_panelFbSet(x, y, c, b);
}

void __wrap_panelFbSetRgb(int x, int y, float r, float g, float bl, float b) {
  if (inPanel(x, y)) gGuard[y][x] = 0u;
  __real_panelFbSetRgb(x, y, r, g, bl, b);
}

void __wrap_panelFbAdd(int x, int y, PanelColor c, float b) {
  if (inPanel(x, y)) gGuard[y][x] = 0u;
  __real_panelFbAdd(x, y, c, b);
}

void __wrap_panelFbDim(int x, int y, float k) {
  if (inPanel(x, y)) {
    if (gGuard[y][x] && isTextTag(gTag) && k < 1.0f) {
      /* This is the B1 defect exactly: a glyph halo eating a bar cell that was
       * painted before it. A dim issued by the renderer itself (D2's knockout
       * box, the LINK LOST plate) is deliberate and merely disarms the guard. */
      if (gHaloHits++ == 0) {
        snprintf(gHaloNote, sizeof gHaloNote,
                 "text halo dimmed a %s cell at (%d,%d) by x%.2f",
                 gGuardCol[y][x] == cCrit ? "crit" : "warn", x, y, (double)k);
      }
    }
    gGuard[y][x] = 0u;
  }
  __real_panelFbDim(x, y, k);
}

void __wrap_panelFbDimAll(float k) {
  memset(gGuard, 0, sizeof gGuard);
  __real_panelFbDimAll(k);
}

/* The text wrappers save and restore the tag so a nested call (a renderer
 * calling panelText inside its own pass) cannot leak the inner tag out. */
#define TEXT_WRAP(call, tg)         \
  PaintTag prev__ = gTag;           \
  gTag = (tg);                      \
  call;                             \
  gTag = prev__

int __wrap_panelText(int x, int y, const char *s, PanelColor c, float b) {
  int r;
  TEXT_WRAP(r = __real_panelText(x, y, s, c, b), TAG_TEXT);
  return r;
}

int __wrap_panelTextOver(int x, int y, const char *s, PanelColor c, float b) {
  int r;
  TEXT_WRAP(r = __real_panelTextOver(x, y, s, c, b), TAG_TEXTOVER);
  return r;
}

int __wrap_panelBig(int x, int y, const char *s, PanelColor c, float b) {
  int r;
  TEXT_WRAP(r = __real_panelBig(x, y, s, c, b), TAG_BIG);
  return r;
}

void __wrap_panelScroll(float t, int y, const char *s, PanelColor c, float b,
                        float speed) {
  TEXT_WRAP(__real_panelScroll(t, y, s, c, b, speed), TAG_SCROLL);
}

/* ------------------------------------------------------------- fixture ---- */

static PanelEnv gEnv;
static JVal *gFixture = NULL;

/* Purpose: map the API's state word to PanelState. Input: word. Output: enum. */
static uint8_t stateWord(const char *w) {
  if (!w) return PANEL_ST_UNKNOWN;
  if (!strcmp(w, "up") || !strcmp(w, "ok")) return PANEL_ST_OK;
  if (!strcmp(w, "degraded")) return PANEL_ST_DEGRADED;
  if (!strcmp(w, "down")) return PANEL_ST_DOWN;
  if (!strcmp(w, "maint")) return PANEL_ST_MAINT;
  return PANEL_ST_UNKNOWN;
}

/* Purpose: copy a JSON string into a fixed field. Input: dst/cap, value. */
static void copyStr(char *dst, size_t cap, const JVal *v) {
  const char *s = jStr(v, "");
  size_t n = strlen(s);
  if (n >= cap) n = cap - 1;
  memcpy(dst, s, n);
  dst[n] = '\0';
}

/* Purpose: decode one fixture history sample into a PanelSnapshot.
 * Input: sample object, snapshot to fill. Output: none. */
static void decodeSample(const JVal *s, PanelSnapshot *snap) {
  const JVal *o;
  int i, n;

  memset(snap, 0, sizeof *snap);
  snap->ts = (uint32_t)jNum(jGet(s, "ts"), 0.0);
  snap->seq = (uint16_t)jNum(jGet(s, "seq"), 0.0);
  snap->score = (uint16_t)jNum(jGet(s, "score"), 0.0);
  snap->alarmActive = (uint8_t)(jBool(jGet(s, "alarmActive"), 0) ? 1 : 0);
  snap->dataStale = (uint8_t)(jBool(jGet(s, "dataStale"), 0) ? 1 : 0);

  o = jGet(s, "stateRoll");
  snap->stateRoll[PANEL_ST_OK] = (uint8_t)jNum(jGet(o, "up"), 0.0);
  snap->stateRoll[PANEL_ST_DEGRADED] = (uint8_t)jNum(jGet(o, "degraded"), 0.0);
  snap->stateRoll[PANEL_ST_DOWN] = (uint8_t)jNum(jGet(o, "down"), 0.0);
  snap->stateRoll[PANEL_ST_UNKNOWN] = (uint8_t)jNum(jGet(o, "unknown"), 0.0);
  snap->stateRoll[PANEL_ST_MAINT] = (uint8_t)jNum(jGet(o, "maint"), 0.0);

  o = jGet(s, "alerts");
  snap->alertCounts[PANEL_SEV_INFO] = (uint8_t)jNum(jGet(o, "info"), 0.0);
  snap->alertCounts[PANEL_SEV_WARN] = (uint8_t)jNum(jGet(o, "warn"), 0.0);
  snap->alertCounts[PANEL_SEV_CRIT] = (uint8_t)jNum(jGet(o, "crit"), 0.0);

  snap->meanLoadPct = (uint8_t)jNum(jGet(s, "meanLoadPct"), 0.0);
  snap->rxKbps = (uint32_t)jNum(jGet(s, "rxKbps"), 0.0);
  snap->txKbps = (uint32_t)jNum(jGet(s, "txKbps"), 0.0);
  snap->rttTenthMs = (uint16_t)jNum(jGet(s, "rttTenthMs"), 0.0);
  snap->lossPermille = (uint16_t)jNum(jGet(s, "lossPermille"), 0.0);

  o = jGet(s, "pools");
  n = jLen(o);
  if (n > (int)PANEL_MAX_POOLS) n = (int)PANEL_MAX_POOLS;
  for (i = 0; i < n; i++) {
    const JVal *p = jAt(o, i);
    copyStr(snap->pools[i].name, sizeof snap->pools[i].name, jGet(p, "name"));
    snap->pools[i].tier = (uint8_t)jNum(jGet(p, "tier"), 0.0);
    snap->pools[i].ok = (uint8_t)jNum(jGet(p, "up"), 0.0);
    snap->pools[i].degraded = (uint8_t)jNum(jGet(p, "degraded"), 0.0);
    snap->pools[i].down = (uint8_t)jNum(jGet(p, "down"), 0.0);
    snap->pools[i].unknown = (uint8_t)jNum(jGet(p, "unknown"), 0.0);
    snap->pools[i].maint = (uint8_t)jNum(jGet(p, "maint"), 0.0);
    snap->pools[i].total = (uint8_t)jNum(jGet(p, "total"), 0.0);
    snap->pools[i].loadPct = (uint8_t)jNum(jGet(p, "loadPct"), 0.0);
  }
  snap->poolCount = (uint8_t)n;

  o = jGet(s, "systems");
  n = jLen(o);
  if (n > (int)PANEL_MAX_SYSTEMS) n = (int)PANEL_MAX_SYSTEMS;
  for (i = 0; i < n; i++) {
    const JVal *y = jAt(o, i);
    copyStr(snap->systems[i].name, sizeof snap->systems[i].name,
            jGet(y, "name"));
    snap->systems[i].state = stateWord(jStr(jGet(y, "state"), "unknown"));
    snap->systems[i].pool = (uint8_t)jNum(jGet(y, "pool"), 0.0);
    snap->systems[i].tier = (uint8_t)jNum(jGet(y, "tier"), 0.0);
    snap->systems[i].loadPct = (uint8_t)jNum(jGet(y, "loadPct"), 0.0);
  }
  snap->systemCount = (uint8_t)n;

  o = jGet(s, "topAlert");
  if (o && o->type == J_OBJ) {
    snap->hasTopAlert = 1u;
    snap->topAlert.alertId = (uint32_t)jNum(jGet(o, "alertId"), 0.0);
    snap->topAlert.episodeId = (uint32_t)jNum(jGet(o, "episodeId"), 0.0);
    snap->topAlert.severity = (uint8_t)jNum(jGet(o, "severity"), 0.0);
    copyStr(snap->topAlert.subject, sizeof snap->topAlert.subject,
            jGet(o, "subject"));
    copyStr(snap->topAlert.detail, sizeof snap->topAlert.detail,
            jGet(o, "detail"));
  }

  o = jGet(s, "gear");
  n = jLen(o);
  if (n > (int)PANEL_MAX_GEAR) n = (int)PANEL_MAX_GEAR;
  for (i = 0; i < n; i++) {
    const JVal *g = jAt(o, i);
    snap->gear[i].role = (uint8_t)jNum(jGet(g, "role"), 0.0);
    snap->gear[i].state = (uint8_t)jNum(jGet(g, "state"), 0.0);
    snap->gear[i].rxLevel = (uint8_t)jNum(jGet(g, "rxLevel"), 0.0);
    snap->gear[i].txLevel = (uint8_t)jNum(jGet(g, "txLevel"), 0.0);
  }
  snap->gearCount = (uint8_t)n;
}

/* Purpose: replay the whole fixture history into gEnv, oldest sample first,
 * so the history rings and the adaptive peak end up where the firmware would
 * have put them. Input: none. Output: number of samples applied. */
static int loadFixture(void) {
  char err[192];
  const JVal *hist;
  int i, n;

  gFixture = jsonParseFile(FIXTURE_PATH, err, sizeof err);
  if (!gFixture) {
    printf("FAIL: cannot read %s: %s\n", FIXTURE_PATH, err);
    ++gFail;
    return 0;
  }
  hist = jGet(gFixture, "history");
  n = jLen(hist);
  panelEnvInit(&gEnv);
  for (i = 0; i < n; i++) {
    PanelSnapshot snap;
    decodeSample(jAt(hist, i), &snap);
    panelEnvApply(&gEnv, &snap);
  }
  return n;
}

/* Purpose: swap in a named gear variant on the already-applied env.
 * Input: variant key. Output: 1 on success. */
static int useGearVariant(const char *key) {
  const JVal *g = jGet(jGet(gFixture, "gearVariants"), key);
  int i, n = jLen(g);
  if (n <= 0) return 0;
  if (n > (int)PANEL_MAX_GEAR) n = (int)PANEL_MAX_GEAR;
  for (i = 0; i < n; i++) {
    const JVal *e = jAt(g, i);
    gEnv.snap.gear[i].role = (uint8_t)jNum(jGet(e, "role"), 0.0);
    gEnv.snap.gear[i].state = (uint8_t)jNum(jGet(e, "state"), 0.0);
    gEnv.snap.gear[i].rxLevel = (uint8_t)jNum(jGet(e, "rxLevel"), 0.0);
    gEnv.snap.gear[i].txLevel = (uint8_t)jNum(jGet(e, "txLevel"), 0.0);
  }
  gEnv.snap.gearCount = (uint8_t)n;
  return 1;
}

/* -------------------------------------------------------------- goldens --- */

#define MAX_FRAMES 12

typedef struct {
  int rows;
  unsigned char px[MAX_FRAMES][PANEL_H][PANEL_W][3];
  float t[MAX_FRAMES];
  int count;
} Golden;

static Golden gShot;

/* Purpose: append the captured framebuffer as a golden frame. Input: t. */
static void shotPush(float t) {
  if (gShot.count >= MAX_FRAMES) return;
  memcpy(gShot.px[gShot.count], gCap, sizeof gCap);
  gShot.t[gShot.count] = t;
  gShot.count++;
}

/* Purpose: read a truthy environment flag. Input: name. Output: 0/1.
 * Set, non-empty and not an explicit negation. GitHub Actions sets CI=true;
 * some runners set CI=1; a developer who exported CI=false locally means it. */
static int envTrue(const char *name) {
  const char *v = getenv(name);
  if (!v || !*v) return 0;
  return strcmp(v, "0") && strcmp(v, "false") && strcmp(v, "FALSE") &&
         strcmp(v, "no") && strcmp(v, "off");
}

/* Purpose: build the golden file path. Input: id, buffer. Output: buffer. */
static const char *goldenPath(const char *id, char *buf, size_t cap) {
  snprintf(buf, cap, "%s/panel-%s.txt", GOLDEN_DIR, id);
  return buf;
}

/* Purpose: write the collected frames as a golden file. Input: id, note. */
static int goldenWrite(const char *id, const char *note) {
  char path[256];
  FILE *f = fopen(goldenPath(id, path, sizeof path), "w");
  if (!f) return 0;
  fprintf(f, "# panel golden %s — %s\n", id, note);
  fprintf(f, "# generated by status-panel/firmware/test/panelParityTest.c "
             "from fixtures/parity-fixture.json (PARITY_REGEN=1)\n");
  fprintf(f, "# %dx%d, one line per row, %d RRGGBB tokens per line, "
             "post-flush 0..255 per channel\n", PANEL_W, PANEL_H, PANEL_W);
  for (int i = 0; i < gShot.count; i++) {
    fprintf(f, "frame t=%.3f\n", (double)gShot.t[i]);
    for (int y = 0; y < PANEL_H; y++) {
      for (int x = 0; x < PANEL_W; x++)
        fprintf(f, "%s%02X%02X%02X", x ? " " : "", gShot.px[i][y][x][0],
                gShot.px[i][y][x][1], gShot.px[i][y][x][2]);
      fputc('\n', f);
    }
  }
  fclose(f);
  return 1;
}

/* Purpose: build the leg-path sidecar path. Input: id, buffer. Output: buffer. */
static const char *legsPath(const char *id, char *buf, size_t cap) {
  snprintf(buf, cap, "%s/panel-%s.legs.txt", GOLDEN_DIR, id);
  return buf;
}

/* Purpose: emit the leg-path coordinate set collected during an A1 render.
 * Input: id. Output: 1 on success.
 *
 * This exists purely so the JS parity diff can name the pixels it excuses.
 * Format: a "# " header, then one "x y" pair per line, ascending. */
static int legsWrite(const char *id) {
  char path[256];
  FILE *f = fopen(legsPath(id, path, sizeof path), "w");
  if (!f) return 0;
  fprintf(f, "# leg-path coordinates for %s, captured from the real a1Leg() by\n"
             "# palette identity (cQuiet) — see panelParityTest.c. One \"x y\" per\n"
             "# line. Consumed by tests/dashboard/test_panel_parity.js.\n", id);
  for (int y = 0; y < PANEL_H; y++)
    for (int x = 0; x < PANEL_W; x++)
      if (gLegPix[y][x]) fprintf(f, "%d %d\n", x, y);
  fclose(f);
  return 1;
}

/* Purpose: check the committed leg-path sidecar still matches what a1Leg()
 * paints. Input: id, message buffer. Output: 1 when identical. */
static int legsCompare(const char *id, char *msg, size_t cap) {
  char path[256];
  FILE *f = fopen(legsPath(id, path, sizeof path), "r");
  unsigned char seen[PANEL_H][PANEL_W];
  char line[128];
  int x, y, extra = 0, missing = 0;

  if (!f) {
    snprintf(msg, cap, "no leg sidecar at %.160s — run with PARITY_REGEN=1", path);
    return 0;
  }
  memset(seen, 0, sizeof seen);
  while (fgets(line, sizeof line, f)) {
    if (line[0] == '#' || line[0] == '\n') continue;
    if (sscanf(line, "%d %d", &x, &y) != 2) continue;
    if (x < 0 || x >= PANEL_W || y < 0 || y >= PANEL_H) { ++extra; continue; }
    seen[y][x] = 1u;
  }
  fclose(f);
  for (y = 0; y < PANEL_H; y++)
    for (x = 0; x < PANEL_W; x++) {
      if (gLegPix[y][x] && !seen[y][x]) ++missing;
      if (!gLegPix[y][x] && seen[y][x]) ++extra;
    }
  if (missing || extra) {
    snprintf(msg, cap, "leg path drifted: %d newly painted, %d no longer painted",
             missing, extra);
    return 0;
  }
  return 1;
}

/* Purpose: compare the collected frames against the committed golden.
 * Input: id, message buffer. Output: 1 when identical. */
static int goldenCompare(const char *id, char *msg, size_t cap) {
  char path[256];
  FILE *f = fopen(goldenPath(id, path, sizeof path), "r");
  char line[8 * PANEL_W + 64];
  int frame = 0;

  if (!f) {
    snprintf(msg, cap, "no golden at %.160s — run with PARITY_REGEN=1", path);
    return 0;
  }
  while (frame < gShot.count) {
    int y;
    /* Skip comments and find the next frame header. */
    do {
      if (!fgets(line, sizeof line, f)) {
        snprintf(msg, cap, "golden ended after %d of %d frames", frame,
                 gShot.count);
        fclose(f);
        return 0;
      }
    } while (line[0] == '#');
    if (strncmp(line, "frame", 5) != 0) {
      snprintf(msg, cap, "expected a frame header, got: %.40s", line);
      fclose(f);
      return 0;
    }
    for (y = 0; y < PANEL_H; y++) {
      char *p;
      int x;
      if (!fgets(line, sizeof line, f)) {
        snprintf(msg, cap, "golden truncated in frame %d row %d", frame, y);
        fclose(f);
        return 0;
      }
      p = line;
      for (x = 0; x < PANEL_W; x++) {
        unsigned r, g, b;
        while (*p == ' ') p++;
        if (sscanf(p, "%2x%2x%2x", &r, &g, &b) != 3) {
          snprintf(msg, cap, "bad token at frame %d (%d,%d)", frame, x, y);
          fclose(f);
          return 0;
        }
        p += 6;
        if (r != gShot.px[frame][y][x][0] || g != gShot.px[frame][y][x][1] ||
            b != gShot.px[frame][y][x][2]) {
          snprintf(msg, cap,
                   "frame %d pixel (%d,%d): golden %02X%02X%02X, rendered "
                   "%02X%02X%02X", frame, x, y, r, g, b,
                   gShot.px[frame][y][x][0], gShot.px[frame][y][x][1],
                   gShot.px[frame][y][x][2]);
          fclose(f);
          return 0;
        }
      }
    }
    frame++;
  }
  fclose(f);
  return 1;
}

/* ------------------------------------------------------------ the cases --- */

typedef enum {
  K_SCREEN,      /* one of the twelve renderers                    */
  K_INLAY,       /* A0 with the universal alert inlay on top       */
  K_HELP,        /* the Vol+ help overlay, one frame per screen    */
  K_LINKLOST,
  K_NODATA
} CaseKind;

typedef struct {
  const char *id;
  const char *note;
  CaseKind kind;
  int index;              /* screen index 0..11 for K_SCREEN */
  const char *gearVariant;/* NULL = the fixture's primary gear */
  int wantArmed;          /* T3 must be armed here: the fixture drives this
                             screen's bar into the warn/crit bands          */
  int wantScroll;         /* T1 must have a ticker band here                */
  int dumpLegs;           /* also emit the cQuiet leg-path sidecar for the JS
                             parity diff (A1 only)                           */
} Case;

static const PanelScreenFn kScreens[12] = {
  panelScreenA0, panelScreenA1, panelScreenA2,
  panelScreenB0, panelScreenB1, panelScreenB2,
  panelScreenC0, panelScreenC1, panelScreenC2,
  panelScreenD0, panelScreenD1, panelScreenD2
};
static const char *kScreenIds[12] = {
  "A0", "A1", "A2", "B0", "B1", "B2", "C0", "C1", "C2", "D0", "D1", "D2"
};

/* wantArmed / wantScroll are coverage claims, not geometry: they say "with THIS
 * fixture, this screen is supposed to paint a conditional cell / a ticker".
 * They fail if a future fixture edit makes T1 or T3 vacuous here. */
static const Case kCases[] = {
  { "A0", "state lattice",            K_SCREEN,   0, NULL, 1, 0, 0 },
  { "A1", "flow gates",               K_SCREEN,   1, NULL, 0, 0, 1 },
  { "A2", "MQ and SNMP",              K_SCREEN,   2, NULL, 0, 0, 0 },
  { "B0", "condition counts",         K_SCREEN,   3, NULL, 0, 0, 0 },
  { "B1", "throughput",               K_SCREEN,   4, NULL, 1, 1, 0 },
  { "B2", "load and latency",         K_SCREEN,   5, NULL, 0, 0, 0 },
  { "C0", "load distribution",        K_SCREEN,   6, NULL, 0, 0, 0 },
  { "C1", "live traces",              K_SCREEN,   7, NULL, 0, 0, 0 },
  { "C2", "pool load",                K_SCREEN,   8, NULL, 1, 0, 0 },
  { "D0", "reachability waterfall",   K_SCREEN,   9, NULL, 0, 0, 0 },
  { "D1", "pool bars",                K_SCREEN,  10, NULL, 0, 1, 0 },
  { "D2", "ambient field",            K_SCREEN,  11, NULL, 0, 0, 0 },
  { "A1-routerdown", "flow gates, router down (CONTRACT-AW §10 A-3)",
                                      K_SCREEN,   1, "routerDown", 0, 0, 1 },
  { "inlay",    "A0 under the universal alert inlay",
                                      K_INLAY,    0, NULL, 1, 1, 0 },
  { "help",     "Vol+ help overlay, all twelve screens",
                                      K_HELP,     0, NULL, 0, 0, 0 },
  { "linklost", "CONTRACT §4 link-lost plate",
                                      K_LINKLOST, 0, NULL, 0, 0, 0 },
  { "nodata",   "CONTRACT §9 zero-node fleet",
                                      K_NODATA,   0, NULL, 0, 0, 0 }
};
#define NCASES ((int)(sizeof kCases / sizeof kCases[0]))

/* Purpose: run one case's frames, collecting goldens and invariant evidence.
 * Input: the case. Output: none (assertions are recorded as it goes).
 *
 * Render ORDER is load-bearing and therefore fixed: A1 and D0 carry file-static
 * integrator state that persists between calls, so a case's output depends on
 * how many frames of that screen ran before it. Insert new cases at the END. */
static void runCase(const Case *c) {
  char name[160], msg[256];
  int t1Fails = 0, t2Fails = 0, t3Fails = 0;
  int maxArmed = 0, maxScrollRows = 0;
  char t1Note[160] = "", t2Note[160] = "", t3Note[160] = "";

  gShot.count = 0;
  /* The leg path is static across frames, so the union over the run is the same
   * set any single frame paints; taking the union just removes the assumption. */
  gCollectQuiet = c->dumpLegs;
  memset(gLegPix, 0, sizeof gLegPix);
  if (c->gearVariant && !useGearVariant(c->gearVariant)) {
    snprintf(name, sizeof name, "%s: gear variant '%s'", c->id, c->gearVariant);
    ok(0, name, "missing from the fixture");
    return;
  }

  int frames = (c->kind == K_HELP) ? 12 : FRAMES;
  for (int f = 0; f < frames; f++) {
    float t = (float)f * TICK_DT;
    int keep;

    obsReset();
    panelFbClear();
    switch (c->kind) {
      case K_SCREEN:   kScreens[c->index](&gEnv, t, TICK_DT); break;
      case K_INLAY:    panelScreenA0(&gEnv, t, TICK_DT);
                       panelInlay(&gEnv, t);
                       break;
      case K_HELP:     panelHelpOverlayDraw((unsigned)f, 3.0f); break;
      case K_LINKLOST: panelScreenLinkLost(t); break;
      case K_NODATA:   panelScreenNoData(t); break;
    }
    panelFbFlush();

    if (gArmed > maxArmed) maxArmed = gArmed;
    {
      int rows = 0;
      for (int i = 0; i < ROW_N; i++) rows += gRowTouch[TAG_SCROLL][i] ? 1 : 0;
      if (rows > maxScrollRows) maxScrollRows = rows;
    }

    /* T1 — the ticker band owns its rows outright. BIG is exempt by design:
     * the ticker scrolling across the BIG watermark is the intended look. */
    {
      int r = sharedRow(TAG_SCROLL, TAG_TEXT);
      if (r < 0) r = sharedRow(TAG_SCROLL, TAG_TEXTOVER);
      if (r >= 0 && t1Fails++ == 0)
        snprintf(t1Note, sizeof t1Note,
                 "small-font glyph shares row %d with the scrolling ticker", r);
    }
    /* T2 — nothing may be drawn into rows that do not exist. */
    if (gOverflow && t2Fails++ == 0)
      snprintf(t2Note, sizeof t2Note, "%s", gOverflowNote);
    /* T3 — a text halo must not eat a conditional cell painted before it. */
    if (gHaloHits && t3Fails++ == 0)
      snprintf(t3Note, sizeof t3Note, "%s", gHaloNote);
    /* T3, second leg: every surviving guarded cell must still read as its own
     * palette constant at the intended brightness, not merely as "lit". */
    for (int y = 0; y < PANEL_H && t3Fails == 0; y++) {
      for (int x = 0; x < PANEL_W; x++) {
        if (!gGuard[y][x]) continue;
        const float *col = gGuardCol[y][x];
        float b = gGuardB[y][x];
        for (int k = 0; k < 3; k++) {
          float want = col[k] * b;
          int wi = (int)(want + 0.5f);
          if (wi < 0) wi = 0;
          if (wi > 255) wi = 255;
          if (abs(wi - (int)gCap[y][x][k]) > 1) {
            if (t3Fails++ == 0)
              snprintf(t3Note, sizeof t3Note,
                       "%s cell at (%d,%d) flushed as %02X%02X%02X, expected "
                       "%s x%.2f", col == cCrit ? "crit" : "warn", x, y,
                       gCap[y][x][0], gCap[y][x][1], gCap[y][x][2],
                       col == cCrit ? "cCrit" : "cWarn", (double)b);
            break;
          }
        }
        if (t3Fails) break;
      }
    }

    keep = (c->kind == K_HELP) || f == SAMPLE_A || f == SAMPLE_B ||
           f == SAMPLE_C;
    if (keep) shotPush(t);
  }

  snprintf(name, sizeof name, "%s (%s): T1 ticker band is text-free", c->id,
           c->note);
  ok(t1Fails == 0, name, t1Note);
  snprintf(name, sizeof name, "%s: T2 no glyph outside rows 0..%d", c->id,
           PANEL_H - 1);
  ok(t2Fails == 0, name, t2Note);
  snprintf(name, sizeof name, "%s: T3 warn/crit cells survive the text passes",
           c->id);
  ok(t3Fails == 0, name, t3Note);

  if (c->wantArmed) {
    snprintf(name, sizeof name, "%s: T3 coverage — conditional cells painted",
             c->id);
    snprintf(msg, sizeof msg, "guard never armed; the fixture no longer drives "
                              "this screen into warn/crit");
    ok(maxArmed > 0, name, msg);
  }
  if (c->wantScroll) {
    snprintf(name, sizeof name, "%s: T1 coverage — a ticker band exists",
             c->id);
    snprintf(msg, sizeof msg, "no panelScroll pixels were painted");
    ok(maxScrollRows > 0, name, msg);
  }

  if (envTrue("PARITY_REGEN")) {
    snprintf(name, sizeof name, "%s: golden regenerated", c->id);
    ok(goldenWrite(c->id, c->note), name, "could not write the golden file");
    if (c->dumpLegs) {
      snprintf(name, sizeof name, "%s: leg path regenerated", c->id);
      ok(legsWrite(c->id), name, "could not write the leg sidecar");
    }
  } else {
    msg[0] = '\0';
    snprintf(name, sizeof name, "%s: golden framebuffers match", c->id);
    ok(goldenCompare(c->id, msg, sizeof msg), name, msg);
    if (c->dumpLegs) {
      msg[0] = '\0';
      snprintf(name, sizeof name, "%s: leg path matches the sidecar", c->id);
      ok(legsCompare(c->id, msg, sizeof msg), name, msg);
    }
  }
  if (c->dumpLegs) {
    int n = 0;
    for (int y = 0; y < PANEL_H; y++)
      for (int x = 0; x < PANEL_W; x++) n += gLegPix[y][x] ? 1 : 0;
    snprintf(name, sizeof name, "%s: leg path is non-empty", c->id);
    snprintf(msg, sizeof msg, "a1Leg() painted nothing; the JS exemption would "
                              "be vacuous");
    ok(n > 0, name, msg);
  }
  gCollectQuiet = 0;
}

/* PARITY_REGEN rewrites every committed golden. In CI that is not a
 * convenience, it is a hole: a real rendering regression would rewrite the
 * evidence and report success, and the diff nobody reads would land in the
 * branch. So the two flags together are an error, loudly, before anything
 * renders. Regenerating goldens is a local action whose diff a human reviews.
 * This is asserted by the CI-safety case in runCase()'s table only indirectly;
 * the direct check is the exit-2 path below, exercised by:
 *     CI=true PARITY_REGEN=1 ./panelParityTest   # must exit 2, goldens intact
 */
static int regenRefusedInCI(void) {
  if (!envTrue("PARITY_REGEN") || !envTrue("CI")) return 0;
  fprintf(stderr,
          "panelParityTest: REFUSING to regenerate goldens.\n"
          "  PARITY_REGEN and CI are both set. Regeneration overwrites the\n"
          "  committed framebuffers in %s, which is exactly the evidence a CI\n"
          "  run exists to check — a rendering regression would rewrite its own\n"
          "  expectation and pass.\n"
          "  Regenerate locally, read the diff, and commit it.\n",
          GOLDEN_DIR);
  return 1;
}

/* Purpose: assert the fixture parser rejects malformed input. Output: none.
 * The harness's whole claim is that both renderers read the SAME BYTES, which
 * rests on the C side parsing those bytes correctly. A parser that quietly
 * accepts a truncated object or a raw control byte can reshape the fixture
 * without anyone noticing, so the accept path is not the only one worth
 * testing. Each case must fail to parse AND leave no leak for ASAN to find. */
static void runJsonCorpus(void) {
  static const struct { const char *why; const char *text; size_t len; } bad[] = {
    { "raw control byte in a string",
      "{\"a\":\"x\ny\"}", 11 },
    { "embedded NUL in a string",
      "{\"a\":\"x\0y\"}", 11 },
    { "truncated object",
      "{\"a\":1,\"b\":", 11 },
    { "truncated array",
      "[1,2,", 5 },
    { "unterminated string",
      "{\"a\":\"oops}", 11 },
    { "trailing garbage after the root",
      "{\"a\":1} junk", 12 },
    { "lone high surrogate",
      "{\"a\":\"\\ud83d\"}", 14 },
    { "lone low surrogate",
      "{\"a\":\"\\udc00\"}", 14 },
    { "bad escape",
      "{\"a\":\"\\q\"}", 10 },
    { "missing colon",
      "{\"a\" 1}", 7 },
  };
  static const struct { const char *why; const char *text; size_t len; } good[] = {
    { "escaped control byte",   "{\"a\":\"x\\ny\"}", 12 },
    { "valid surrogate pair",   "{\"a\":\"\\ud83d\\ude00\"}", 20 },
    { "trailing whitespace",    "{\"a\":1}\n ", 9 },
  };
  char name[128], err[128];
  size_t i;

  for (i = 0; i < sizeof bad / sizeof bad[0]; i++) {
    JVal *v = jsonParseMem(bad[i].text, bad[i].len, err, sizeof err);
    snprintf(name, sizeof name, "json corpus: rejects %s", bad[i].why);
    ok(v == NULL, name, "parsed as valid");
    jsonFree(v);
  }
  for (i = 0; i < sizeof good / sizeof good[0]; i++) {
    JVal *v = jsonParseMem(good[i].text, good[i].len, err, sizeof err);
    snprintf(name, sizeof name, "json corpus: accepts %s", good[i].why);
    ok(v != NULL && jGet(v, "a") != NULL, name, err);
    jsonFree(v);
  }
}

int main(void) {
  int samples;

  if (regenRefusedInCI()) return 2;

  runJsonCorpus();

  samples = loadFixture();
  if (!samples) {
    printf("%d passed, %d failed\n", gPass, gFail);
    return 1;
  }
  ok(gEnv.haveData, "fixture: history applied to the real PanelEnv", "");
  {
    char note[96];
    snprintf(note, sizeof note, "%d samples, %d pools, %d gear", samples,
             gEnv.poolCount, (int)gEnv.snap.gearCount);
    ok(samples >= 2 && gEnv.poolCount > 0 && gEnv.snap.gearCount > 0,
       "fixture: shape is usable", note);
    printf("      %s, rate label \"%s\"\n", note, gEnv.gLabel);
  }

  for (int i = 0; i < NCASES; i++) runCase(&kCases[i]);

  ok(gOob == 0, "no pixel was flushed outside the 53x11 matrix", "");

  /* Every screen id must have a golden, so a renderer can never be added to
   * panelScreens.h and quietly skip the assertion floor. */
  for (int i = 0; i < 12; i++) {
    int found = 0;
    char name[96];
    for (int j = 0; j < NCASES; j++)
      if (kCases[j].kind == K_SCREEN && !strcmp(kCases[j].id, kScreenIds[i]))
        found = 1;
    snprintf(name, sizeof name, "screen %s has a golden case", kScreenIds[i]);
    ok(found, name, "");
  }

  jsonFree(gFixture);
  printf("%d passed, %d failed\n", gPass, gFail);
  return gFail ? 1 : 0;
}
