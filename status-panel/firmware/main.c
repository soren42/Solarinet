/*
 * main.c — solari-panel-fw core loop.
 *
 * Responsibilities, in the order the tick performs them:
 *   1. drain USB-CDC bytes into the shared frame parser (protocol.c)
 *   2. apply any complete SNAPSHOT, refreshing PanelEnv and its history rings
 *   3. sample buttons and run the interaction state machine
 *   4. run the alarm state machine (arm / re-alarm / acknowledge)
 *   5. paint the active screen, then the inlay if the alarm is live
 *   6. report STATE to the host when it changed, or on the 30 s heartbeat
 *
 * Host CONTROL frames (CONTRACT-CP.md §3/§6) arrive in step 1 and are applied
 * through the SAME functions the physical buttons call — pressTheme(),
 * ackAlarm(), panelHwSetBrightness(). There is deliberately no second copy of
 * any of that behaviour; a software theme press and a physical one are the same
 * press. What differs is only that CONTROL carries an explicit argument where a
 * button carries an implicit one: sleep(0/1) instead of ZZZ's toggle, and ack
 * only via ackAlarm() rather than "any button while the alarm is armed".
 *
 * Rendering is autonomous (CONTRACT §5): the panel animates at its own 25 Hz
 * tick from the LAST applied snapshot. Data staleness only changes what is
 * drawn, never whether drawing happens.
 */

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "pico/stdlib.h"
#include "pico/binary_info.h"
#ifdef SOLARI_PANEL_PICO_FLASH
#include "pico/flash.h"
#endif

#include "protocol.h"
#include "panelCtl.h"
#include "panelHelp.h"
#include "panelHelpOverlay.h"
#include "panelScreenCfg.h"
#include "panelHw.h"
#include "panelLink.h"
#include "panelFb.h"
#include "panelFont.h"
#include "panelHist.h"
#include "panelScreens.h"

#define SOLARI_STR2(x) #x
#define SOLARI_STR(x)  SOLARI_STR2(x)
#define SOLARI_FW_VERSION \
  SOLARI_STR(SOLARI_FW_VERSION_MAJOR) "." SOLARI_STR(SOLARI_FW_VERSION_MINOR)
/* Build string is derived from the PINNED SOURCE_DATE_EPOCH, never from
 * __DATE__/__TIME__, so two builds of the same tree produce the same image. */
#define SOLARI_BUILD_STRING SOLARI_FW_VERSION "+" SOLARI_STR(SOLARI_BUILD_EPOCH)

/* picotool identity — CONTRACT §9 acceptance 3 checks both of these. */
bi_decl(bi_program_name("solari-panel-fw"))
bi_decl(bi_program_version_string(SOLARI_BUILD_STRING))
bi_decl(bi_program_description("SolariNet fleet status panel (Galactic Unicorn)"))

/* ---- timing constants --------------------------------------------------- */
#define TICK_MS          40u     /* DESIGN-BRIEF: 40 ms logic tick, ~25 Hz   */
#define DWELL_DEFAULT_SEC 6.0f   /* DESIGN-BRIEF: dwell default 6 s          */
/* Link liveness and snapshot-ordering thresholds now live in panelLink.h,
 * alongside the state machine they govern and its host unit test.          */
/* OPERATOR AMENDMENT 2026-08-04, superseding the DESIGN-BRIEF's 12 s re-alarm:
 * the tone repeats every 60 s, and an episode left unacknowledged for 5 minutes
 * stops re-sounding permanently. Auto-silence is NOT an acknowledge — the inlay
 * and the beacon both stay up, and a new episodeId re-arms sound as normal.  */
#define REALARM_SEC      60.0f   /* was 12 s (DESIGN-BRIEF "Animation")       */
#define AUTOSILENCE_SEC  300.0f  /* unacked for this long -> tone off, once   */
#define BRIGHT_STEP      0.05f   /* DESIGN-BRIEF Ambiguity #3: 5% steps      */
/* Raw light reading at which auto-brightness goes to full. Deliberately well
 * below the ADC's 4095 ceiling: the panel lives in direct daylight and that is
 * the NORMAL condition, not the extreme. Tunable knob — see runAutoBrightness. */
