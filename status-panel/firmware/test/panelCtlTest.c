/*
 * panelCtlTest.c — host-side unit test for CONTROL consumption and STATE
 * cadence (CONTRACT-CP.md §3/§6/§10).
 *
 * Same doctrine as panelLinkTest.c: this is pure state-machine logic over a
 * synthetic clock, so every defect in it is reachable without a board and none
 * of them has any business being found on one. The cases here are deliberately
 * unkind — the command queue's ONLY completion signal is lastCmdId coming back
 * in a STATE frame, so a bug that fails to advance the counter, or fails to
 * report that it advanced, does not look like a firmware fault at all: it looks
 * like the operator's click quietly expiring 120 s later.
 *
 * The last group drives real frames through the real protocol.c parser. That is
 * where a CP2-side omission shows up, and it is meant to be loud.
 *
 * Build and run:   make -C firmware/test
 * Exit code 0 = all cases pass. Any failure prints the case and returns 1.
 */

#include <stdio.h>
#include <string.h>

#include "../panelCtl.h"
#include "../../protocol.h"

static int gFailures = 0;

/* check — record and report one assertion.
 * Input: condition, label. Output: none (increments gFailures).               */
static void check(bool ok, const char *what) {
  if (!ok) {
    printf("FAIL: %s\n", what);
    gFailures++;
  } else {
    printf("ok:   %s\n", what);
  }
}

/* baseState — a plausible resting panel: theme D screen 1, auto-brightness on.
 * Input: none. Output: the state struct.                                      */
static PanelCtlState baseState(void) {
  PanelCtlState s;
  s.theme = 3u; s.screen = 1u; s.brightnessPct = 85u; s.autoBright = 1u;
  s.sleeping = 0u; s.alarmArmed = 0u; s.alarmAcked = 0u; s.ackedEpisodeId = 0u;
  s.dwellSec = 6u;   /* DESIGN-BRIEF shipped default */
  return s;
}

/*
 * Case 1 — every valid kind maps to its action and its argument survives.
 */
static void caseValidKinds(void) {
  PanelCtl ctl;
  uint8_t  arg = 0xFFu;
  panelCtlInit(&ctl, 0u);

  check(panelCtlConsume(&ctl, 1u, PANEL_CTL_SETTHEME, 2u, &arg) ==
            PANEL_CTLACT_THEME && arg == 2u,
        "case 1: setTheme(2) -> THEME action, arg preserved");
  check(panelCtlConsume(&ctl, 2u, PANEL_CTL_SETSCREEN, 0u, &arg) ==
            PANEL_CTLACT_SCREEN && arg == 0u,
        "case 1: setScreen(0) -> SCREEN action");
  check(panelCtlConsume(&ctl, 3u, PANEL_CTL_BRIGHTNESS, 100u, &arg) ==
            PANEL_CTLACT_BRIGHTNESS && arg == 100u,
        "case 1: setBrightness(100) -> BRIGHTNESS action at the boundary");
  check(panelCtlConsume(&ctl, 4u, PANEL_CTL_AUTOBRIGHT, 77u, &arg) ==
            PANEL_CTLACT_AUTOBRIGHT,
        "case 1: autoBrightness applies with the arg ignored, not rejected");
  check(panelCtlConsume(&ctl, 5u, PANEL_CTL_ACKALARM, 42u, &arg) ==
            PANEL_CTLACT_ACKALARM,
        "case 1: ackAlarm applies with the arg ignored");
  check(panelCtlConsume(&ctl, 6u, PANEL_CTL_SETDWELL, 30u, &arg) ==
            PANEL_CTLACT_DWELL && arg == 30u,
        "case 1: setDwell(30) -> DWELL action");
  check(panelCtlConsume(&ctl, 7u, PANEL_CTL_SLEEP, 1u, &arg) ==
            PANEL_CTLACT_SLEEP && arg == 1u,
        "case 1: sleep(1) -> SLEEP action");
  check(panelCtlConsume(&ctl, 8u, PANEL_CTL_SLEEP, 0u, &arg) ==
            PANEL_CTLACT_SLEEP && arg == 0u,
        "case 1: sleep(0) is wake, NOT a toggle");
  check(panelCtlLastCmdId(&ctl) == 8u,
        "case 1: lastCmdId tracks the highest consumed");
}

