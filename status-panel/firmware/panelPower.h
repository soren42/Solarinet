/*
 * panelPower.h — VBUS ride-through state (CONTRACT-SW §7, D15/D16/D18).
 *
 * Pure, host-testable: no SDK includes. The device tick samples WL_GPIO2 and
 * feeds the raw level into panelPowerPoll(); this module owns the debounce,
 * the power-loss episode allocation, and the on-battery flags that drive the
 * brightness cap and the corner glyph. It does NOT own tone or acknowledge
 * logic — the alarm state machine in main.c composes power episodes with the
 * server's topAlert episode (the same no-second-copy rule CONTROL follows).
 *
 * Episode identity (D15): 0xC0000000 | (edgeCounter & 0x3FFFFFFF), where
 * edgeCounter starts from a per-boot random salt and increments on every
 * debounced VBUS-loss edge. Two outages in one boot are two episodes; an
 * acknowledge of the first cannot mute the second. A server SNAPSHOT never
 * clears a power episode; VBUS restore clears ONLY the power episode.
 */

#ifndef SOLARI_PANEL_POWER_H
#define SOLARI_PANEL_POWER_H

#include <stdbool.h>
#include <stdint.h>

/* D18: 100 ms debounce, both edges — an edge commits on the first sample
 * taken >= 100 ms after the raw level moved (the fourth 40 ms tick sample;
 * the third sits at 80 ms). Any flap back restarts the window. */
#define PANEL_POWER_DEBOUNCE_MS 100u

/* D15 episode namespace: high bits mark "power-loss, firmware-local". */
#define PANEL_POWER_EPISODE_BASE 0xC0000000u
#define PANEL_POWER_EPISODE_MASK 0x3FFFFFFFu

typedef enum {
  PANEL_POWER_STEADY  = 0,
  PANEL_POWER_LOSS    = 1,   /* debounced VBUS-loss edge: new episode */
  PANEL_POWER_RESTORE = 2    /* debounced VBUS-restore edge: episode cleared */
} PanelPowerEdge;

typedef struct {
  bool     vbusStable;    /* debounced VBUS-present level                  */
  bool     vbusRaw;       /* last raw sample                               */
  uint32_t rawChangedMs;  /* when the raw level last diverged from stable  */
  bool     onBattery;     /* == !vbusStable, the cap/glyph driver          */
  uint32_t edgeCounter;   /* salt-seeded; ++ per debounced loss edge       */
  uint32_t episodeId;     /* current power episode (valid while onBattery) */
  bool     acked;         /* this episode acknowledged (ack-all applies)   */
} PanelPower;

/* panelPowerInit — seed from the per-boot salt and the raw VBUS level at
 * boot. D18: boot-on-battery IS a loss at t=0 — no debounce wait, the first
 * episode is allocated immediately so cap + glyph + tone precede the first
 * snapshot ever arriving. */
void panelPowerInit(PanelPower *p, uint32_t salt, bool vbusRaw, uint32_t nowMs);

/* panelPowerPoll — feed one raw VBUS sample. Returns the debounced edge this
 * sample committed, if any. A loss edge allocates a fresh episode (unacked);
 * a restore edge clears the power episode — and nothing else (D16). */
PanelPowerEdge panelPowerPoll(PanelPower *p, bool vbusRaw, uint32_t nowMs);

/* panelPowerAck — the ack-all hook: main.c's single ackAlarm() path calls
 * this alongside recording the server episode, so one press acknowledges
 * every currently-unacked source (D15). No-op when not on battery. */
void panelPowerAck(PanelPower *p);

/* panelPowerAlarmPending — true while an UNACKED power episode is live; the
 * OR-composition input to the alarm state machine. */
bool panelPowerAlarmPending(const PanelPower *p);

#endif /* SOLARI_PANEL_POWER_H */
