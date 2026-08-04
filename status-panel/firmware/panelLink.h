/*
 * panelLink.h — link liveness and snapshot-ordering state machine.
 *
 * Extracted from main.c's tick on 2026-08-04 after a LINKLOST/LINKBACK flapping
 * incident reached hardware. The logic is small but it is time-arithmetic over
 * wrapping uint32 counters, which is exactly the kind of code that reads correct
 * and is not. Splitting it out of the render loop makes it reachable from a host
 * unit test (test/panelLinkTest.c), which is the actual point: this class of
 * defect must be caught on the build host, not on the board.
 *
 * No hardware dependencies — plain C over a caller-supplied millisecond clock,
 * so it compiles identically for the RP2350 firmware and for the host test.
 */

#ifndef SOLARI_PANEL_LINK_H
#define SOLARI_PANEL_LINK_H

#include <stdbool.h>
#include <stdint.h>

/* CONTRACT §4: more than 15 s with no valid frame of any kind is LINK LOST. */
#define PANEL_LINK_TIMEOUT_MS 15000u

/* Snapshots arriving but none ACCEPTED for this long means the sender's seq
 * counter went backwards and stayed there — see panelLinkAcceptSnapshot. */
#define PANEL_SEQ_RESYNC_MS   30000u

typedef enum {
  PANEL_LINK_NO_CHANGE = 0,
  PANEL_LINK_WENT_LOST = 1,
  PANEL_LINK_CAME_BACK = 2
} PanelLinkEdge;

typedef struct {
  uint32_t lastFrameMs;    /* any CRC-valid frame, PING included             */
  uint32_t lastAppliedMs;  /* last snapshot ACCEPTED, not merely received    */
  uint16_t lastSeq;
  bool     haveSeq;
  bool     lost;
} PanelLink;

/* panelLinkInit — seed both clocks from the current time so a freshly booted
 * panel is not instantly stale.
 * Input: link, current ms. Output: none.                                    */
void panelLinkInit(PanelLink *link, uint32_t nowMs);

/* panelLinkNoteFrame — record that a CRC-valid frame of ANY type arrived.
 * Input: link, the ms at which it arrived. Output: none.                    */
void panelLinkNoteFrame(PanelLink *link, uint32_t nowMs);

/* panelLinkAcceptSnapshot — apply protocol.h's receiver ordering rule.
 * Input:  link, the snapshot's seq, current ms, out-param resynced (may be
 *         NULL) which is set true when the escape hatch fired.
 * Output: true if the caller should apply this snapshot, false to drop it.  */
bool panelLinkAcceptSnapshot(PanelLink *link, uint16_t seq, uint32_t nowMs,
                             bool *resynced);

/* panelLinkPoll — evaluate liveness once per tick.
 * Input:  link, current ms. Output: the edge crossed this tick, if any.
 * MUST be called with a timestamp sampled AFTER the frame pump — see the
 * implementation note, which is the flapping bug this file was born from.   */
PanelLinkEdge panelLinkPoll(PanelLink *link, uint32_t nowMs);

#endif /* SOLARI_PANEL_LINK_H */