/*
 * Case 2 — invalid args and unknown kinds are CONSUMED but not applied.
 *
 * This is the case that matters most and is the easiest to get wrong by
 * "helpfully" rejecting the whole frame. CONTRACT-CP §10: a rejected command is
 * terminal, so lastCmdId MUST advance. If it does not, the server keeps
 * re-serving it for 120 s and then tells the operator it expired.
 */
static void caseRejectedStillConsumes(void) {
  PanelCtl ctl;
  panelCtlInit(&ctl, 0u);

  check(panelCtlConsume(&ctl, 10u, PANEL_CTL_SETTHEME, 4u, NULL) ==
            PANEL_CTLACT_NONE && panelCtlLastCmdId(&ctl) == 10u,
        "case 2: setTheme(4) is rejected AND consumed");
  check(panelCtlConsume(&ctl, 11u, PANEL_CTL_SETSCREEN, 3u, NULL) ==
            PANEL_CTLACT_NONE && panelCtlLastCmdId(&ctl) == 11u,
        "case 2: setScreen(3) is rejected AND consumed");
  check(panelCtlConsume(&ctl, 12u, PANEL_CTL_BRIGHTNESS, 101u, NULL) ==
            PANEL_CTLACT_NONE && panelCtlLastCmdId(&ctl) == 12u,
        "case 2: setBrightness(101) is rejected AND consumed");
  check(panelCtlConsume(&ctl, 13u, PANEL_CTL_SLEEP, 2u, NULL) ==
            PANEL_CTLACT_NONE && panelCtlLastCmdId(&ctl) == 13u,
        "case 2: sleep(2) is rejected AND consumed");
  check(panelCtlConsume(&ctl, 14u, 0x7Fu, 0u, NULL) == PANEL_CTLACT_NONE &&
            panelCtlLastCmdId(&ctl) == 14u,
        "case 2: unknown kind 0x7F is consumed, never desyncs the queue");
  check(panelCtlConsume(&ctl, 15u, 0u, 0u, NULL) == PANEL_CTLACT_NONE &&
            panelCtlLastCmdId(&ctl) == 15u,
        "case 2: kind 0 (not a PanelControlKind) is consumed");
}

/*
 * Case 3 — setDwell takes exactly 3, 6 and 30 and nothing near them.
 * Clamping would report success for a dwell nobody chose.
 */
static void caseDwellValidation(void) {
  PanelCtl ctl;
  uint8_t  arg = 0;
  uint32_t id = 1u;
  panelCtlInit(&ctl, 0u);

  check(panelCtlConsume(&ctl, id++, PANEL_CTL_SETDWELL, 3u, &arg) ==
            PANEL_CTLACT_DWELL && arg == 3u, "case 3: dwell 3 accepted");
  check(panelCtlConsume(&ctl, id++, PANEL_CTL_SETDWELL, 6u, &arg) ==
            PANEL_CTLACT_DWELL && arg == 6u, "case 3: dwell 6 accepted");
  check(panelCtlConsume(&ctl, id++, PANEL_CTL_SETDWELL, 30u, &arg) ==
            PANEL_CTLACT_DWELL && arg == 30u, "case 3: dwell 30 accepted");

  check(panelCtlConsume(&ctl, id++, PANEL_CTL_SETDWELL, 0u, NULL) ==
            PANEL_CTLACT_NONE, "case 3: dwell 0 rejected");
  check(panelCtlConsume(&ctl, id++, PANEL_CTL_SETDWELL, 5u, NULL) ==
            PANEL_CTLACT_NONE, "case 3: dwell 5 rejected, not rounded to 6");
  check(panelCtlConsume(&ctl, id++, PANEL_CTL_SETDWELL, 31u, NULL) ==
            PANEL_CTLACT_NONE, "case 3: dwell 31 rejected, not clamped to 30");
  check(panelCtlConsume(&ctl, id++, PANEL_CTL_SETDWELL, 255u, NULL) ==
            PANEL_CTLACT_NONE, "case 3: dwell 255 rejected");
  check(panelCtlLastCmdId(&ctl) == id - 1u,
        "case 3: every rejected dwell still advanced lastCmdId");
}

/*
 * Case 4 — dedupe and strict ascent.
 *
 * The daemon re-serves the entire pending queue on EVERY poll (§10), so the
 * firmware sees the same cmdId repeatedly by design; that must be free. And a
 * duplicate must not move lastCmdId, or a re-served command would look
 * confirmed while the panel had already refused it once.
 */
