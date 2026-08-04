/*
 * panelLinkTest.c — host-side unit test for the link/staleness state machine.
 *
 * Born from the 2026-08-04 flapping incident: build dbd33885 emitted an
 * EV_LINKLOST/EV_LINKBACK pair once per snapshot on real hardware. That defect
 * was reachable from a pure-C state machine with a synthetic clock, so it should
 * never have needed a board to find. This test is the gate that keeps the class
 * off the hardware.
 *
 * Build and run:   make -C firmware/test
 * Exit code 0 = all cases pass. Any failure prints the case and returns 1.
 */

#include <stdio.h>
#include <string.h>

#include "../panelLink.h"

static int gFailures = 0;

/* check — record and report one assertion.
 * Input: condition, printf-style label. Output: none (increments gFailures).  */
static void check(bool ok, const char *what) {
  if (!ok) {
    printf("FAIL: %s\n", what);
    gFailures++;
  } else {
    printf("ok:   %s\n", what);
  }
}

/*
 * Case 1 — the incident, reproduced.
 *
 * Models main.c's tick exactly as the flapping build ran it: a 40 ms tick, a
 * snapshot every 2 s, and — this is the defect — the liveness check evaluated
 * against a clock sampled BEFORE the serial pump, while the frame's arrival is
 * stamped one millisecond LATER (a ~110-byte read crossing a ms boundary).
 *
 * Under the old unsigned comparison this produced 4294967295 ms of apparent age
 * and fired LINKLOST on the tick a healthy frame arrived. Asserts zero LINKLOST
 * emissions across 150 snapshots (300 s of simulated healthy stream).
 */
static void caseHealthyStreamNeverDropsLink(void) {
  PanelLink link;
  uint32_t  ms = 100000u;   /* arbitrary non-zero boot offset */

  panelLinkInit(&link, ms);

  int lostCount = 0, backCount = 0, applied = 0, snapshots = 0;
  uint16_t seq = 1;

  for (int tick = 0; tick < 7500; tick++) {   /* 7500 * 40 ms = 300 s */
    uint32_t tickMs = ms;                     /* clock sampled at top of tick */
    ms += 40u;

    /* One snapshot every 50 ticks = 2 s, the daemon's real cadence. */
    if (tick % 50 == 0) {
      /* The frame lands mid-tick, AFTER tickMs was sampled, and its read
       * crosses a millisecond boundary. This +1 is the whole incident. */
      uint32_t arriveMs = tickMs + 1u;
      panelLinkNoteFrame(&link, arriveMs);
      if (panelLinkAcceptSnapshot(&link, seq++, arriveMs, NULL)) applied++;
      snapshots++;
    }

    /* Deliberately poll with the pre-pump timestamp: the fix must tolerate a
     * frame timestamp in the future rather than reading it as a 49-day age. */
    PanelLinkEdge edge = panelLinkPoll(&link, tickMs);
    if (edge == PANEL_LINK_WENT_LOST) lostCount++;
    if (edge == PANEL_LINK_CAME_BACK) backCount++;
  }

  check(snapshots >= 100, "case 1: drove 100+ snapshot frames");
  check(applied == snapshots, "case 1: every in-order snapshot applied");
  check(lostCount == 0, "case 1: ZERO LINKLOST over a healthy 2 s stream");
  check(backCount == 0, "case 1: zero spurious LINKBACK");
}

/*
 * Case 2 — a real outage still reports, and recovery still reports.
 * The fix must not achieve zero flapping by never firing at all.
 */
static void caseRealOutageStillFires(void) {
  PanelLink link;
  uint32_t  ms = 0u;

  panelLinkInit(&link, ms);

  check(panelLinkPoll(&link, ms + 14000u) == PANEL_LINK_NO_CHANGE,
        "case 2: 14 s silence is not yet lost");
  check(panelLinkPoll(&link, ms + 15001u) == PANEL_LINK_WENT_LOST,
        "case 2: 15.001 s silence raises LINKLOST");
  check(panelLinkPoll(&link, ms + 16000u) == PANEL_LINK_NO_CHANGE,
        "case 2: LINKLOST is edge-triggered, not repeated");

  panelLinkNoteFrame(&link, ms + 20000u);
  check(panelLinkPoll(&link, ms + 20000u) == PANEL_LINK_CAME_BACK,
        "case 2: a frame restores the link once");
  check(panelLinkPoll(&link, ms + 20040u) == PANEL_LINK_NO_CHANGE,
        "case 2: LINKBACK is edge-triggered, not repeated");
}

