/*
 * panelPowerTest.c — host suite for the VBUS ride-through module (P4).
 *
 * Covers the DESIGN-BRIEF-P4 §7 list: debounce bounds on both edges,
 * two-episodes-per-boot identity, ack semantics under ack-all, restore
 * clearing only the power episode, boot-on-battery at t=0, and episode-id
 * masking at the 0x3FFFFFFF wraparound. The OR-composition itself lives in
 * main.c's runAlarm(); its module-side inputs (panelPowerAlarmPending and
 * panelPowerAck) are what this suite pins down.
 */

#include <stdio.h>
#include <stdlib.h>

#include "../panelPower.h"

static int gChecks = 0;

static void check(int cond, const char *what) {
  gChecks++;
  if (cond) return;
  fprintf(stderr, "FAIL: %s\n", what);
  exit(1);
}

/* Feed one raw level for `ticks` 40 ms samples, returning the last edge. */
static PanelPowerEdge feed(PanelPower *p, bool raw, int ticks, uint32_t *ms) {
  PanelPowerEdge e = PANEL_POWER_STEADY;
  for (int i = 0; i < ticks; i++) {
    *ms += 40u;
    PanelPowerEdge got = panelPowerPoll(p, raw, *ms);
    if (got != PANEL_POWER_STEADY) e = got;
  }
  return e;
}