static void caseDedupeAndAscent(void) {
  PanelCtl ctl;
  panelCtlInit(&ctl, 0u);

  check(panelCtlConsume(&ctl, 5u, PANEL_CTL_SETTHEME, 1u, NULL) ==
            PANEL_CTLACT_THEME, "case 4: cmdId 5 applies");
  check(panelCtlConsume(&ctl, 5u, PANEL_CTL_SETTHEME, 1u, NULL) ==
            PANEL_CTLACT_NONE, "case 4: the same cmdId re-served does nothing");
  check(panelCtlConsume(&ctl, 4u, PANEL_CTL_SETTHEME, 2u, NULL) ==
            PANEL_CTLACT_NONE, "case 4: a lower cmdId is ignored");
  check(panelCtlConsume(&ctl, 0u, PANEL_CTL_SETTHEME, 2u, NULL) ==
            PANEL_CTLACT_NONE, "case 4: cmdId 0 is never valid");
  check(panelCtlLastCmdId(&ctl) == 5u,
        "case 4: ignored commands leave lastCmdId untouched");

  /* AUTO_INCREMENT leaves gaps (rolled-back inserts, other rows); a gap must
   * not be mistaken for an out-of-order command. */
  check(panelCtlConsume(&ctl, 900u, PANEL_CTL_SETTHEME, 0u, NULL) ==
            PANEL_CTLACT_THEME, "case 4: a large forward jump applies");
  check(panelCtlLastCmdId(&ctl) == 900u, "case 4: lastCmdId follows the jump");

  /* Re-serving a whole batch: only the ones above lastCmdId do anything. */
  int applied = 0;
  for (uint32_t id = 898u; id <= 903u; id++) {
    if (panelCtlConsume(&ctl, id, PANEL_CTL_SETSCREEN, 1u, NULL) !=
        PANEL_CTLACT_NONE) applied++;
  }
  check(applied == 3, "case 4: a re-served batch applies only 901,902,903");
  check(panelCtlLastCmdId(&ctl) == 903u, "case 4: batch left lastCmdId at 903");
}

/*
 * Case 5 — reboot replay, the residual CONTRACT-CP §10 accepts explicitly.
 * lastCmdId resets to 0, so unconfirmed pending commands re-apply. This asserts
 * the accepted behaviour rather than a fix, so that a future change which
 * silently persists lastCmdId shows up as a contract change and not a "fix".
 */
static void caseRebootReplay(void) {
  PanelCtl ctl;
  panelCtlInit(&ctl, 0u);

  check(panelCtlConsume(&ctl, 400u, PANEL_CTL_SETTHEME, 1u, NULL) ==
            PANEL_CTLACT_THEME, "case 5: pre-reboot command applies");

  panelCtlInit(&ctl, 1000u);   /* reboot */
  check(panelCtlLastCmdId(&ctl) == 0u, "case 5: lastCmdId is 0 after reboot");
  check(panelCtlConsume(&ctl, 400u, PANEL_CTL_SETTHEME, 1u, NULL) ==
            PANEL_CTLACT_THEME,
        "case 5: the re-served command re-applies (accepted replay window)");
}

/*
 * Case 6 — STATE emission on change, with the rate floor honoured and the
 * pending change never dropped.
 */
static void caseStateOnChange(void) {
  PanelCtl ctl;
  PanelCtlState st = baseState();
  panelCtlInit(&ctl, 0u);

  check(panelCtlPoll(&ctl, &st, 0u),
        "case 6: the first poll after boot always reports");
  check(!panelCtlPoll(&ctl, &st, 40u), "case 6: an unchanged tick is silent");
  check(!panelCtlPoll(&ctl, &st, 5000u),
        "case 6: still silent 5 s in with nothing changed");

  st.theme = 1u;
  check(panelCtlPoll(&ctl, &st, 5001u),
        "case 6: a theme change reports at once (floor already elapsed)");

  /* A second change inside the floor waits, and is NOT lost. */
  st.screen = 2u;
  check(!panelCtlPoll(&ctl, &st, 5100u),
        "case 6: a change inside the 1 s floor is withheld");
  check(!panelCtlPoll(&ctl, &st, 5999u), "case 6: still withheld at 998 ms");
  check(panelCtlPoll(&ctl, &st, 6001u),
        "case 6: the withheld change reports as soon as the floor opens");
  check(!panelCtlPoll(&ctl, &st, 7500u),
        "case 6: and is then genuinely settled, not re-reported");
}

