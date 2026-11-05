/* panelSim.c — host simulator: renders the REAL firmware screens to image files.
 *
 * The panel's defects have repeatedly been invisible to green tests because
 * nobody looked at the lit pixels. This links the production renderers exactly
 * as panelParityTest.c does, replays a scene's history at the firmware's 25 Hz
 * and writes binary PPM frames that tools/panelsim/sheet.py turns into an
 * LED-look contact sheet. Not firmware: never linked into the panel binary.
 *
 * Usage: panelSim <scene.json> <outDir> [secondsAtWhichToCapture...]
 * Scene: {"history":[sample...]}, same sample shape as parity-fixture.json.  */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../panelScreens.h"
#include "../panelHelpOverlay.h"
#include "testJson.h"
#include "panelSceneExt.h"

static unsigned char gCap[PANEL_H][PANEL_W][3];

void panelHwSetPixel(int x, int y, unsigned char r, unsigned char g,
                     unsigned char b) {
  if (x < 0 || x >= PANEL_W || y < 0 || y >= PANEL_H) return;
  gCap[y][x][0] = r;
  gCap[y][x][1] = g;
  gCap[y][x][2] = b;
}

static uint8_t stateWord(const char *w) {
  if (!w) return PANEL_ST_UNKNOWN;
  if (!strcmp(w, "up") || !strcmp(w, "ok")) return PANEL_ST_OK;
  if (!strcmp(w, "degraded")) return PANEL_ST_DEGRADED;
  if (!strcmp(w, "down")) return PANEL_ST_DOWN;
  if (!strcmp(w, "maint")) return PANEL_ST_MAINT;
  return PANEL_ST_UNKNOWN;
}

static void copyStr(char *dst, size_t cap, const JVal *v) {
  const char *s = jStr(v, "");
  size_t n = strlen(s);
  if (n >= cap) n = cap - 1;
  memcpy(dst, s, n);
  dst[n] = '\0';
}

#define NUM(o, k) jNum(jGet((o), (k)), 0.0)

static void decodeSample(const JVal *s, PanelSnapshot *snap) {
  const JVal *o;
  int i, n;

  memset(snap, 0, sizeof *snap);
  snap->ts = (uint32_t)NUM(s, "ts");
  snap->seq = (uint16_t)NUM(s, "seq");
  snap->score = (uint16_t)NUM(s, "score");
  snap->alarmActive = (uint8_t)(jBool(jGet(s, "alarmActive"), 0) ? 1 : 0);
  snap->dataStale = (uint8_t)(jBool(jGet(s, "dataStale"), 0) ? 1 : 0);
  o = jGet(s, "stateRoll");
  snap->stateRoll[PANEL_ST_OK] = (uint8_t)NUM(o, "up");
  snap->stateRoll[PANEL_ST_DEGRADED] = (uint8_t)NUM(o, "degraded");
  snap->stateRoll[PANEL_ST_DOWN] = (uint8_t)NUM(o, "down");
  snap->stateRoll[PANEL_ST_UNKNOWN] = (uint8_t)NUM(o, "unknown");
  snap->stateRoll[PANEL_ST_MAINT] = (uint8_t)NUM(o, "maint");
  o = jGet(s, "alerts");
  snap->alertCounts[PANEL_SEV_INFO] = (uint8_t)NUM(o, "info");
  snap->alertCounts[PANEL_SEV_WARN] = (uint8_t)NUM(o, "warn");
  snap->alertCounts[PANEL_SEV_CRIT] = (uint8_t)NUM(o, "crit");
  snap->meanLoadPct = (uint8_t)NUM(s, "meanLoadPct");
  snap->rxKbps = (uint32_t)NUM(s, "rxKbps");
  snap->txKbps = (uint32_t)NUM(s, "txKbps");
  snap->rttTenthMs = (uint16_t)NUM(s, "rttTenthMs");
  snap->lossPermille = (uint16_t)NUM(s, "lossPermille");

  o = jGet(s, "pools");
  n = jLen(o);
  if (n > (int)PANEL_MAX_POOLS) n = (int)PANEL_MAX_POOLS;
  for (i = 0; i < n; i++) {
    const JVal *p = jAt(o, i);
    copyStr(snap->pools[i].name, sizeof snap->pools[i].name, jGet(p, "name"));
    snap->pools[i].tier = (uint8_t)NUM(p, "tier");
    snap->pools[i].ok = (uint8_t)NUM(p, "up");
    snap->pools[i].degraded = (uint8_t)NUM(p, "degraded");
    snap->pools[i].down = (uint8_t)NUM(p, "down");
    snap->pools[i].unknown = (uint8_t)NUM(p, "unknown");
    snap->pools[i].maint = (uint8_t)NUM(p, "maint");
    snap->pools[i].total = (uint8_t)NUM(p, "total");
    snap->pools[i].loadPct = (uint8_t)NUM(p, "loadPct");
  }
  snap->poolCount = (uint8_t)n;

  o = jGet(s, "systems");
  n = jLen(o);
  if (n > (int)PANEL_MAX_SYSTEMS) n = (int)PANEL_MAX_SYSTEMS;
  for (i = 0; i < n; i++) {
    const JVal *y = jAt(o, i);
    copyStr(snap->systems[i].name, sizeof snap->systems[i].name, jGet(y, "name"));
    snap->systems[i].state = stateWord(jStr(jGet(y, "state"), "unknown"));
    snap->systems[i].pool = (uint8_t)NUM(y, "pool");
    snap->systems[i].tier = (uint8_t)NUM(y, "tier");
    snap->systems[i].loadPct = (uint8_t)NUM(y, "loadPct");
  }
  snap->systemCount = (uint8_t)n;

  o = jGet(s, "topAlert");
  if (o && o->type == J_OBJ) {
    snap->hasTopAlert = 1u;
    snap->topAlert.alertId = (uint32_t)NUM(o, "alertId");
    snap->topAlert.episodeId = (uint32_t)NUM(o, "episodeId");
    snap->topAlert.severity = (uint8_t)NUM(o, "severity");
    copyStr(snap->topAlert.subject, sizeof snap->topAlert.subject, jGet(o, "subject"));
    copyStr(snap->topAlert.detail, sizeof snap->topAlert.detail, jGet(o, "detail"));
  }

  o = jGet(s, "gear");
  n = jLen(o);
  if (n > (int)PANEL_MAX_GEAR) n = (int)PANEL_MAX_GEAR;
  for (i = 0; i < n; i++) {
    const JVal *g = jAt(o, i);
    snap->gear[i].role = (uint8_t)NUM(g, "role");
    snap->gear[i].state = (uint8_t)NUM(g, "state");
    snap->gear[i].rxLevel = (uint8_t)NUM(g, "rxLevel");
    snap->gear[i].txLevel = (uint8_t)NUM(g, "txLevel");
  }
  snap->gearCount = (uint8_t)n;
  sceneDecodeExt(s, snap);
}

