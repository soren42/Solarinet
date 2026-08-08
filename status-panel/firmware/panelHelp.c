/* panelHelp.c — hardware-free on-panel help state machine. */

#include "panelHelp.h"

void panelHelpInit(PanelHelp *help) {
  help->active = false;
  help->startT = 0.0f;
}

PanelHelpToggleResult panelHelpToggle(PanelHelp *help, float t,
                                      bool alarmArmed) {
  if (alarmArmed) return PANEL_HELP_IGNORED_ARMED;
  if (help->active) {
    help->active = false;
    return PANEL_HELP_DISMISSED;
  }
  help->active = true;
  help->startT = t;
  return PANEL_HELP_OPENED;
}

void panelHelpTick(PanelHelp *help, float t, bool alarmArmed) {
  if (!help->active) return;
  if (alarmArmed || t - help->startT >= PANEL_HELP_DURATION_SEC)
    help->active = false;
}

void panelHelpDismiss(PanelHelp *help) {
  help->active = false;
}
