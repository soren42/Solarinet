/* panelScreensATest.c — host checks for the A1 flow-gates renderer.
 *
 * Promoted from the review round's ASAN probe (REVIEW-AW M3/M4): the
 * particle source-selection chain read hubs[-2] whenever a role band was
 * empty. Build under -fsanitize=address,undefined so a regression is a
 * loud crash, not a wrong pixel. Every case drives 25 frames.            */
#include <stdio.h>
#include <string.h>

#include "../panelScreens.h"

static int gFailures = 0;

/* Minimal hw/env stubs — the renderer only needs a framebuffer sink. */
void panelScreenNoData(float t) { (void)t; }
void panelScreenLinkLost(float t) { (void)t; }
void panelHwSetPixel(int x, int y, unsigned char r, unsigned char g,
                     unsigned char b) {
  (void)r; (void)g; (void)b;
  if (x < 0 || x >= 53 || y < 0 || y >= 11) {
    printf("FAIL: pixel out of bounds (%d,%d)\n", x, y);
    ++gFailures;
  }
}

static PanelEnv env;

/* Purpose: reset env and add one gear entry. Input: role/state. Output: none. */
static void addGear(unsigned char role, unsigned char state) {
  unsigned char i = env.snap.gearCount++;
  env.snap.gear[i].role = role;
  env.snap.gear[i].state = state;
  env.snap.gear[i].rxLevel = 4u;
  env.snap.gear[i].txLevel = 4u;
}

/* Purpose: run one named A1 case for 25 frames. Input: label. Output: none. */
static void runCase(const char *label) {
  int f;
  for (f = 0; f < 25; ++f) {
    panelFbClear();
    panelScreenA1(&env, (float)f * 0.04f, 0.04f);
  }
  printf("ok:   A1 renderer: %s\n", label);
}

static void reset(void) {
  panelEnvInit(&env);
  env.haveData = true;
  env.snap.gearCount = 0u;
}

int main(void) {
  /* M3 case 1: APs + switch + router, NO hubs (the ASAN-proven crash). */
  reset();
  addGear(3u, 1u); addGear(3u, 1u); addGear(1u, 1u); addGear(0u, 1u);
  runCase("APs+switch+router, no hubs");

  /* M3 case 2: APs + hubs + router, NO switch. */
  reset();
  addGear(3u, 1u); addGear(3u, 1u); addGear(2u, 1u); addGear(2u, 1u); addGear(0u, 1u);
  runCase("APs+hubs+router, no switch");

  /* Full real fleet: router, switch, 3 hubs, 3 APs, wanBackup. */
  reset();
  addGear(0u, 1u); addGear(1u, 1u);
  addGear(2u, 1u); addGear(2u, 1u); addGear(2u, 1u);
  addGear(3u, 1u); addGear(3u, 1u); addGear(3u, 1u);
  addGear(4u, 1u);
  runCase("full 9-device fleet");

  /* Degenerates. */
  reset(); addGear(3u, 1u); addGear(3u, 1u); runCase("APs only");
  reset(); addGear(0u, 1u); runCase("router only");
  reset(); addGear(0u, 0u); addGear(4u, 1u);
  runCase("router down + wanBackup (A-3 inherit path)");
  reset(); runCase("gearCount 0 (no-data path)");

  /* All states over the full fleet incl. degraded/broken rendering. */
  reset();
  addGear(0u, 2u); addGear(1u, 0u);
  addGear(2u, 1u); addGear(2u, 0u); addGear(2u, 2u);
  addGear(3u, 0u); addGear(3u, 2u); addGear(3u, 1u);
  addGear(4u, 0u);
  runCase("mixed states fleet");

  if (gFailures != 0) { printf("%d failures\n", gFailures); return 1; }
  return 0;
}
