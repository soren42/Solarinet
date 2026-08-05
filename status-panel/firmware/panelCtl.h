/*
 * panelCtl.h — CONTROL consumption and STATE report scheduling.
 *
 * CONTRACT-CP.md §3/§6/§10. Two jobs, both pure decision logic:
 *
 *   1. Consume host CONTROL commands. Dedupe by cmdId (strictly ascending,
 *      cmdId <= lastCmdId ignored), validate kind/arg, and hand main.c back an
 *      ACTION to perform. panelCtl never touches hardware or firmware globals —
 *      main.c applies every action through the SAME code paths as the physical
 *      buttons, which is the whole point of the split.
 *   2. Decide WHEN a STATE frame goes out: on any reported field changing, and
 *      as a 30 s heartbeat.
 *
 * Split out of the tick for the same reason panelLink.c was (see its header):
 * dedupe over a wrapping-ish monotonic counter and change-detection against a
 * cadence are exactly the logic that reads correct and is not, and both are
 * reachable from a host unit test with a synthetic clock. No hardware
 * dependencies — plain C over a caller-supplied millisecond clock.
 */

#ifndef SOLARI_PANEL_CTL_H
#define SOLARI_PANEL_CTL_H

#include <stdbool.h>
#include <stdint.h>

/* CONTRACT-CP §3: "Emitted on ANY state change and every 30 s as heartbeat." */
#define PANEL_CTL_HEARTBEAT_MS 30000u

/* Floor between two change-triggered STATE frames.
 *
 * Without it, auto-brightness alone would spam the link: gAutoBrightSmoothed
 * moves every 40 ms tick, so brightnessPct ticks through whole percents as the
 * room light changes and every one of those is "a state change". 1 s bounds the
 * worst case to ~26 bytes/s while staying an order of magnitude inside the
 * contract's ≤7 s round-trip budget. A change seen during the window is NOT
 * lost — panelCtlPoll re-evaluates against live state every tick and emits as
 * soon as the window opens. The heartbeat is never subject to this floor.     */
#define PANEL_CTL_MIN_STATE_MS 1000u

/* What main.c should do with a consumed command. NONE means the command was
 * consumed (lastCmdId advanced) but nothing is to be applied — a duplicate, an
 * unknown kind, or an out-of-range arg. CONTRACT-CP §10: "lastCmdId = highest
 * cmdId CONSUMED by the firmware (applied, or rejected as invalid/unknown —
 * both consume)."                                                             */
typedef enum {
  PANEL_CTLACT_NONE = 0,
  PANEL_CTLACT_THEME,       /* arg 0-3   — pressTheme() semantics             */
  PANEL_CTLACT_SCREEN,      /* arg 0-2   — within the current theme           */
  PANEL_CTLACT_BRIGHTNESS,  /* arg 0-100 — latches manual, as LUX+/-          */
  PANEL_CTLACT_AUTOBRIGHT,  /* arg 0     — re-enable the light sensor         */
  PANEL_CTLACT_ACKALARM,    /* arg 0     — the any-button ack path            */
  PANEL_CTLACT_DWELL,       /* arg 3|6|30 seconds                             */
  PANEL_CTLACT_SLEEP        /* arg 0 wake / 1 sleep                           */
} PanelCtlAction;

/* The reportable panel state — the STATE payload's fields minus lastCmdId,
 * which panelCtl owns. Assembled by main.c from its globals each tick.        */
typedef struct {
  uint8_t  theme;           /* 0-3                                            */
  uint8_t  screen;          /* 0-2                                            */
  uint8_t  brightnessPct;   /* 0-100                                          */
  uint8_t  autoBright;      /* 0/1                                            */
  uint8_t  sleeping;        /* 0/1                                            */
  uint8_t  alarmArmed;      /* 0/1                                            */
  uint8_t  alarmAcked;      /* 0/1 — the CURRENT episode is acknowledged      */
  uint8_t  dwellSec;        /* 3|6|30; STATE byte 7, 0 = unreported           */
  uint32_t ackedEpisodeId;  /* 0 when nothing has ever been acked             */
} PanelCtlState;

typedef struct {
  uint32_t      lastCmdId;      /* highest cmdId CONSUMED; 0 at boot          */
  PanelCtlState reported;       /* the state last put on the wire             */
  uint32_t      reportedCmdId;  /* lastCmdId as of that frame                 */
  uint32_t      lastSentMs;
  bool          haveSent;
} PanelCtl;

/* panelCtlInit — boot state. lastCmdId starts at 0 BY DESIGN: CONTRACT-CP §10
 * accepts the reboot replay window (unconfirmed pending commands re-apply,
 * bounded by the server's 120 s expiry, and every kind is idempotent).
 * Input: ctl, current ms. Output: none.                                      */
void panelCtlInit(PanelCtl *ctl, uint32_t nowMs);

/* panelCtlConsume — dedupe and validate one decoded CONTROL command.
 * Input:  ctl, the frame's cmdId/kind/arg, out-param argOut (may be NULL)
 *         which receives the VALIDATED arg for the returned action.
 * Output: the action to apply, or PANEL_CTLACT_NONE.
 *
 * A command with cmdId <= lastCmdId is ignored SILENTLY and lastCmdId does not
 * move — that is what makes the daemon's every-poll re-serve free. Any other
 * command advances lastCmdId whether or not it applies, so an invalid or
 * unknown command still terminates server-side instead of re-serving until it
 * expires.                                                                    */
PanelCtlAction panelCtlConsume(PanelCtl *ctl, uint32_t cmdId, uint8_t kind,
                               uint8_t arg, uint8_t *argOut);

/* panelCtlPoll — should a STATE frame go out this tick?
 * Input:  ctl, the live state, current ms.
 * Output: true if the caller MUST now send a STATE frame carrying `cur` and
 *         panelCtlLastCmdId(); the call records it as sent, so a caller that
 *         returns true and does not send loses that report until the next
 *         change or the heartbeat.                                            */
bool panelCtlPoll(PanelCtl *ctl, const PanelCtlState *cur, uint32_t nowMs);

/* panelCtlForceReport — make the next panelCtlPoll report unconditionally,
 * bypassing both the change test and the rate floor.
 * Input: ctl. Output: none.
 *
 * For the host announcing itself (HELLOREQ). CONTRACT-CP §10's capability gate
 * withholds every queued command until the daemon has seen a STATE frame since
 * the current serial link-up — so after a DAEMON restart against an already-
 * booted panel, waiting for the 30 s heartbeat would stall the first command of
 * the session by up to half a minute for no reason. The panel already answers
 * HELLOREQ; answering with STATE as well costs 24 bytes.                      */
void panelCtlForceReport(PanelCtl *ctl);

/* panelCtlLastCmdId — the value to put in the STATE frame's lastCmdId field.
 * Input: ctl. Output: highest cmdId consumed.                                */
uint32_t panelCtlLastCmdId(const PanelCtl *ctl);

#endif /* SOLARI_PANEL_CTL_H */
