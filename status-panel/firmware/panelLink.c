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
  int i;
  link->lastAppliedMs = nowMs;
  link->activeTransport = PANEL_LINK_SERIAL;
  for (i = 0; i < PANEL_LINK_TRANSPORT_COUNT; ++i) {
    link->seq[i].lastSeq = 0u;
    link->seq[i].haveSeq = false;
    link->seq[i].adoptNext = false;
  }
  link->lost          = false;
}

void panelLinkSetActiveTransport(PanelLink *link,
                                 PanelLinkTransport transport) {
  if (link == NULL || transport >= PANEL_LINK_TRANSPORT_COUNT ||
      transport == link->activeTransport) return;
  link->activeTransport = transport;
  /* CONTRACT-SW D13: adoption is explicit on every active-transport switch,
   * even when this transport has an older/equal retained sequence number. */
  link->seq[transport].adoptNext = true;
}

void panelLinkResetTransportSeq(PanelLink *link,
                                PanelLinkTransport transport) {
  if (link == NULL || transport >= PANEL_LINK_TRANSPORT_COUNT) return;
  link->seq[transport].adoptNext = true;
}

bool panelLinkAcceptSnapshot(PanelLink *link, PanelLinkTransport transport,
                             uint16_t seq, uint32_t nowMs, bool *resynced) {
  PanelLinkSeq *seqState;
  if (resynced) *resynced = false;
  if (link == NULL || transport >= PANEL_LINK_TRANSPORT_COUNT ||
      transport != link->activeTransport) return false;
  seqState = &link->seq[transport];

  if (seqState->adoptNext) {
    seqState->adoptNext = false;
    seqState->lastSeq = seq;
    seqState->haveSeq = true;
    link->lastAppliedMs = nowMs;
    return true;
  }

  if (seqState->haveSeq && !panelSeqNewer(seq, seqState->lastSeq)) {
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
    /* D13 keeps CONTRACT.md's timed resync escape hatch SERIAL-ONLY. WiFi
     * sequence reset is signaled by panelLinkResetTransportSeq instead. */
    if (transport != PANEL_LINK_SERIAL ||
        ageMs(nowMs, link->lastAppliedMs) < (int32_t)PANEL_SEQ_RESYNC_MS) {
      return false;
    }
    if (resynced) *resynced = true;
  }

  seqState->lastSeq   = seq;
  seqState->haveSeq   = true;
  link->lastAppliedMs = nowMs;
  return true;
}

PanelLinkEdge panelLinkPoll(PanelLink *link, uint32_t nowMs) {
  /* CONTRACT-SW D14: only accepted SNAPSHOT staleness drives LINKLOST/BACK.
   * Switching transports does not touch either liveness flag or timestamp. */
  bool lost = ageMs(nowMs, link->lastAppliedMs) >
              (int32_t)PANEL_LINK_TIMEOUT_MS;

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