static void writePpm(const char *dir, const char *id, float t) {
  char path[512];
  FILE *f;
  snprintf(path, sizeof path, "%s/%s@%05d.ppm", dir, id, (int)(t * 1000.0f + 0.5f));
  f = fopen(path, "wb");
  if (!f) { perror(path); exit(1); }
  fprintf(f, "P6\n%d %d\n255\n", PANEL_W, PANEL_H);
  fwrite(gCap, 1, sizeof gCap, f);
  fclose(f);
}

typedef enum { SIM_SCREEN, SIM_INLAY, SIM_HELP, SIM_LINKLOST, SIM_NODATA, SIM_TITLE } SimKind;

typedef struct {
  const char *id;
  SimKind kind;
  PanelScreenFn fn;
  unsigned help;
} SimCase;

static const SimCase kCases[] = {
  { "A0", SIM_SCREEN, panelScreenA0, 0 },
  { "A1", SIM_SCREEN, panelScreenA1, 0 },
  { "A2", SIM_SCREEN, panelScreenA2, 0 },
  { "A3", SIM_SCREEN, panelScreenA3, 0 },
  { "B0", SIM_SCREEN, panelScreenB0, 0 },
  { "B1", SIM_SCREEN, panelScreenB1, 0 },
  { "B2", SIM_SCREEN, panelScreenB2, 0 },
  { "B3", SIM_SCREEN, panelScreenB3, 0 },
  { "C0", SIM_SCREEN, panelScreenC0, 0 },
  { "C1", SIM_SCREEN, panelScreenC1, 0 },
  { "C2", SIM_SCREEN, panelScreenC2, 0 },
  { "C3", SIM_SCREEN, panelScreenC3, 0 },
  { "D0", SIM_SCREEN, panelScreenD0, 0 },
  { "D1", SIM_SCREEN, panelScreenD1, 0 },
  { "D2", SIM_SCREEN, panelScreenD2, 0 },
  { "D3", SIM_SCREEN, panelScreenD3, 0 },
  { "TITLE", SIM_TITLE, panelScreenA0, 0 },
  { "INLAY", SIM_INLAY, panelScreenA0, 0 },
  { "HELP", SIM_HELP, NULL, 0 },
  { "LINKLOST", SIM_LINKLOST, panelScreenA0, 0 },
  { "NODATA", SIM_NODATA, NULL, 0 },
};

int main(int argc, char **argv) {
  static PanelEnv env;
  char err[192];
  JVal *root;
  const JVal *hist;
  float caps[8] = { 1.0f, 4.0f };
  int capN = 2, i, n;
  size_t c;

  if (argc < 3) {
    fprintf(stderr, "usage: %s scene.json outDir [seconds...]\n", argv[0]);
    return 2;
  }
  if (argc > 3) {
    capN = 0;
    for (i = 3; i < argc && capN < 8; i++) caps[capN++] = (float)atof(argv[i]);
  }
  root = jsonParseFile(argv[1], err, sizeof err);
  if (!root) { fprintf(stderr, "%s: %s\n", argv[1], err); return 1; }
  hist = jGet(root, "history");
  n = jLen(hist);
  panelEnvInit(&env);
  for (i = 0; i < n; i++) {
    static PanelSnapshot snap;
    decodeSample(jAt(hist, i), &snap);
    panelEnvApply(&env, &snap);
  }
  for (c = 0; c < sizeof kCases / sizeof kCases[0]; c++) {
    const SimCase *k = &kCases[c];
    int cap = 0, tick;
    for (tick = 0; cap < capN; tick++) {
      float t = (float)tick * 0.04f;
      panelFbClear();
      if (k->fn) k->fn(&env, t, 0.04f);
      if (k->kind == SIM_TITLE) panelTitleCard(11u, 0.5f);
      if (k->kind == SIM_INLAY) panelInlay(&env, t);
      if (k->kind == SIM_HELP) panelHelpOverlayDraw(0u, t);
      if (k->kind == SIM_LINKLOST) panelScreenLinkLost(t);
      if (k->kind == SIM_NODATA) panelScreenNoData(env.haveData, t);
      panelFbFlush();
      if (t + 0.001f >= caps[cap]) writePpm(argv[2], k->id, caps[cap++]);
    }
  }
  return 0;
}