#define PANEL_LUX_FULL   1600.0f
#define DEBOUNCE_TICKS   2       /* 80 ms at the 25 Hz tick                  */

/* ---- panel state -------------------------------------------------------- */
static PanelEnv    gEnv;
static PanelParser gParser;

static int   gTheme = 3;       /* CONTRACT §5: boot = Theme D ...            */
static int   gScreen = 1;      /* ... screen 2 of 3, the "resting face"      */
static float gT = 0.0f;        /* seconds since boot, the animation clock    */
static float gScreenT = 0.0f;  /* dwell accumulator                          */
/* Dwell is runtime-settable via CONTROL setDwell (3/6/30 s only, validated in
 * panelCtl.c). It is NOT a STATE field — the contract's STATE payload has no
 * room for it — so a setDwell is confirmed to the server by lastCmdId alone.  */
static float gDwellSec = DWELL_DEFAULT_SEC;

static bool  gSleeping = false;
static bool  gAutoBright = true;   /* SHOULD; LUX+/- latches manual control  */
static float gAutoBrightSmoothed = 0.85f;

/* Link liveness + snapshot ordering. See panelLink.c; do not reimplement the
 * time arithmetic here, that is what flapped the link on 2026-08-04.       */
static PanelLink gLink;

/* CONTROL dedupe + STATE cadence. See panelCtl.c; the ascending-cmdId rule and
 * the change/heartbeat decision live there so the host suite can drive them.  */
static PanelCtl gCtl;
static PanelHelp gHelp;
static PanelScreenCfg gScreenCfg;
static PanelScreenCfgFlash gScreenCfgFlash;

/* Alarm state — CONTRACT §9. ack is firmware-local and scoped to episodeId;
 * a NEW episodeId re-arms the tone even if the previous one was acked. */
static bool     gAlarmArmed = false;
static uint32_t gAckedEpisode = 0;
static bool     gHaveAcked = false;
static float    gAlarmToneAt = -1000.0f;  /* start time of the current triad */
static int      gToneNote = 3;            /* 3 = triad finished              */
static float    gAlarmRaisedAt = 0.0f;    /* gT at this episode's rising edge*/
static bool     gAlarmSilenced = false;   /* auto-silenced: tone off, seen   */

static uint8_t  gBtnStable[PANEL_BTN_COUNT];
static uint8_t  gBtnCount[PANEL_BTN_COUNT];

static const PanelScreenFn kScreens[4][3] = {
  { panelScreenA0, panelScreenA1, panelScreenA2 },
  { panelScreenB0, panelScreenB1, panelScreenB2 },
  { panelScreenC0, panelScreenC1, panelScreenC2 },
  { panelScreenD0, panelScreenD1, panelScreenD2 }
};

static uint32_t nowMs(void) { return to_ms_since_boot(get_absolute_time()); }

/* sendFrame — encode and push one panel->host frame.
 * Input:  type, payload + length. Output: none (best effort; the daemon is
 * required to tolerate silence, and stdio drops writes when no host is
 * attached rather than blocking).                                           */
static void sendFrame(uint8_t type, const uint8_t *payload, size_t len) {
  uint8_t out[PANEL_HDR_SIZE + 160 + PANEL_CRC_SIZE];
  if (len > 160) return;
  size_t n = panelEncodeFrame(type, payload, len, out, sizeof(out));
  if (!n) return;
  fwrite(out, 1, n, stdout);
  fflush(stdout);
}

/* sendHello — PANEL_FT_HELLO: u8 protoVer, u8 fwMajor, u8 fwMinor, then the
 * ASCII build string (<= 32), per protocol.h. */
static void sendHello(void) {
  uint8_t p[3 + 32];
  p[0] = PANEL_PROTO_VERSION;
  p[1] = SOLARI_FW_VERSION_MAJOR;
  p[2] = SOLARI_FW_VERSION_MINOR;
  size_t n = strlen(SOLARI_BUILD_STRING);
  if (n > 32) n = 32;
  memcpy(p + 3, SOLARI_BUILD_STRING, n);
  sendFrame(PANEL_FT_HELLO, p, 3 + n);
}

