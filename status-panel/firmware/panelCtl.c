/*
 * panelCtl.c — CONTROL consumption and STATE report scheduling.
 *
 * See panelCtl.h for why this is a separate translation unit.
 *
 * TWO RULES HERE ARE LOAD-BEARING AND EASY TO GET SUBTLY WRONG:
 *
 * 1. Consumption is not application. CONTRACT-CP §10 makes the panel's own
 *    STATE report the ONLY completion path for a queued command: the server
 *    marks a command applied when it sees lastCmdId >= cmdId. So a command that
 *    the firmware REJECTS — unknown kind, out-of-range arg — must still advance
 *    lastCmdId, or it re-serves every poll for 120 s and then surfaces to the
 *    operator as "expired/failed" when in truth the panel looked at it and said
 *    no. Rejection is a terminal outcome, and it has to look like one on the
 *    wire. Only a DUPLICATE (cmdId <= lastCmdId) leaves the counter alone.
 *
 * 2. STATE must be emitted when lastCmdId changes, not only when one of the
 *    visible fields does. Several commands legitimately change no visible state
 *    at all — ackAlarm with no alarm armed, setBrightness to the value already
 *    showing, setDwell to the dwell already set, any rejected command. (Plain
 *    setDwell no longer belongs on that list: the v1.1 CP amendment gave dwell
 *    STATE byte 7, so a dwell CHANGE is now a visible field change as well as a
 *    lastCmdId change — belt and braces, and the reason the list still matters
 *    is the other three.) If those did not trigger a report, the server would
 *    lastCmdId advance, the command would sit pending for the full 120 s and
 *    then expire, and the page would show a failure for a command the panel
 *    handled correctly. This widens the assignment's "any of the 7 state
 *    fields" to "any field on the wire"; it is recorded as a deliberate
 *    deviation in RETURN-CP3.md because §10 requires it.
 */

#include "panelCtl.h"

#include "../protocol.h"

/* ageMs — signed age of a timestamp, wrap-safe and future-safe. Same reasoning
 * as panelLink.c: an unsigned subtraction turns a timestamp one millisecond in
 * the future into 4294967295 ms of age, and this clock wraps every 49.7 days.
 * Input:  now, then (uint32 millisecond counters).
 * Output: milliseconds elapsed, negative if `then` is in the future.          */
static int32_t ageMs(uint32_t now, uint32_t then) {
  return (int32_t)(now - then);
}

/* statesEqual — field-by-field compare of two reports.
 * Field-by-field and not memcmp: PanelCtlState has padding before its uint32,
 * and padding bytes are indeterminate. A memcmp here would emit spurious STATE
 * frames, or worse, suppress real ones.
 * Input:  two states. Output: true if every field matches.                    */
static bool statesEqual(const PanelCtlState *a, const PanelCtlState *b) {
  return a->theme          == b->theme &&
         a->screen         == b->screen &&
         a->brightnessPct  == b->brightnessPct &&
         a->autoBright     == b->autoBright &&
         a->sleeping       == b->sleeping &&
         a->alarmArmed     == b->alarmArmed &&
         a->alarmAcked     == b->alarmAcked &&
         a->dwellSec       == b->dwellSec &&
         a->ackedEpisodeId == b->ackedEpisodeId;
}

void panelCtlInit(PanelCtl *ctl, uint32_t nowMs) {
  ctl->lastCmdId     = 0u;
  ctl->reportedCmdId = 0u;
  ctl->lastSentMs    = nowMs;
  ctl->haveSent      = false;
  ctl->reported.theme          = 0u;
  ctl->reported.screen         = 0u;
  ctl->reported.brightnessPct  = 0u;
  ctl->reported.autoBright     = 0u;
  ctl->reported.sleeping       = 0u;
  ctl->reported.alarmArmed     = 0u;
  ctl->reported.alarmAcked     = 0u;
  ctl->reported.dwellSec       = 0u;
  ctl->reported.ackedEpisodeId = 0u;
}

