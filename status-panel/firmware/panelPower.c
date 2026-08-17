/*
 * panelPower.c — VBUS debounce + power-loss episode allocation (D15/D18).
 * Pure C, covered by test/panelPowerTest.c on the host.
 */

#include "panelPower.h"

/* newEpisode — allocate the next power episode id from the salted counter.
 * The mask keeps the id inside the 0xC0000000 firmware-local namespace across
 * counter wraparound (D15). */
static void newEpisode(PanelPower *p) {
  p->episodeId = PANEL_POWER_EPISODE_BASE |
                 (p->edgeCounter & PANEL_POWER_EPISODE_MASK);
  p->edgeCounter++;
  p->acked = false;
}

void panelPowerInit(PanelPower *p, uint32_t salt, bool vbusRaw, uint32_t nowMs) {
  p->vbusStable   = vbusRaw;
  p->vbusRaw      = vbusRaw;
  p->rawChangedMs = nowMs;
  p->onBattery    = !vbusRaw;
  p->edgeCounter  = salt;
  p->episodeId    = 0u;
  p->acked        = false;
  if (p->onBattery) newEpisode(p);   /* D18: boot-on-battery = loss at t=0 */
}

PanelPowerEdge panelPowerPoll(PanelPower *p, bool vbusRaw, uint32_t nowMs) {
  if (vbusRaw != p->vbusRaw) {
    p->vbusRaw = vbusRaw;
    p->rawChangedMs = nowMs;         /* level moved: restart the window */
  }
  if (vbusRaw == p->vbusStable) return PANEL_POWER_STEADY;
  /* Signed wrap-safe compare, same idiom as panelLink/panelNetFsm. */
  if ((int32_t)(nowMs - p->rawChangedMs) < (int32_t)PANEL_POWER_DEBOUNCE_MS)
    return PANEL_POWER_STEADY;

  p->vbusStable = vbusRaw;
  p->onBattery = !vbusRaw;
  if (p->onBattery) {
    newEpisode(p);
    return PANEL_POWER_LOSS;
  }
  /* D16: restore clears ONLY the power episode — acked or not. */
  p->episodeId = 0u;
  p->acked = false;
  return PANEL_POWER_RESTORE;
}

void panelPowerAck(PanelPower *p) {
  if (p->onBattery) p->acked = true;
}

bool panelPowerAlarmPending(const PanelPower *p) {
  return p->onBattery && !p->acked;
}

bool panelAlarmWant(bool serverUnacked, const PanelPower *p) {
  return serverUnacked || panelPowerAlarmPending(p);
}