/* sendConfig — report the panel-authoritative twelve screen settings.
 * CONTRACT-AW.md §3.3: CONFIG is emitted on every accepted setting change and
 * on HELLOREQ. bit0 says the current image is known to match persistent flash. */
static void sendConfig(void) {
  uint8_t payload[PANEL_CONFIG_SIZE];
  uint8_t flags = gScreenCfg.persisted ? 1u : 0u;
  size_t n = panelEncodeConfig(gScreenCfg.screenCfg, flags, payload,
                               sizeof(payload));
  if (n) sendFrame(PANEL_FT_CONFIG, payload, n);
}

/* sendLog — PANEL_FT_LOG: ASCII text (<=128), diagnostics only. Used for the
 * events the daemon journal should carry but that no EVENT kind covers.     */
static void sendLog(const char *text) {
  size_t n = strlen(text);
  if (n > 128) n = 128;
  sendFrame(PANEL_FT_LOG, (const uint8_t *)text, n);
}

static void sendEvent(uint8_t kind, uint8_t arg) {
  uint8_t p[2] = { kind, arg };
  sendFrame(PANEL_FT_EVENT, p, sizeof(p));
}

/* Defined below, next to the button handler it shares its code paths with.
 * Forward-declared because CONTROL frames are applied where they arrive, inside
 * onFrame: a single pump can deliver several commands (the daemon re-serves the
 * whole pending queue every poll) and they must apply in the order they were
 * queued, which stashing one pending action for the tick to pick up would not
 * preserve.                                                                   */
static void applyControl(PanelCtlAction act, uint8_t arg);

/* onFrame — parser callback, fires once per CRC-valid frame.
 * Applies the protocol.h receiver rules: a snapshot whose seq equals the last
 * applied seq is a duplicate and ignored, and so is an older one — both via
 * panelSeqNewer() in the shared codec, which is RFC1982 wraparound-aware.   */
static void onFrame(uint8_t type, const uint8_t *payload, size_t len, void *user) {
  (void)user;
  uint32_t arriveMs = nowMs();
  panelLinkNoteFrame(&gLink, arriveMs);  /* ANY valid frame, PING included */

  switch (type) {
    case PANEL_FT_SNAPSHOT: {
      PanelSnapshot snap;
      if (panelDecodeSnapshot(payload, len, &snap) != 0) return;
      /* The ordering test and the resync escape both live in panelLink.c, which
       * is covered by test/panelLinkTest.c. Do not hand-roll either here.
       *
       * The escape exists because the ordering rule alone is a one-way trap.
       * When the DAEMON restarts, its seq counter restarts at 1 while the panel
       * still holds a much larger lastApplied, and panelSeqNewer() correctly
       * calls every subsequent snapshot OLDER — for the next ~32768 snapshots,
       * about 18 hours at the 2 s cadence. The panel is not visibly broken while
       * this happens, which is what makes it nasty: rendering is autonomous
       * (CONTRACT §5) so the screens keep animating the last applied snapshot,
       * the rejected frames are CRC-valid and so keep refreshing the liveness
       * timer, and the link never goes LOST. It silently shows stale data
       * forever. A live crit alert raised after such a restart never reaches
       * PanelEnv at all — no inlay, no tone, no beacon.                      */
      bool resynced = false;
      if (!panelLinkAcceptSnapshot(&gLink, snap.seq, arriveMs, &resynced)) return;
      if (resynced) sendLog("seq resync: sender restarted");
      panelEnvApply(&gEnv, &snap);
      break;
    }
    case PANEL_FT_HELLOREQ:
      sendHello();
      sendConfig();
      /* The daemon asks for HELLO on every link-up, and CONTRACT-CP §10 makes
       * a STATE frame the proof that this firmware speaks CONTROL at all —
       * until it arrives, the daemon withholds the queue. Answer both now
       * rather than making a restarted daemon wait for the heartbeat. */
      panelCtlForceReport(&gCtl);
      break;
    case PANEL_FT_CONTROL: {
      /* CONTRACT-CP §3/§10. Dedupe and validation are panelCtl's; application
       * is the button code paths'. Nothing is acknowledged on the wire here —
       * the panel's next STATE report, carrying the advanced lastCmdId, is the
       * only completion signal the server accepts. */
      uint32_t cmdId; uint8_t kind, cmdArg, arg = 0;
      if (panelDecodeControl(payload, len, &cmdId, &kind, &cmdArg) != 0) return;
      applyControl(panelCtlConsume(&gCtl, cmdId, kind, cmdArg, &arg), arg);
      break;
    }
    case PANEL_FT_PING:
    default:
      break;   /* unknown types are skipped, never desync (protocol.h) */
    }
}