/*
 * Case 7 — lastCmdId alone must trigger a report.
 *
 * setDwell, a rejected command, an ack with no alarm armed and a brightness set
 * to the value already showing all leave the seven visible fields identical.
 * lastCmdId is the only evidence the panel handled them, and STATE is the only
 * channel that carries it, so if this does not emit, the operator watches a
 * correctly-handled command expire.
 */
static void caseLastCmdIdTriggersReport(void) {
  PanelCtl ctl;
  PanelCtlState st = baseState();
  panelCtlInit(&ctl, 0u);

  check(panelCtlPoll(&ctl, &st, 0u), "case 7: initial report");
  check(!panelCtlPoll(&ctl, &st, 2000u), "case 7: quiet with nothing changed");

  /* A dwell change: nothing in `st` moves. */
  check(panelCtlConsume(&ctl, 1u, PANEL_CTL_SETDWELL, 30u, NULL) ==
            PANEL_CTLACT_DWELL, "case 7: setDwell applies");
  check(panelCtlPoll(&ctl, &st, 2040u),
        "case 7: setDwell is reported via lastCmdId despite no field change");
  check(panelCtlLastCmdId(&ctl) == 1u, "case 7: the report carries cmdId 1");

  /* A rejected command: also nothing in `st` moves. */
  check(panelCtlConsume(&ctl, 2u, 0x55u, 0u, NULL) == PANEL_CTLACT_NONE,
        "case 7: unknown kind consumed");
  check(panelCtlPoll(&ctl, &st, 4000u),
        "case 7: a REJECTED command is reported too, so it can go terminal");

  /* An ignored duplicate is NOT news and must not generate traffic. */
  check(panelCtlConsume(&ctl, 2u, 0x55u, 0u, NULL) == PANEL_CTLACT_NONE,
        "case 7: duplicate ignored");
  check(!panelCtlPoll(&ctl, &st, 6000u),
        "case 7: an ignored duplicate produces no STATE frame");
}

/*
 * Case 8 — the 30 s heartbeat, and its independence from the rate floor.
 */
static void caseHeartbeat(void) {
  PanelCtl ctl;
  PanelCtlState st = baseState();
  panelCtlInit(&ctl, 100000u);

  check(panelCtlPoll(&ctl, &st, 100000u), "case 8: initial report");
  check(!panelCtlPoll(&ctl, &st, 100000u + 29999u),
        "case 8: silent at 29.999 s");
  check(panelCtlPoll(&ctl, &st, 100000u + 30000u),
        "case 8: heartbeat fires at exactly 30 s");
  check(!panelCtlPoll(&ctl, &st, 100000u + 30040u),
        "case 8: the heartbeat re-arms rather than repeating every tick");
  check(panelCtlPoll(&ctl, &st, 100000u + 60000u),
        "case 8: the next heartbeat is 30 s after the last report");
}

/*
 * Case 9 — auto-brightness must not turn the link into a firehose.
 *
 * gAutoBrightSmoothed moves every 40 ms tick, so brightnessPct genuinely
 * changes several times a second in a room whose light is changing. Ten minutes
 * of that at the tick rate would be 15000 STATE frames without the floor.
 */
static void caseBrightnessChurnIsBounded(void) {
  PanelCtl ctl;
  PanelCtlState st = baseState();
  uint32_t ms = 0u;
  int sent = 0;
  panelCtlInit(&ctl, ms);

  /* 60 s of 25 Hz ticks with brightness stepping every single tick. */
  for (int tick = 0; tick < 1500; tick++) {
    st.brightnessPct = (uint8_t)(20u + (unsigned)(tick % 60));
    if (panelCtlPoll(&ctl, &st, ms)) sent++;
    ms += 40u;
  }
  check(sent <= 62, "case 9: 60 s of per-tick brightness churn stays <= ~1 Hz");
  check(sent >= 58, "case 9: and the floor does not suppress it altogether");
}

/*
 * Case 10 — the uint32 millisecond clock wrap at ~49.7 days. The panel is meant
 * to run unattended for months; a wrap must not stall the heartbeat forever.
 */