int main(void) {
  PanelPower p;
  uint32_t ms = 5000u;

  /* ---- powered boot: no episode, not on battery ---- */
  panelPowerInit(&p, 0x1234u, true, ms);
  check(!p.onBattery, "powered boot: not on battery");
  check(p.episodeId == 0u, "powered boot: no episode");
  check(!panelPowerAlarmPending(&p), "powered boot: no alarm");

  /* ---- debounce lower bound: a 2-sample (80 ms) glitch never commits ---- */
  check(feed(&p, false, 2, &ms) == PANEL_POWER_STEADY, "80 ms dip ignored");
  check(feed(&p, true, 1, &ms) == PANEL_POWER_STEADY, "recovery is silent");
  check(!p.onBattery, "80 ms dip: still powered");

  /* ---- debounce upper bound: the window runs from the raw change, so the
   * samples land at 0/40/80/120 ms — 80 ms is still short of 100, and the
   * 120 ms sample commits. ---- */
  check(feed(&p, false, 3, &ms) == PANEL_POWER_STEADY, "80 ms: not yet");
  check(!p.onBattery, "80 ms: still powered");
  check(feed(&p, false, 1, &ms) == PANEL_POWER_LOSS, "120 ms loss commits");
  check(p.onBattery, "loss: on battery");
  uint32_t ep1 = p.episodeId;
  check((ep1 & 0xC0000000u) == PANEL_POWER_EPISODE_BASE,
        "episode carries the power namespace");
  check(ep1 == (PANEL_POWER_EPISODE_BASE | 0x1234u), "episode = base|salt");
  check(panelPowerAlarmPending(&p), "loss: alarm pending");

  /* ---- ack-all hook: ack mutes THIS episode only ---- */
  panelPowerAck(&p);
  check(!panelPowerAlarmPending(&p), "acked: no alarm");
  check(p.onBattery, "acked: still on battery (glyph + cap stay)");

  /* ---- restore clears only the power episode ---- */
  check(feed(&p, true, 4, &ms) == PANEL_POWER_RESTORE, "restore commits");
  check(!p.onBattery && p.episodeId == 0u, "restore: episode cleared");
  check(!panelPowerAlarmPending(&p), "restore: no alarm");

  /* ---- D15: second outage in the same boot = a NEW episode; the earlier
   * ack cannot mute it ---- */
  check(feed(&p, false, 4, &ms) == PANEL_POWER_LOSS, "second loss commits");
  uint32_t ep2 = p.episodeId;
  check(ep2 != ep1, "two outages, two episodes");
  check(ep2 == (PANEL_POWER_EPISODE_BASE | 0x1235u), "counter advanced by 1");
  check(panelPowerAlarmPending(&p), "second episode re-arms despite old ack");
  check(feed(&p, true, 4, &ms) == PANEL_POWER_RESTORE, "cleanup restore");

  /* ---- ack when powered is a no-op ---- */
  panelPowerAck(&p);
  check(!p.acked, "ack off-battery: no-op");

  /* ---- a raw flap inside the window restarts the debounce ---- */
  check(feed(&p, false, 2, &ms) == PANEL_POWER_STEADY, "flap: 40 ms down");
  check(feed(&p, true, 1, &ms) == PANEL_POWER_STEADY, "flap: back up");
  check(feed(&p, false, 3, &ms) == PANEL_POWER_STEADY,
        "flap: window restarted, 80 ms again is not enough");
  check(feed(&p, false, 1, &ms) == PANEL_POWER_LOSS, "flap: settles, commits");
  check(feed(&p, true, 4, &ms) == PANEL_POWER_RESTORE, "cleanup restore 2");

  /* ---- boot-on-battery: loss at t=0, episode before any poll ---- */
  PanelPower b;
  panelPowerInit(&b, 77u, false, 0u);
  check(b.onBattery, "boot-on-battery: on battery at t=0");
  check(b.episodeId == (PANEL_POWER_EPISODE_BASE | 77u),
        "boot-on-battery: episode allocated immediately");
  check(panelPowerAlarmPending(&b), "boot-on-battery: alarm pending");

  /* ---- mask wraparound: a salt at the top of the 30-bit space stays inside
   * the 0xC0000000 namespace and never collides with bit 31/30 ---- */
  PanelPower w;
  panelPowerInit(&w, 0x3FFFFFFFu, false, 0u);
  check(w.episodeId == 0xFFFFFFFFu, "wrap: top-of-mask id");
  ms = 0u;
  check(feed(&w, true, 4, &ms) == PANEL_POWER_RESTORE, "wrap: restore");
  check(feed(&w, false, 4, &ms) == PANEL_POWER_LOSS, "wrap: next loss");
  check(w.episodeId == PANEL_POWER_EPISODE_BASE,
        "wrap: counter wraps to base, namespace intact");

  /* ---- a salt with high bits set is masked off, not smeared into the
   * namespace ---- */
  PanelPower s;
  panelPowerInit(&s, 0xDEADBEEFu, false, 0u);
  check((s.episodeId & 0xC0000000u) == PANEL_POWER_EPISODE_BASE &&
        (s.episodeId & PANEL_POWER_EPISODE_MASK) ==
            (0xDEADBEEFu & PANEL_POWER_EPISODE_MASK),
        "dirty salt: masked into namespace");

  /* ---- D15/D21 OR-composition, through the SAME predicate main.c keys on
   * (panelAlarmWant). Simulated server side: `srvUnacked` plays the role of
   * runAlarm()'s serverActive && !ackedThis input. ---- */
  PanelPower c2;
  ms = 1000u;
  panelPowerInit(&c2, 9u, true, ms);
  check(!panelAlarmWant(false, &c2), "compose: no source, no alarm");
  check(panelAlarmWant(true, &c2), "compose: server alone");
  check(feed(&c2, false, 4, &ms) == PANEL_POWER_LOSS, "compose: loss");
  check(panelAlarmWant(false, &c2), "compose: power alone");
  check(panelAlarmWant(true, &c2), "compose: both (D21 simultaneous)");

  /* Ack-all: one acknowledge covers both sources — the caller records the
   * server ack (srvUnacked falls) AND calls panelPowerAck. */
  panelPowerAck(&c2);
  check(!panelAlarmWant(false, &c2), "ack-all: silent after both acked");

  /* Restore isolation: VBUS restore clears ONLY the power episode — a live
   * unacked server episode keeps the alarm wanted. */
  check(feed(&c2, false, 1, &ms) == PANEL_POWER_STEADY, "hold battery");
  check(feed(&c2, true, 4, &ms) == PANEL_POWER_RESTORE, "compose: restore");
  check(panelAlarmWant(true, &c2), "restore keeps the server alarm");
  check(!panelAlarmWant(false, &c2), "restore cleared the power side");

  /* Server SNAPSHOT-side changes never clear the power episode: a fresh
   * loss stays pending regardless of the server input's transitions. */
  check(feed(&c2, false, 4, &ms) == PANEL_POWER_LOSS, "compose: loss 2");
  check(panelAlarmWant(false, &c2) && panelAlarmWant(true, &c2),
        "power episode survives any server-side transition");

  /* ---- Episode identity (final review MUST-2): a NEW unacked episode on
   * either source must report fresh even while the alarm is already armed —
   * main.c re-inits cadence/auto-silence/wake on `fresh`. ---- */
  {
    PanelAlarmIdent id;
    PanelPower c3;
    ms = 1000u;
    panelPowerInit(&c3, 9u, true, ms);
    panelAlarmIdentReset(&id);
    check(!panelAlarmIdentUpdate(&id, false, 0u, &c3), "ident: quiet start");

    /* Server episode appears — fresh once, then steady. */
    check(panelAlarmIdentUpdate(&id, true, 100u, &c3), "ident: server episode fresh");
    check(!panelAlarmIdentUpdate(&id, true, 100u, &c3), "ident: steady server not fresh");

    /* Power loss while the server alarm still holds armed: MUST-2 core. */
    check(feed(&c3, false, 4, &ms) == PANEL_POWER_LOSS, "ident: loss");
    check(panelAlarmIdentUpdate(&id, true, 100u, &c3),
          "ident: power episode fresh while server holds armed");
    check(!panelAlarmIdentUpdate(&id, true, 100u, &c3), "ident: both steady");

    /* Server episode id rolls A -> B under a held alarm: also fresh. */
    check(panelAlarmIdentUpdate(&id, true, 101u, &c3), "ident: server A->B fresh");

    /* Ack-all empties the ident; the same episodes never re-fire. */
    panelPowerAck(&c3);
    check(!panelAlarmIdentUpdate(&id, false, 0u, &c3), "ident: ack-all clears");
    check(!panelAlarmIdentUpdate(&id, false, 0u, &c3), "ident: stays clear");

    /* A second power episode after restore is a new identity. */
    check(feed(&c3, true, 4, &ms) == PANEL_POWER_RESTORE, "ident: restore");
    check(feed(&c3, false, 4, &ms) == PANEL_POWER_LOSS, "ident: loss 2");
    check(panelAlarmIdentUpdate(&id, false, 0u, &c3), "ident: second power episode fresh");
  }

  printf("panelPowerTest: %d checks passed\n", gChecks);
  return 0;
}