/* pumpSerial — drain whatever the host has sent into the frame parser.
 * The cap keeps one tick bounded; at the 2 s snapshot cadence the real load is
 * roughly 30 bytes per tick, so 1024 is ample headroom for a burst after a
 * reconnect without ever starving the render.                               */
static void pumpSerial(void) {
  uint8_t buf[256];
  uint32_t ms = nowMs();
  for (int burst = 0; burst < 4; burst++) {
    size_t n = 0;
    while (n < sizeof(buf)) {
      int c = getchar_timeout_us(0);
      if (c == PICO_ERROR_TIMEOUT) break;
      buf[n++] = (uint8_t)c;
    }
    if (!n) break;
    panelParserFeed(&gParser, buf, n, ms, onFrame, NULL);
    if (n < sizeof(buf)) break;
  }
}

/* pressTheme — DESIGN-BRIEF FINAL DECISIONS: "Buttons choose a language, not a
 * screen". Pressing the ACTIVE theme's button advances that theme's screens;
 * pressing a different theme's button switches to it at screen 0.           */
static void pressTheme(int theme) {
  gScreenT = 0.0f;
  if (gTheme == theme)
    gScreen = (int)panelScreenCfgNext(&gScreenCfg, (uint8_t)gTheme,
                                      (uint8_t)gScreen);
  else {
    gTheme = theme;
    /* CONTRACT-AW.md §4: choosing a theme may not land on a disabled tile. */
    gScreen = panelScreenCfgEnabled(&gScreenCfg, (uint8_t)theme, 0u) ? 0 :
              (int)panelScreenCfgNext(&gScreenCfg, (uint8_t)theme, 2u);
    sendEvent(PANEL_EV_THEMECHANGE, (uint8_t)theme);
  }
}

/* ackAlarm — acknowledge the CURRENT episode. The single acknowledge path:
 * both "any button while the alarm is armed" (CONTRACT §5, DESIGN-BRIEF
 * Ambiguity #4) and CONTROL ackAlarm (CONTRACT-CP §3 kind 5) come through here,
 * so the two can never drift. Ack is firmware-local and scoped to episodeId; a
 * new episodeId re-arms the tone even after this ran.
 * Input:  none. Output: none; emits EV_ACK when it actually silenced an alarm. */
static void ackAlarm(void) {
  if (!gAlarmArmed) return;
  gAckedEpisode = gEnv.snap.topAlert.episodeId;
  gHaveAcked = true;
  gAlarmArmed = false;
  gAlarmSilenced = false;
  panelHwToneOff();
  gToneNote = 3;
  sendEvent(PANEL_EV_ACK, 0);
}

/* handlePress — one debounced button-down edge.
 * CONTRACT §5 (Lead's recorded resolution of DESIGN-BRIEF Ambiguity #4): ANY
 * button press during an active alarm is consumed as acknowledge — it does not
 * also change theme, step brightness or toggle sleep.                       */
