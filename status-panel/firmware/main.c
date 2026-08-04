/*
 * main.c — solari-panel-fw core loop.
 *
 * Responsibilities, in the order the tick performs them:
 *   1. drain USB-CDC bytes into the shared frame parser (protocol.c)
 *   2. apply any complete SNAPSHOT, refreshing PanelEnv and its history rings
 *   3. sample buttons and run the interaction state machine
 *   4. run the alarm state machine (arm / re-alarm / acknowledge)
 *   5. paint the active screen, then the inlay if the alarm is live
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

#include "protocol.h"
#include "panelHw.h"
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
#define DWELL_SEC        6.0f    /* DESIGN-BRIEF: dwell default 6 s          */
#define LINK_TIMEOUT_MS  15000u  /* CONTRACT §4: >15 s with no valid frame   */
/* OPERATOR AMENDMENT 2026-08-04, superseding the DESIGN-BRIEF's 12 s re-alarm:
 * the tone repeats every 60 s, and an episode left unacknowledged for 5 minutes
 * stops re-sounding permanently. Auto-silence is NOT an acknowledge — the inlay
 * and the beacon both stay up, and a new episodeId re-arms sound as normal.  */
#define REALARM_SEC      60.0f   /* was 12 s (DESIGN-BRIEF "Animation")       */
#define AUTOSILENCE_SEC  300.0f  /* unacked for this long -> tone off, once   */
#define BRIGHT_STEP      0.05f   /* DESIGN-BRIEF Ambiguity #3: 5% steps      */
#define DEBOUNCE_TICKS   2       /* 80 ms at the 25 Hz tick                  */

/* ---- panel state -------------------------------------------------------- */
static PanelEnv    gEnv;
static PanelParser gParser;

static int   gTheme = 3;       /* CONTRACT §5: boot = Theme D ...            */
static int   gScreen = 1;      /* ... screen 2 of 3, the "resting face"      */
static float gT = 0.0f;        /* seconds since boot, the animation clock    */
static float gScreenT = 0.0f;  /* dwell accumulator                          */

static bool  gSleeping = false;
static bool  gAutoBright = true;   /* SHOULD; LUX+/- latches manual control  */
static float gAutoBrightSmoothed = 0.85f;

static uint32_t gLastFrameMs = 0;
static bool     gLinkLost = false;
static bool     gHaveSeq = false;
static uint16_t gLastSeq = 0;

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

static void sendEvent(uint8_t kind, uint8_t arg) {
  uint8_t p[2] = { kind, arg };
  sendFrame(PANEL_FT_EVENT, p, sizeof(p));
}

/* onFrame — parser callback, fires once per CRC-valid frame.
 * Applies the protocol.h receiver rules: a snapshot whose seq equals the last
 * applied seq is a duplicate and ignored, and so is an older one — both via
 * panelSeqNewer() in the shared codec, which is RFC1982 wraparound-aware.   */
static void onFrame(uint8_t type, const uint8_t *payload, size_t len, void *user) {
  (void)user;
  gLastFrameMs = nowMs();     /* ANY valid frame, PING included, is liveness */

  switch (type) {
    case PANEL_FT_SNAPSHOT: {
      PanelSnapshot snap;
      if (panelDecodeSnapshot(payload, len, &snap) != 0) return;
      /* The ordering test lives in the shared codec so firmware and daemon
       * cannot disagree about it. Do not hand-roll the comparison here. */
      if (gHaveSeq && !panelSeqNewer(snap.seq, gLastSeq)) return;
      gLastSeq = snap.seq;
      gHaveSeq = true;
      panelEnvApply(&gEnv, &snap);
      break;
    }
    case PANEL_FT_HELLOREQ:
      sendHello();
      break;
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
  if (gTheme == theme) gScreen = (gScreen + 1) % 3;
  else { gTheme = theme; gScreen = 0; sendEvent(PANEL_EV_THEMECHANGE, (uint8_t)theme); }
}

/* handlePress — one debounced button-down edge.
 * CONTRACT §5 (Lead's recorded resolution of DESIGN-BRIEF Ambiguity #4): ANY
 * button press during an active alarm is consumed as acknowledge — it does not
 * also change theme, step brightness or toggle sleep.                       */
static void handlePress(PanelButton b) {
  sendEvent(PANEL_EV_BUTTON, (uint8_t)b);

  if (gAlarmArmed) {
    gAckedEpisode = gEnv.snap.topAlert.episodeId;
    gHaveAcked = true;
    gAlarmArmed = false;
    gAlarmSilenced = false;
    panelHwToneOff();
    gToneNote = 3;
    sendEvent(PANEL_EV_ACK, 0);
    return;
  }
  if (gSleeping) { gSleeping = false; return; }   /* any button wakes */

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
      panelHwSetVolume(panelHwGetVolume() + 0.1f);
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
  bool wantAlarm = gEnv.haveData && gEnv.alarmActive && !gLinkLost;
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
  float lux = (float)panelHwLight() / 4095.0f;
  if (lux < 0.0f) lux = 0.0f;
  if (lux > 1.0f) lux = 1.0f;
  float target = 0.25f + 0.75f * sqrtf(lux);
  gAutoBrightSmoothed += (target - gAutoBrightSmoothed) * 0.01f;
  panelHwSetBrightness(gAutoBrightSmoothed);
}

int main(void) {
  stdio_init_all();
  panelHwInit();
  panelEnvInit(&gEnv);
  panelParserInit(&gParser);
  gLastFrameMs = nowMs();

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

    /* CONTRACT §4: >15 s without any valid frame is LINK LOST; recovery is
     * announced too so the daemon journal shows both edges. */
    bool lost = (ms - gLastFrameMs) > LINK_TIMEOUT_MS;
    if (lost && !gLinkLost)      { gLinkLost = true;  sendEvent(PANEL_EV_LINKLOST, 0); }
    else if (!lost && gLinkLost) { gLinkLost = false; sendEvent(PANEL_EV_LINKBACK, 0); }

    scanButtons();
    runAlarm();
    runAutoBrightness();

    if (!gLinkLost) gScreenT += dt;
    if (gScreenT >= DWELL_SEC) { gScreenT = 0.0f; gScreen = (gScreen + 1) % 3; }

    panelFbClear();
    if (gSleeping) {
      /* Blanked, but the tick keeps running — and an unacknowledged episode
       * still shows its beacon on the otherwise dark panel. */
      if (gAlarmArmed) panelBeacon(gT);
      panelFbFlush();
      continue;
    }

    if (gLinkLost) {
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