static void caseClockWrap(void) {
  PanelCtl ctl;
  PanelCtlState st = baseState();
  uint32_t beforeWrap = 0xFFFFF000u;   /* ~4 s before rollover */

  panelCtlInit(&ctl, beforeWrap);
  check(panelCtlPoll(&ctl, &st, beforeWrap), "case 10: initial report");
  check(!panelCtlPoll(&ctl, &st, beforeWrap + 6000u),
        "case 10: 6 s spanning the wrap does not fire the heartbeat early");
  check(panelCtlPoll(&ctl, &st, beforeWrap + 30000u),
        "case 10: the heartbeat still fires 30 s later, across the wrap");
}

/*
 * Case 11 — alarm acknowledge is per-episode on the wire.
 * A new episodeId while gHaveAcked is still set must show alarmAcked = 0 again;
 * main.c computes that, but the transition has to produce a STATE frame or the
 * page shows a stale "acknowledged" over a live alarm.
 */
static void caseAckEpisodeTransitions(void) {
  PanelCtl ctl;
  PanelCtlState st = baseState();
  panelCtlInit(&ctl, 0u);

  st.alarmArmed = 1u;
  check(panelCtlPoll(&ctl, &st, 0u), "case 11: initial report, alarm armed");

  st.alarmArmed = 0u; st.alarmAcked = 1u; st.ackedEpisodeId = 77u;
  check(panelCtlPoll(&ctl, &st, 2000u), "case 11: the ack is reported");

  /* New episode: same ackedEpisodeId value on record, but it no longer matches
   * the live episode, so alarmAcked drops and the alarm re-arms. */
  st.alarmArmed = 1u; st.alarmAcked = 0u;
  check(panelCtlPoll(&ctl, &st, 4000u),
        "case 11: a new unacked episode is reported, ack falls away");

  st.alarmAcked = 1u; st.ackedEpisodeId = 78u;
  check(panelCtlPoll(&ctl, &st, 6000u),
        "case 11: ackedEpisodeId alone changing still reports");
}

/*
 * Case 11b — HELLOREQ forces a report even mid-floor and mid-heartbeat.
 * A restarted daemon must not wait up to 30 s to learn the panel speaks
 * CONTROL, because its capability gate withholds the whole queue until then.
 */
static void caseForceReport(void) {
  PanelCtl ctl;
  PanelCtlState st = baseState();
  panelCtlInit(&ctl, 0u);

  check(panelCtlPoll(&ctl, &st, 0u), "case 11b: initial report");
  check(!panelCtlPoll(&ctl, &st, 100u), "case 11b: silent 100 ms later");

  panelCtlForceReport(&ctl);
  check(panelCtlPoll(&ctl, &st, 100u),
        "case 11b: HELLOREQ reports immediately, inside the 1 s floor");
  check(!panelCtlPoll(&ctl, &st, 200u),
        "case 11b: and the force is one-shot, not sticky");
  check(panelCtlPoll(&ctl, &st, 100u + 30000u),
        "case 11b: the heartbeat clock restarts from the forced report");
}

/*
 * Case 11c — dwell is a reported field (v1.1 CP amendment, STATE byte 7).
 *
 * A dwell change is now news TWICE: the field itself moves, and setDwell
 * advances lastCmdId. Either alone would trigger a report; the case asserts the
 * field path specifically, so that a future change which stops routing dwell
 * through a command (a physical dwell control, say) does not silently stop
 * reporting it.
 */
static void caseDwellIsReported(void) {
  PanelCtl ctl;
  PanelCtlState st = baseState();
  panelCtlInit(&ctl, 0u);

  check(panelCtlPoll(&ctl, &st, 0u), "case 11c: initial report");
  check(!panelCtlPoll(&ctl, &st, 2000u), "case 11c: quiet at the default dwell");

  /* Field path only — no command, so lastCmdId does not move. */
  st.dwellSec = 30u;
  check(panelCtlPoll(&ctl, &st, 2040u),
        "case 11c: a dwell change alone triggers a report");
  check(!panelCtlPoll(&ctl, &st, 4000u), "case 11c: and then settles");

  st.dwellSec = 3u;
  check(panelCtlPoll(&ctl, &st, 6000u), "case 11c: 30 -> 3 reports too");

  /* And the ordinary path: a setDwell command moves both. */
  check(panelCtlConsume(&ctl, 1u, PANEL_CTL_SETDWELL, 6u, NULL) ==
            PANEL_CTLACT_DWELL, "case 11c: setDwell(6) applies");
  st.dwellSec = 6u;
  check(panelCtlPoll(&ctl, &st, 8000u),
        "case 11c: a commanded dwell change reports (field + lastCmdId)");
}