static void handlePress(PanelButton b) {
  sendEvent(PANEL_EV_BUTTON, (uint8_t)b);

  if (gAlarmArmed) { ackAlarm(); return; }
  if (gSleeping) { gSleeping = false; return; }   /* any button wakes */

  if (gHelp.active && b != PANEL_BTN_VOLUP) panelHelpDismiss(&gHelp);

  switch (b) {
    case PANEL_BTN_A: case PANEL_BTN_B:
    case PANEL_BTN_C: case PANEL_BTN_D:
      pressTheme((int)b);
      break;
    case PANEL_BTN_LUXUP:
      gAutoBright = false;
      panelHwSetBrightness(panelHwGetBrightness() + BRIGHT_STEP);
      break;
    case PANEL_BTN_LUXDN:
      gAutoBright = false;
      panelHwSetBrightness(panelHwGetBrightness() - BRIGHT_STEP);
      break;
    case PANEL_BTN_VOLUP:
      (void)panelHelpToggle(&gHelp, gT, false);
      break;
    case PANEL_BTN_VOLDN:
      panelHwSetVolume(panelHwGetVolume() - 0.1f);
      break;
    case PANEL_BTN_SLEEP:
      gSleeping = true;    /* DESIGN-BRIEF Ambiguity #3: ZZZ = sleep toggle */
      break;
    default:
      break;
  }
}

/* applyControl — perform one validated CONTROL action.
 * Input:  the action panelCtlConsume returned and its validated argument.
 * Output: none. PANEL_CTLACT_NONE is the normal no-op path (duplicate, unknown
 *         kind, or out-of-range arg) and deliberately does nothing at all.
 *
 * Every arm here routes into an existing physical-input path. Two deliberate
 * differences from handlePress, both because CONTROL carries explicit intent
 * where a button carries implicit intent:
 *   - No "any command acknowledges the alarm". The control page has its own ACK
 *     button (kind 5); silently swallowing a theme change as an ack would make
 *     the page's other controls dead whenever an alarm is up.
 *   - No "any command wakes the panel". Sleep is kind 7 with an explicit 0/1,
 *     so a theme change can be staged on a sleeping panel without lighting it.
 * Both are recorded in RETURN-CP3.md.                                         */
static void applyControl(PanelCtlAction act, uint8_t arg) {
  switch (act) {
    case PANEL_CTLACT_THEME:
      pressTheme((int)arg);   /* identical semantics to the A-D buttons */
      break;
    case PANEL_CTLACT_SCREEN:
      gScreen = (int)arg;
      gScreenT = 0.0f;        /* as a manual advance does: restart the dwell */
      break;
    case PANEL_CTLACT_BRIGHTNESS:
      gAutoBright = false;    /* latches manual, exactly as LUX+/- do */
      panelHwSetBrightness((float)arg / 100.0f);
      break;
    case PANEL_CTLACT_AUTOBRIGHT:
      gAutoBright = true;
      break;
    case PANEL_CTLACT_ACKALARM:
      ackAlarm();
      break;
    case PANEL_CTLACT_DWELL:
      gDwellSec = (float)arg;
      gScreenT = 0.0f;        /* never strand the panel past a shortened dwell */
      break;
    case PANEL_CTLACT_SLEEP:
      gSleeping = (arg != 0u);
      break;
    case PANEL_CTLACT_SCREENEN: {
      uint8_t index = (uint8_t)(arg >> 1);
      uint8_t value = (uint8_t)((gScreenCfg.screenCfg[index] & 0x0eu) |
                                (arg & 1u));
      if (panelScreenCfgSet(&gScreenCfg, index, value, nowMs())) sendConfig();
      break;
    }
    case PANEL_CTLACT_SCREENWT: {
      uint8_t index = (uint8_t)(arg >> 3);
      uint8_t value = (uint8_t)((gScreenCfg.screenCfg[index] & 1u) |
                                ((arg & 0x07u) << 1));
      uint8_t active = (uint8_t)(gTheme * 3 + gScreen);
      float oldDwell = panelScreenCfgDwellSec(&gScreenCfg, active, gDwellSec);
      if (panelScreenCfgSet(&gScreenCfg, index, value, nowMs())) {
        /* CONTRACT-AW §10.5: preserve the fraction already waited, then
         * advance immediately if the rescaled dwell has expired. */
        if (index == active && oldDwell > 0.0f) {
          float newDwell = panelScreenCfgDwellSec(&gScreenCfg, index, gDwellSec);
          int advanced = 0;
          /* Shared shipped implementation (review: main.c previously carried
           * an untested copy while the suite asserted a reimplementation). */
          gScreenT = panelScreenCfgRescaleDwell(gScreenT, oldDwell, newDwell,
                                                &advanced);
          if (advanced) {
            gScreen = (int)panelScreenCfgNext(&gScreenCfg, (uint8_t)gTheme,
                                              (uint8_t)gScreen);
          }
        }
        sendConfig();
      }
      break;
    }
    case PANEL_CTLACT_NONE:
    default:
      break;
  }
}

