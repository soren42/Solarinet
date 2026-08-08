/*
 * panelHelp.h — on-panel screen-help overlay state machine.
 *
 * This is intentionally hardware-free, like panelLink and panelCtl: its clock
 * is supplied by main.c in seconds, so all timing and alarm-priority edges can
 * be exercised by the plain-gcc host suite.
 */

#ifndef SOLARI_PANEL_HELP_H
#define SOLARI_PANEL_HELP_H

#include <stdbool.h>

#define PANEL_HELP_DURATION_SEC 30.0f

typedef struct {
  bool active;
  float startT;
} PanelHelp;

typedef enum {
  PANEL_HELP_IGNORED_ARMED = 0,
  PANEL_HELP_OPENED,
  PANEL_HELP_DISMISSED
} PanelHelpToggleResult;

/* panelHelpInit — establish the inactive boot state. */
void panelHelpInit(PanelHelp *help);

/* panelHelpToggle — process a VOL+ press at `t` seconds.
 *
 * An armed alarm is deliberately ignored here. main.c keeps its acknowledge
 * path ahead of this call, and the explicit result makes that precedence
 * independently testable. Otherwise, an inactive overlay opens and an active
 * overlay closes. */
PanelHelpToggleResult panelHelpToggle(PanelHelp *help, float t,
                                      bool alarmArmed);

/* panelHelpTick — close the overlay after 30 seconds or as soon as an alarm
 * becomes armed. */
void panelHelpTick(PanelHelp *help, float t, bool alarmArmed);

/* panelHelpDismiss — close an open overlay for a non-VOL+ button press. */
void panelHelpDismiss(PanelHelp *help);

#endif /* SOLARI_PANEL_HELP_H */