/* ---- wire-level integration against the real parser ---------------------- */

typedef struct {
  int      frames;
  uint8_t  type;
  uint8_t  payload[PANEL_MAX_PAYLOAD];
  size_t   len;
} CapturedFrame;

/* captureCb — PanelFrameCb that records the last frame the parser dispatched. */
static void captureCb(uint8_t type, const uint8_t *payload, size_t len,
                      void *user) {
  CapturedFrame *c = (CapturedFrame *)user;
  c->frames++;
  c->type = type;
  c->len  = len;
  if (len <= sizeof(c->payload)) memcpy(c->payload, payload, len);
}

/*
 * Case 12 — a real CONTROL frame must survive the real parser.
 *
 * The parser's knownType() gate is the specific thing under test. Before the
 * CONTRACT-CP amendment it admitted only 0x01-0x03 and 0x81-0x83, which would
 * DROP every 0x04 CONTROL frame before the callback — CRC-valid, no desync,
 * silently discarded. The firmware would have looked perfectly healthy while
 * ignoring every command the operator sent, so this case is worth its weight
 * even now that the gate is correct.
 *
 * Bytes are fed ONE AT A TIME, which is how they actually arrive from the USB
 * CDC read loop and is the shape that finds off-by-one framing bugs.
 */
static void caseControlThroughRealParser(void) {
  uint8_t payload[PANEL_CONTROL_SIZE];
  uint8_t frame[PANEL_HDR_SIZE + PANEL_CONTROL_SIZE + PANEL_CRC_SIZE];
  PanelParser parser;
  CapturedFrame cap;
  size_t plen, flen, i;

  memset(&cap, 0, sizeof(cap));
  plen = panelEncodeControl(4242u, PANEL_CTL_BRIGHTNESS, 55u,
                            payload, sizeof(payload));
  check(plen == PANEL_CONTROL_SIZE,
        "case 12: CONTROL encodes to PANEL_CONTROL_SIZE bytes");

  flen = panelEncodeFrame(PANEL_FT_CONTROL, payload, plen, frame, sizeof(frame));
  check(flen == PANEL_HDR_SIZE + PANEL_CONTROL_SIZE + PANEL_CRC_SIZE,
        "case 12: CONTROL frames to header+payload+crc");

  panelParserInit(&parser);
  for (i = 0; i < flen; i++) panelParserFeed(&parser, frame + i, 1u, 1000u,
                                             captureCb, &cap);

  check(cap.frames == 1,
        "case 12: the parser DISPATCHES 0x04 CONTROL (knownType admits it)");
  if (cap.frames == 1) {
    uint32_t cmdId = 0u; uint8_t kind = 0u, arg = 0u;
    check(cap.type == PANEL_FT_CONTROL, "case 12: dispatched as PANEL_FT_CONTROL");
    check(panelDecodeControl(cap.payload, cap.len, &cmdId, &kind, &arg) == 0,
          "case 12: the payload decodes");
    check(cmdId == 4242u && kind == PANEL_CTL_BRIGHTNESS && arg == 55u,
          "case 12: cmdId/kind/arg round-trip intact");
  }
  check(parser.crcErrors == 0u, "case 12: no CRC errors on a clean frame");
}

/*
 * Case 12b — the panel's own STATE frame must be dispatchable by the same
 * parser, which is what the daemon runs. 0x84 is above the old 0x83 ceiling,
 * so it is the mirror image of the CONTROL gate above.
 */
static void caseStateThroughRealParser(void) {
  uint8_t payload[PANEL_STATE_SIZE];
  uint8_t frame[PANEL_HDR_SIZE + PANEL_STATE_SIZE + PANEL_CRC_SIZE];
  PanelParser parser;
  CapturedFrame cap;
  size_t plen, flen;

  memset(&cap, 0, sizeof(cap));
  plen = panelEncodeState(2u, 1u, 73u, 0u, 1u, 1u, 0u, 6u, 77u, 12345u,
                          payload, sizeof(payload));
  flen = panelEncodeFrame(PANEL_FT_STATE, payload, plen, frame, sizeof(frame));
  panelParserInit(&parser);
  panelParserFeed(&parser, frame, flen, 1000u, captureCb, &cap);

  check(cap.frames == 1 && cap.type == PANEL_FT_STATE,
        "case 12b: the parser DISPATCHES 0x84 STATE (knownType admits it)");
  check(cap.len == PANEL_STATE_SIZE, "case 12b: STATE arrives at full length");
}