/* brightnessPct — the STATE field, from the hardware's own 0..1 scalar.
 * Input:  none. Output: 0..100, rounded.                                      */
static uint8_t brightnessPct(void) {
  float v = panelHwGetBrightness() * 100.0f + 0.5f;
  if (v < 0.0f) v = 0.0f;
  if (v > 100.0f) v = 100.0f;
  return (uint8_t)v;
}

/* reportState — emit PANEL_FT_STATE when panelCtl says one is due.
 * Input:  current ms. Output: none.
 *
 * alarmAcked reports whether the CURRENTLY SHOWN episode is acknowledged, not
 * merely whether an ack ever happened: gHaveAcked survives the episode it
 * belongs to, and a new episodeId re-arms (runAlarm), so the page must see the
 * ack fall away when the alarm changes. ackedEpisodeId carries the identity so
 * the server can match it against the episode it raised — CONTRACT-CP §10
 * acceptance 4 checks exactly that.                                           */
static void reportState(uint32_t ms) {
  PanelCtlState st;
  uint8_t payload[PANEL_STATE_SIZE];
  size_t n;

  st.theme          = (uint8_t)gTheme;
  st.screen         = (uint8_t)gScreen;
  st.brightnessPct  = brightnessPct();
  st.autoBright     = gAutoBright ? 1u : 0u;
  st.sleeping       = gSleeping ? 1u : 0u;
  st.alarmArmed     = gAlarmArmed ? 1u : 0u;
  st.alarmAcked     = (gHaveAcked &&
                       gAckedEpisode == gEnv.snap.topAlert.episodeId) ? 1u : 0u;
  st.dwellSec       = (uint8_t)gDwellSec;
  st.ackedEpisodeId = gHaveAcked ? gAckedEpisode : 0u;

  if (!panelCtlPoll(&gCtl, &st, ms)) return;

  /* dwellSec occupies STATE byte 7 (v1.1 CP amendment; was reserved) so the
   * page's dwell control can confirm. It is a plain codec argument — this side
   * never touches wire offsets. gDwellSec is only ever 3/6/30, so the float
   * truncation above is exact and 0 ("unreported") is unreachable here. */
  n = panelEncodeState(st.theme, st.screen, st.brightnessPct, st.autoBright,
                       st.sleeping, st.alarmArmed, st.alarmAcked, st.dwellSec,
                       st.ackedEpisodeId, panelCtlLastCmdId(&gCtl),
                       payload, sizeof(payload));
  if (n) sendFrame(PANEL_FT_STATE, payload, n);
}

/* scanButtons — 2-tick (80 ms) debounce, rising edges only. */
static void scanButtons(void) {
  for (int i = 0; i < PANEL_BTN_COUNT; i++) {
    bool raw = panelHwButton((PanelButton)i);
    if (raw == (bool)gBtnStable[i]) { gBtnCount[i] = 0; continue; }
    if (++gBtnCount[i] >= DEBOUNCE_TICKS) {
      gBtnStable[i] = raw ? 1 : 0;
      gBtnCount[i] = 0;
      if (raw) handlePress((PanelButton)i);
    }
  }
}

/* runAlarm — arm/re-arm/tone. CONTRACT §9: alarmActive is server-computed and
 * the firmware never recomputes score; episodeId is the re-arm key, and ack is
 * firmware-local per episode. */
