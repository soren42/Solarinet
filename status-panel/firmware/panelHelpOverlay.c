/* panelHelpOverlay.c — font-path renderer for on-panel screen help. */

#include "panelFont.h"
#include "panelHelpOverlay.h"
#include "panelHw.h"

typedef struct {
  const char *line1;
  const char *line2;
} PanelHelpText;

/* Index is theme * 3 + screen, matching main.c's kScreens table. All text is
 * intentionally within the restricted HELP_TEXTS charset: [A-Z0-9 .:-]. */
static const PanelHelpText HELP_TEXTS[12] = {
  { "STATE LATTICE",       "CELL LOAD STATE" },
  { "NET FLOW AP-HUB",     "SW-RTR RED DOWN" },
  { "LANES LOSS STALE",    "UNKNOWN DEGRADED" },
  { "UP DOWN COUNTS",      "DEG MNT TIER" },
  { "LINK UTILIZATION",    "TICKER TRAFFIC" },
  { "MEAN LOAD P95 RTT",  "LOSS HOT COUNT" },
  { "LOAD HISTOGRAM",      "RIGHT THROUGHPUT" },
  { "TRACES THRU CPU",     "RTT TOP TO BOTTOM" },
  { "POOL LOAD BARS",      "TIER 0 MARKER" },
  { "PROBE WATERFALL",     "RED LOSS AMBER SLOW" },
  { "POOL SIZE BARS",      "ALERT TICKER" },
  { "AMBIENT LOAD FIELD",  "SYS LOAD RATE TEXT" }
};

static void drawLine(float t, int y, const char *line, PanelColor color) {
  int width = panelTextW(line);
  if (width + 2 > PANEL_W) {
    panelScroll(t, y, line, color, 0.65f, 11.0f);
  } else {
    panelText((PANEL_W - width) / 2, y, line, color, 0.65f);
  }
}

void panelHelpOverlayDraw(unsigned screenIndex, float t) {
  const PanelHelpText *text;
  if (screenIndex >= 12u) return;
  text = &HELP_TEXTS[screenIndex];
  drawLine(t, 1, text->line1, cInk);
  drawLine(t, 6, text->line2, cAzure);
}