/*
 * Case 13 — STATE payload round-trip, including the u32 fields at their extreme.
 * The panel is the only producer of this frame, so a byte-order error here would
 * only ever surface as nonsense on the dashboard.
 */
static void caseStateRoundTrip(void) {
  uint8_t payload[PANEL_STATE_SIZE];
  uint8_t theme = 0, screen = 0, pct = 0, autoB = 0, slp = 0, armed = 0, ackd = 0;
  uint8_t dwell = 0;
  uint32_t episode = 0u, lastCmd = 0u;
  size_t n;

  n = panelEncodeState(2u, 1u, 73u, 0u, 1u, 1u, 0u, 30u,
                       0xDEADBEEFu, 0xFFFFFFFFu, payload, sizeof(payload));
  check(n == PANEL_STATE_SIZE, "case 13: STATE encodes to PANEL_STATE_SIZE");

  /* Byte 7 is dwellSec as of the v1.1 CP amendment (it was reserved, and the
   * codec used to hardcode it to 0). Asserted at the raw offset as well as
   * through the decoder: a decoder that read the wrong offset would still
   * round-trip against its own encoder, and this is the offset the daemon and
   * the dashboard page read. */
  check(payload[7] == 30u, "case 13: dwellSec lands at wire offset 7");

  check(panelDecodeState(payload, n, &theme, &screen, &pct, &autoB, &slp,
                         &armed, &ackd, &dwell, &episode, &lastCmd) == 0,
        "case 13: STATE decodes");
  check(theme == 2u && screen == 1u && pct == 73u && autoB == 0u &&
            slp == 1u && armed == 1u && ackd == 0u && dwell == 30u,
        "case 13: the eight u8 state fields round-trip");
  check(episode == 0xDEADBEEFu,
        "case 13: ackedEpisodeId round-trips little-endian");
  check(lastCmd == 0xFFFFFFFFu,
        "case 13: lastCmdId round-trips at the u32 ceiling");

  check(panelDecodeState(payload, PANEL_STATE_SIZE - 1u, &theme, &screen, &pct,
                         &autoB, &slp, &armed, &ackd, &dwell, &episode,
                         &lastCmd) == -1,
        "case 13: a short STATE payload is rejected, not read past");
  check(panelEncodeState(2u, 1u, 73u, 0u, 1u, 1u, 0u, 6u, 1u, 1u,
                         payload, PANEL_STATE_SIZE - 1u) == 0u,
        "case 13: encoding into a short buffer refuses rather than overruns");
}

/*
 * Case 14 — a truncated CONTROL payload must be rejected by the decoder.
 * A CRC-valid frame can still be short if the sender is buggy or a future
 * version shrinks the payload; main.c drops the frame on -1 and must not
 * consume a cmdId it never read.
 */
static void caseShortControlRejected(void) {
  uint8_t payload[PANEL_CONTROL_SIZE];
  uint32_t cmdId = 0u;
  uint8_t kind = 0u, arg = 0u;

  panelEncodeControl(9u, PANEL_CTL_SETTHEME, 1u, payload, sizeof(payload));
  check(panelDecodeControl(payload, PANEL_CONTROL_SIZE - 1u,
                           &cmdId, &kind, &arg) == -1,
        "case 14: a short CONTROL payload decodes to -1");
  check(panelDecodeControl(payload, PANEL_CONTROL_SIZE,
                           &cmdId, &kind, &arg) == 0 && cmdId == 9u,
        "case 14: the full-length payload still decodes");
}

int main(void) {
  caseValidKinds();
  caseRejectedStillConsumes();
  caseDwellValidation();
  caseDedupeAndAscent();
  caseRebootReplay();
  caseStateOnChange();
  caseLastCmdIdTriggersReport();
  caseHeartbeat();
  caseBrightnessChurnIsBounded();
  caseClockWrap();
  caseAckEpisodeTransitions();
  caseForceReport();
  caseDwellIsReported();
  caseControlThroughRealParser();
  caseStateThroughRealParser();
  caseStateRoundTrip();
  caseShortControlRejected();

  if (gFailures) {
    printf("\n%d FAILURE(S)\n", gFailures);
    return 1;
  }
  printf("\nall panelCtl cases pass\n");
  return 0;
}
