/* panelHelpTest.c — host-side tests for the on-panel help state machine. */

#include <stdio.h>

#include "../panelHelp.h"

static int gFailures = 0;

static void check(bool ok, const char *what) {
  if (ok) printf("ok:   %s\n", what);
  else { printf("FAIL: %s\n", what); gFailures++; }
}

static void caseTriggerAndTimeout(void) {
  PanelHelp help;
  panelHelpInit(&help);
  check(!help.active, "case 1: init is inactive");
  check(panelHelpToggle(&help, 12.5f, false) == PANEL_HELP_OPENED &&
            help.active && help.startT == 12.5f,
        "case 1: VOL+ opens help at the supplied clock time");
  panelHelpTick(&help, 42.49f, false);
  check(help.active, "case 1: help remains open before 30 seconds");
  panelHelpTick(&help, 42.5f, false);
  check(!help.active, "case 1: help closes at the 30 second boundary");
}

static void caseAckWins(void) {
  PanelHelp help;
  panelHelpInit(&help);
  check(panelHelpToggle(&help, 4.0f, true) == PANEL_HELP_IGNORED_ARMED &&
            !help.active,
        "case 2: armed-alarm VOL+ never enters help");
}

static void caseSecondPressDismisses(void) {
  PanelHelp help;
  panelHelpInit(&help);
  (void)panelHelpToggle(&help, 1.0f, false);
  check(panelHelpToggle(&help, 2.0f, false) == PANEL_HELP_DISMISSED &&
            !help.active,
        "case 3: second VOL+ dismisses help only");
}

static void caseDismissPaths(void) {
  PanelHelp help;
  panelHelpInit(&help);
  (void)panelHelpToggle(&help, 1.0f, false);
  panelHelpDismiss(&help);
  check(!help.active, "case 4: another button dismisses before its normal action");
  (void)panelHelpToggle(&help, 2.0f, false);
  panelHelpTick(&help, 2.1f, true);
  check(!help.active, "case 4: alarm arming immediately dismisses help");
}

int main(void) {
  caseTriggerAndTimeout();
  caseAckWins();
  caseSecondPressDismisses();
  caseDismissPaths();
  if (gFailures) {
    printf("\n%d FAILURE(S)\n", gFailures);
    return 1;
  }
  printf("\nall panelHelp cases pass\n");
  return 0;
}
