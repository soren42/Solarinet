/*
 * panelLink.c — link liveness and snapshot-ordering state machine.
 *
 * See panelLink.h for why this is a separate translation unit.
 *
 * THE FLAPPING BUG THIS FILE FIXES (incident 2026-08-04, build dbd33885):
 *
 *   main.c's tick sampled the clock BEFORE pumping the serial port:
 *
 *       uint32_t ms = nowMs();          <- sampled here
 *       pumpSerial();                   <- onFrame() sets gLastFrameMs = nowMs()
 *       bool lost = (ms - gLastFrameMs) > LINK_TIMEOUT_MS;
 *
 *   Reading a ~110-byte SNAPSHOT one byte at a time crosses a millisecond
 *   boundary, so gLastFrameMs lands one or more ms AFTER ms. The subtraction is
 *   unsigned, so "one millisecond in the future" evaluates to 4294967295, which
 *   is comfortably greater than the 15 s timeout: the panel declared LINK LOST
 *   on the very tick a healthy frame arrived, then declared LINK BACK on the
 *   next tick. One EV_LINKLOST/EV_LINKBACK pair per snapshot, locked to the
 *   snapshot interval. An 8-byte PING usually reads inside a single millisecond,
 *   which is why the flap tracked snapshots and not pings.
 *
 * The fix is two-part and both halves matter:
 *   1. Compare as int32_t. A timestamp slightly in the future yields a small
 *      NEGATIVE age instead of a huge positive one, and the comparison stays
 *      correct across the 49.7-day uint32 millisecond wrap.
 *   2. panelLinkPoll takes the clock as an argument so the caller can — and
 *      main.c now does — sample it after the pump.
 * Part 1 alone is sufficient; part 2 means the age is also honest.
 */

#include "panelLink.h"

#include "../protocol.h"

/* ageMs — signed age of a timestamp, wrap-safe and future-safe.
 * Input:  now, then (uint32 millisecond counters).
 * Output: milliseconds elapsed, negative if `then` is in the future.        */
static int32_t ageMs(uint32_t now, uint32_t then) {
  return (int32_t)(now - then);
}

void panelLinkInit(PanelLink *link, uint32_t nowMs) {
  link->lastFrameMs   = nowMs;
  link->lastAppliedMs = nowMs;
  link->lastSeq       = 0;
  link->haveSeq       = false;
  link->lost          = false;
}

void panelLinkNoteFrame(PanelLink *link, uint32_t nowMs) {
  link->lastFrameMs = nowMs;
}

bool panelLinkAcceptSnapshot(PanelLink *link, uint16_t seq, uint32_t nowMs,
                             bool *resynced) {
  if (resynced) *resynced = false;

  if (link->haveSeq && !panelSeqNewer(seq, link->lastSeq)) {
    /* protocol.h's ordering rule: older-or-equal is a duplicate and is dropped.
     * That rule has no way out if the SENDER's counter goes backwards — a
     * daemon restart, or a bug that resets seq — because every subsequent frame
     * looks like a duplicate forever and the panel silently freezes on stale
     * data while looking perfectly healthy (CONTRACT §5: it keeps animating).
     *
     * The escape: if snapshots keep arriving but NONE has been applied for
     * PANEL_SEQ_RESYNC_MS, believe the sender and adopt its counter. The window
     * is long relative to the 2 s snapshot cadence, so ordinary duplicates and
     * brief reordering never trip it. */
    if (ageMs(nowMs, link->lastAppliedMs) < (int32_t)PANEL_SEQ_RESYNC_MS) {
      return false;
    }
    if (resynced) *resynced = true;
  }

  link->lastSeq       = seq;
  link->haveSeq       = true;
  link->lastAppliedMs = nowMs;
  return true;
}

PanelLinkEdge panelLinkPoll(PanelLink *link, uint32_t nowMs) {
  /* CONTRACT §4: more than 15 s with no valid frame is LINK LOST; recovery is
   * announced too so the daemon journal shows both edges. Signed compare — see
   * the file header. */
  bool lost = ageMs(nowMs, link->lastFrameMs) > (int32_t)PANEL_LINK_TIMEOUT_MS;

  if (lost && !link->lost) {
    link->lost = true;
    return PANEL_LINK_WENT_LOST;
  }
  if (!lost && link->lost) {
    link->lost = false;
    return PANEL_LINK_CAME_BACK;
  }
  return PANEL_LINK_NO_CHANGE;
}