/*
 * Case 3 — the uint32 millisecond wrap at ~49.7 days.
 * The panel is meant to run unattended for months; the comparison must survive
 * the counter rolling over mid-stream.
 */
static void caseClockWrap(void) {
  PanelLink link;
  uint32_t  beforeWrap = 0xFFFFF000u;   /* ~4 s before rollover */

  panelLinkInit(&link, beforeWrap);
  panelLinkNoteFrame(&link, beforeWrap);

  uint32_t afterWrap = beforeWrap + 6000u;   /* wraps past zero */
  check(panelLinkPoll(&link, afterWrap) == PANEL_LINK_NO_CHANGE,
        "case 3: 6 s spanning the uint32 wrap is not lost");

  check(panelLinkPoll(&link, beforeWrap + 16000u) == PANEL_LINK_WENT_LOST,
        "case 3: 16 s spanning the wrap is correctly lost");
}

/*
 * Case 4 — snapshot ordering, including the resync escape hatch.
 */
static void caseSeqOrdering(void) {
  PanelLink link;
  uint32_t  ms = 500000u;

  panelLinkInit(&link, ms);

  bool resynced = false;
  check(panelLinkAcceptSnapshot(&link, 10u, ms, &resynced) && !resynced,
        "case 4: first snapshot applies");
  check(panelLinkAcceptSnapshot(&link, 11u, ms + 2000u, &resynced) && !resynced,
        "case 4: newer seq applies");
  check(!panelLinkAcceptSnapshot(&link, 11u, ms + 4000u, &resynced) && !resynced,
        "case 4: equal seq is a duplicate and is dropped");
  check(!panelLinkAcceptSnapshot(&link, 9u, ms + 6000u, &resynced) && !resynced,
        "case 4: older seq is dropped");

  /* Sender restarts its counter at 1 and never advances past our last applied
   * value. Nothing may apply until the 30 s window elapses, then exactly one
   * resync adopts the sender's counter. */
  /* The window is measured from the last ACCEPTANCE (ms + 2000), not from the
   * restart, so this loop stops at ms + 30000 — age 28 s, still inside. */
  uint32_t restartMs = ms + 8000u;
  int      dropped   = 0;
  for (uint32_t t = 0u; t < 24000u; t += 2000u) {
    if (!panelLinkAcceptSnapshot(&link, 1u, restartMs + t, &resynced)) dropped++;
  }
  check(dropped == 12, "case 4: duplicates keep dropping inside the window");
  check(!resynced, "case 4: escape hatch stays idle inside the window");

  /* lastAppliedMs is still ms + 2000 (the last acceptance), so cross the
   * 30 s mark measured from there. */
  check(panelLinkAcceptSnapshot(&link, 1u, ms + 2000u + 30001u, &resynced),
        "case 4: escape hatch applies after 30 s with nothing applied");
  check(resynced, "case 4: escape hatch reports the resync");
  check(panelLinkAcceptSnapshot(&link, 2u, ms + 2000u + 32000u, &resynced) &&
            !resynced,
        "case 4: stream resumes normally on the adopted counter");
}

/*
 * Case 5 — the escape hatch must never fire on a healthy stream.
 * The 30 s counter is refreshed by every ACCEPTED snapshot, so 300 s of
 * in-order traffic must produce zero resyncs.
 */
static void caseNoSpuriousResync(void) {
  PanelLink link;
  uint32_t  ms = 1000u;

  panelLinkInit(&link, ms);

  int resyncCount = 0;
  uint16_t seq = 1;
  for (int i = 0; i < 150; i++) {
    bool resynced = false;
    panelLinkNoteFrame(&link, ms);
    panelLinkAcceptSnapshot(&link, seq++, ms, &resynced);
    if (resynced) resyncCount++;
    ms += 2000u;
  }
  check(resyncCount == 0, "case 5: no resync over 150 healthy snapshots");
}

int main(void) {
  caseHealthyStreamNeverDropsLink();
  caseRealOutageStillFires();
  caseClockWrap();
  caseSeqOrdering();
  caseNoSpuriousResync();

  if (gFailures) {
    printf("\n%d FAILURE(S)\n", gFailures);
    return 1;
  }
  printf("\nall panelLink cases pass\n");
  return 0;
}
