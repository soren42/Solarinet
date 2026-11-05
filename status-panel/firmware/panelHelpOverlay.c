/* panelHelpOverlay.c — font-path renderer for on-panel screen help. */

#include "panelFont.h"
#include "panelHelpOverlay.h"
#include "panelHw.h"

typedef struct {
  const char *line1;
  const char *line2;
} PanelHelpText;

/* Index is theme * 4 + screen, matching main.c's kScreens table. Each line is at
 * most 13 characters, the small font's limit, within [A-Z0-9 .:-]: help stands
 * still to be read (CONTRACT-V2 §2). */
static const PanelHelpText HELP_TEXTS[16] = {
  { "TILE IS HOST",  "FILL IS LOAD" },
  { "HOST LIST",     "WORST FIRST" },
  { "EACH POOL",     "HOSTS LOAD" },
  { "HOSTS UP",      "AND WORST NOW" },
  { "LINK TRAFFIC",  "DOTS ARE RATE" },
  { "INTERNET AND",  "LOCAL TRAFFIC" },
  { "BUSIEST HOSTS", "BY TRAFFIC" },
  { "NETWORK TREE",  "DOTS ARE RATE" },
  { "OPEN ALERTS",   "WORST FIRST" },
  { "OPEN FAULTS",   "BY KIND" },
  { "PING HISTORY",  "RED IS LOSS" },
  { "ALERTS 26 HRS", "NOW AT RIGHT" },
  { "ONE HOST",      "CPU MEM DISK" },
  { "UP IS CPU",     "DOWN IS MEM" },
  { "TRAFFIC CPU",   "AND PING" },
  { "AMBIENT FLEET", "RED IS FAULT" }
};

static void drawLine(int y, const char *line, PanelColor color) {
  panelText((PANEL_W - panelTextW(line)) / 2, y, line, color, 1.0f);
}

void panelHelpOverlayDraw(unsigned screenIndex, float t) {
  const PanelHelpText *text;
  (void)t;
  if (screenIndex >= 16u) return;
  text = &HELP_TEXTS[screenIndex];
  drawLine(0, text->line1, cInk);
  drawLine(6, text->line2, cAzure);
}