static void runAlarm(void) {
  bool wantAlarm = gEnv.haveData && gEnv.alarmActive && !gLink.lost;
  uint32_t episode = gEnv.snap.topAlert.episodeId;

  if (wantAlarm) {
    bool ackedThis = gHaveAcked && gAckedEpisode == episode;
    if (!ackedThis && !gAlarmArmed) {
      /* Rising edge, or a NEW episodeId after an acknowledged one. */
      gAlarmArmed = true;
      gAlarmSilenced = false;
      gAlarmRaisedAt = gT;
      gAlarmToneAt = gT - REALARM_SEC;   /* sound immediately */
      /* The rising edge wakes the board ONCE. It used to re-assert every tick,
       * which made an unacknowledged episode pin the panel awake indefinitely —
       * incompatible with the amendment's overnight case, where the tone stops
       * after 5 minutes and the beacon is meant to be what carries the fault on
       * a sleeping panel. Recorded as a deviation in RETURN-C3.md.           */
      gSleeping = false;
    }
    if (ackedThis) { gAlarmArmed = false; gAlarmSilenced = false; }
  } else {
    if (gAlarmArmed) { panelHwToneOff(); gToneNote = 3; }
    gAlarmArmed = false;
    gAlarmSilenced = false;
  }

  if (!gAlarmArmed) return;

  /* Auto-silence: one-way, per episode, tone only. Cleared exclusively by an
   * ack, by the alarm clearing, or by a new episodeId — all three above. */
  if (!gAlarmSilenced && gT - gAlarmRaisedAt >= AUTOSILENCE_SEC) {
    gAlarmSilenced = true;
    panelHwToneOff();
    gToneNote = 3;
  }
  if (gAlarmSilenced) return;

  /* DESIGN-BRIEF "Animation": 3-note square triad at offsets 0 / 0.22 / 0.44 s,
   * frequencies 990 / 660 / 990 Hz, each note 200 ms. The triad itself is
   * unchanged; only its repeat interval moved to REALARM_SEC (60 s). */
  if (gT - gAlarmToneAt >= REALARM_SEC) { gAlarmToneAt = gT; gToneNote = 0; }
  static const float noteAt[3]  = { 0.0f, 0.22f, 0.44f };
  static const uint16_t noteHz[3] = { 990, 660, 990 };
  float since = gT - gAlarmToneAt;
  if (gToneNote < 3 && since >= noteAt[gToneNote]) {
    panelHwToneOn(noteHz[gToneNote]);
    gToneNote++;
  }
  if (gToneNote >= 1 && gToneNote <= 3 && since >= noteAt[gToneNote - 1] + 0.2f)
    panelHwToneOff();
}

/* runAutoBrightness — DESIGN-BRIEF Ambiguity #5 / CONTRACT §5: light-sensor
 * auto-brightness is a SHOULD and LUX+/- overrides it. The phototransistor
 * reads 0..4095; it is mapped onto the design's own 25%..100% range and heavily
 * smoothed so a passing shadow does not visibly step the board. */
static void runAutoBrightness(void) {
  if (!gAutoBright) return;
  /* The raw reading was previously normalised against the ADC's full 4095, so
   * the panel only reached full brightness with the sensor pegged — which a
   * phototransistor in a divider realistically never is. In this panel's actual
   * home (family room, beside floor-to-ceiling windows, direct daylight on the
   * board) that put the NORMAL condition well short of maximum. The reference
   * is now PANEL_LUX_FULL, the raw reading at which the panel goes to 1.0, and
   * anything brighter clamps there. See UNVERIFIED in RETURN-C3.md: the real
   * daylight reading has not been measured, so this constant is the one knob to
   * turn if the panel still looks held back in the sun. */
  float lux = (float)panelHwLight() / PANEL_LUX_FULL;
  if (lux < 0.0f) lux = 0.0f;
  if (lux > 1.0f) lux = 1.0f;
  float target = 0.25f + 0.75f * sqrtf(lux);
  /* Asymmetric: brighten quickly (~0.5 s) so the panel is never caught dim when
   * the room lights up, dim slowly (~4 s) so a passing shadow or someone
   * walking between the window and the board does not visibly pump it. */
  float k = (target > gAutoBrightSmoothed) ? 0.08f : 0.01f;
  gAutoBrightSmoothed += (target - gAutoBrightSmoothed) * k;
  panelHwSetBrightness(gAutoBrightSmoothed);
}