void panelCtlForceReport(PanelCtl *ctl) {
  /* Reuses the boot path: haveSent false means "report now, whatever the
   * clock and whatever the state", which is exactly the semantics wanted. */
  ctl->haveSent = false;
}

uint32_t panelCtlLastCmdId(const PanelCtl *ctl) {
  return ctl->lastCmdId;
}

PanelCtlAction panelCtlConsume(PanelCtl *ctl, uint32_t cmdId, uint8_t kind,
                               uint8_t arg, uint8_t *argOut) {
  uint8_t out = 0u;
  PanelCtlAction act = PANEL_CTLACT_NONE;

  if (argOut) *argOut = 0u;

  /* Strictly ascending. cmdId 0 is never valid (the server's AUTO_INCREMENT
   * starts at 1 and 0 is the sentinel for "nothing consumed"), which the
   * <= comparison already covers at lastCmdId == 0. */
  if (cmdId <= ctl->lastCmdId) return PANEL_CTLACT_NONE;

  /* Consumed from here on, whatever the verdict below. */
  ctl->lastCmdId = cmdId;

  switch (kind) {
    case PANEL_CTL_SETTHEME:
      if (arg <= 3u) { act = PANEL_CTLACT_THEME; out = arg; }
      break;
    case PANEL_CTL_SETSCREEN:
      if (arg <= 2u) { act = PANEL_CTLACT_SCREEN; out = arg; }
      break;
    case PANEL_CTL_BRIGHTNESS:
      if (arg <= 100u) { act = PANEL_CTLACT_BRIGHTNESS; out = arg; }
      break;
    case PANEL_CTL_AUTOBRIGHT:
      act = PANEL_CTLACT_AUTOBRIGHT;   /* arg ignored per protocol.h */
      break;
    case PANEL_CTL_ACKALARM:
      act = PANEL_CTLACT_ACKALARM;     /* arg ignored per protocol.h */
      break;
    case PANEL_CTL_SETDWELL:
      /* DESIGN-BRIEF: the mockup offers 3 s / 6 s / 30 s and nothing else.
       * Anything else is consumed and dropped rather than clamped — clamping
       * would report success for a dwell the operator never chose. */
      if (arg == 3u || arg == 6u || arg == 30u) {
        act = PANEL_CTLACT_DWELL; out = arg;
      }
      break;
    case PANEL_CTL_SLEEP:
      if (arg <= 1u) { act = PANEL_CTLACT_SLEEP; out = arg; }
      break;
    default:
      break;   /* unknown kind: consumed, not applied (protocol.h) */
  }

  if (argOut) *argOut = out;
  return act;
}

bool panelCtlPoll(PanelCtl *ctl, const PanelCtlState *cur, uint32_t nowMs) {
  bool changed;
  int32_t since;

  if (!ctl->haveSent) {
    /* First report after boot goes out immediately: it is what tells the
     * daemon the firmware speaks CONTROL/STATE at all, and until it arrives
     * CONTRACT-CP §10's capability gate withholds every queued command. */
    ctl->reported      = *cur;
    ctl->reportedCmdId = ctl->lastCmdId;
    ctl->lastSentMs    = nowMs;
    ctl->haveSent      = true;
    return true;
  }

  since   = ageMs(nowMs, ctl->lastSentMs);
  changed = !statesEqual(cur, &ctl->reported) ||
            ctl->lastCmdId != ctl->reportedCmdId;

  if (since < (int32_t)PANEL_CTL_HEARTBEAT_MS &&
      !(changed && since >= (int32_t)PANEL_CTL_MIN_STATE_MS)) {
    return false;
  }

  ctl->reported      = *cur;
  ctl->reportedCmdId = ctl->lastCmdId;
  ctl->lastSentMs    = nowMs;
  return true;
}