int main(void) {
  stdio_init_all();
  panelHwInit();
  panelEnvInit(&gEnv);
  panelParserInit(&gParser);
  panelLinkInit(&gLink, nowMs());
  panelCtlInit(&gCtl, nowMs());
  panelHelpInit(&gHelp);
#ifdef SOLARI_PANEL_PICO_FLASH
  /* Review S3: flash_safe_execute_core_init() initialises the CALLING core
   * as a lockout VICTIM — it belongs on the core that is NOT flashing.
   * This firmware never launches core 1, so no victim init is required at
   * all; the SDK's other-core check passes trivially. If a core-1 task is
   * ever added, call flash_safe_execute_core_init() FROM CORE 1, not here. */
#endif
  gScreenCfgFlash = panelScreenCfgDeviceFlash();
  panelScreenCfgLoad(&gScreenCfg, &gScreenCfgFlash, nowMs());

  sendHello();

  absolute_time_t next = make_timeout_time_ms(TICK_MS);
  uint32_t prevMs = nowMs();

  for (;;) {
    sleep_until(next);
    next = delayed_by_ms(next, TICK_MS);

    uint32_t ms = nowMs();
    float dt = (float)(ms - prevMs) / 1000.0f;
    prevMs = ms;
    if (dt > 0.2f) dt = 0.2f;   /* prototype's clamp: never jump the anims */
    gT += dt;

    pumpSerial();

    /* Sample the clock AFTER the pump: a frame parsed during this tick stamps
     * its arrival later than `ms`, and feeding a stale `ms` here is precisely
     * what produced one LINKLOST/LINKBACK pair per snapshot in build dbd33885.
     * panelLinkPoll now tolerates that too (signed compare), but there is no
     * reason to hand it a timestamp we know to be behind. */
    switch (panelLinkPoll(&gLink, nowMs())) {
      case PANEL_LINK_WENT_LOST: sendEvent(PANEL_EV_LINKLOST, 0); break;
      case PANEL_LINK_CAME_BACK: sendEvent(PANEL_EV_LINKBACK, 0); break;
      default: break;
    }

    scanButtons();
    runAlarm();
    panelHelpTick(&gHelp, gT, gAlarmArmed);
    runAutoBrightness();

    if (!gHelp.active) {
      if (!gLink.lost) gScreenT += dt;
      if (gScreenT >= panelScreenCfgDwellSec(&gScreenCfg,
                                              (uint8_t)(gTheme * 3 + gScreen),
                                              gDwellSec)) {
        gScreenT = 0.0f;
        gScreen = (int)panelScreenCfgNext(&gScreenCfg, (uint8_t)gTheme,
                                          (uint8_t)gScreen);
      }
    }
    if (panelScreenCfgSaveDue(&gScreenCfg, &gScreenCfgFlash, ms)) sendConfig();

    /* After every state-moving step of the tick and before the paint, so a
     * command applied this tick is reported this tick — and so the sleeping
     * short-circuit below cannot skip the heartbeat. */
    reportState(nowMs());

    panelFbClear();
    if (gSleeping) {
      /* Blanked, but the tick keeps running — and an unacknowledged episode
       * still shows its beacon on the otherwise dark panel. */
      if (gAlarmArmed) panelBeacon(gT);
      panelFbFlush();
      continue;
    }

    if (gHelp.active) {
      panelHelpOverlayDraw((unsigned)(gTheme * 3 + gScreen), gT);
    } else if (gLink.lost) {
      /* Keep the last good picture underneath so the operator still sees the
       * fleet, then lay the link treatment over it. */
      kScreens[gTheme][gScreen](&gEnv, gT, dt);
      panelScreenLinkLost(gT);
    } else if (!gEnv.haveData || gEnv.total == 0) {
      panelScreenNoData(gT);
    } else {
      kScreens[gTheme][gScreen](&gEnv, gT, dt);
      if (gAlarmArmed) panelInlay(&gEnv, gT);
    }
    /* Beacon last of all: it must sit on top of the inlay, whose right-hand
     * rail runs through x=52. Placed outside the branch so it also covers the
     * zero-node NO DATA case, which draws no inlay. (LINK LOST cannot coexist
     * with an armed alarm — runAlarm() clears it.)                          */
    if (gAlarmArmed) panelBeacon(gT);
    panelFbFlush();
  }
}
