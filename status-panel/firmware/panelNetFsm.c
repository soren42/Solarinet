/*
 * panelNetFsm.c — platform-free standalone WiFi transport state machine.
 *
 * No operation allocates. All platform work is initiated through PanelNetOps;
 * result callbacks feed completion back into this deterministic policy core.
 */

#include "panelNetFsm.h"

#include <string.h>

static bool deadlineReached(uint32_t nowMs, uint32_t deadlineMs) {
  return (int32_t)(nowMs - deadlineMs) >= 0;
}

static void enterState(PanelNetFsm *fsm, PanelNetState state) {
  fsm->state = state;
  fsm->actionPending = state == PANEL_NET_JOINING ||
                       state == PANEL_NET_ADDRESSING ||
                       state == PANEL_NET_TIMESYNC ||
                       state == PANEL_NET_CONNECTING;
}

static void closeTransport(PanelNetFsm *fsm) {
  if (fsm->ops.close != NULL) fsm->ops.close(fsm->ops.user);
}

/* D20: bounded exponential backoff. Jitter is additive in [0, base/4] until
 * the cap, then subtractive in [0, cap/4] so retries remain jittered rather
 * than collapsing to a synchronized constant 60 s interval at the ceiling. */
static void enterBackoff(PanelNetFsm *fsm, PanelNetState retryState,
                         uint32_t nowMs) {
  uint32_t jitterMax = fsm->backoffBaseMs / 4u;
  uint32_t jitter = 0u;
  if (jitterMax != 0u && fsm->ops.randByte != NULL) {
    jitter = ((uint32_t)fsm->ops.randByte(fsm->ops.user) * jitterMax) / 255u;
  }
  fsm->lastBackoffMs = fsm->backoffBaseMs == PANEL_NET_BACKOFF_MAX_MS ?
                       fsm->backoffBaseMs - jitter :
                       fsm->backoffBaseMs + jitter;
  if (fsm->lastBackoffMs > PANEL_NET_BACKOFF_MAX_MS)
    fsm->lastBackoffMs = PANEL_NET_BACKOFF_MAX_MS;
  fsm->deadlineMs = nowMs + fsm->lastBackoffMs;
  fsm->retryState = retryState;
  fsm->state = PANEL_NET_BACKOFF;
  fsm->actionPending = false;
  if (fsm->backoffBaseMs < PANEL_NET_BACKOFF_MAX_MS) {
    uint32_t next = fsm->backoffBaseMs * 2u;
    fsm->backoffBaseMs = next > PANEL_NET_BACKOFF_MAX_MS ?
                         PANEL_NET_BACKOFF_MAX_MS : next;
  }
}

static void failFrom(PanelNetFsm *fsm, PanelNetState expected,
                     PanelNetState retryState, uint32_t nowMs) {
  if (fsm == NULL || fsm->state != expected) return;
  closeTransport(fsm);
  enterBackoff(fsm, retryState, nowMs);
}

void panelNetFsmInit(PanelNetFsm *fsm, const PanelNetOps *ops,
                     bool provisioned, bool timeValid, uint64_t wallEpoch,
                     uint64_t buildEpoch) {
  if (fsm == NULL) return;
  memset(fsm, 0, sizeof(*fsm));
  if (ops != NULL) fsm->ops = *ops;
  fsm->buildEpoch = buildEpoch;
  fsm->provisioned = provisioned;
  fsm->timeValid = timeValid && wallEpoch > buildEpoch;
  fsm->backoffBaseMs = PANEL_NET_BACKOFF_MIN_MS;
  enterState(fsm, provisioned ? PANEL_NET_JOINING : PANEL_NET_OFF);
}

void panelNetFsmPoll(PanelNetFsm *fsm, uint32_t nowMs) {
  if (fsm == NULL) return;
  if (fsm->state == PANEL_NET_BACKOFF &&
      deadlineReached(nowMs, fsm->deadlineMs)) {
    enterState(fsm, fsm->retryState);
  } else if (fsm->state == PANEL_NET_SUSPENDED && !fsm->usbActive) {
    enterState(fsm, PANEL_NET_JOINING);
  } else if (fsm->state == PANEL_NET_SUSPENDED &&
             deadlineReached(nowMs, fsm->lastUsbMs +
                                      PANEL_NET_USB_SILENCE_MS)) {
    fsm->usbActive = false;
    enterState(fsm, PANEL_NET_JOINING);
  }

  if (!fsm->actionPending) return;
  fsm->actionPending = false;
  switch (fsm->state) {
    case PANEL_NET_JOINING:
      if (fsm->ops.startJoin != NULL) fsm->ops.startJoin(fsm->ops.user);
      break;
    case PANEL_NET_ADDRESSING:
      if (fsm->ops.startAddressing != NULL)
        fsm->ops.startAddressing(fsm->ops.user);
      break;
    case PANEL_NET_TIMESYNC:
      if (fsm->ops.startTimeSync != NULL)
        fsm->ops.startTimeSync(fsm->ops.user);
      break;
    case PANEL_NET_CONNECTING:
      /* D7: a stale/missing wall clock can never reach the TLS seam. */
      if (!fsm->timeValid) {
        enterState(fsm, PANEL_NET_TIMESYNC);
        panelNetFsmPoll(fsm, nowMs);
      } else if (fsm->ops.startTls != NULL) {
        fsm->ops.startTls(fsm->ops.user);
      }
      break;
    default:
      break;
  }
}

void panelNetFsmSetProvisioned(PanelNetFsm *fsm, bool provisioned) {
  if (fsm == NULL) return;
  fsm->provisioned = provisioned;
  if (!provisioned) {
    if (fsm->state != PANEL_NET_OFF) closeTransport(fsm);
    fsm->usbActive = false;
    enterState(fsm, PANEL_NET_OFF);
  } else if (fsm->state == PANEL_NET_OFF) {
    /* A PROVISION commit is itself qualifying USB activity under D14. The
     * usual caller reports that frame first, so do not start WiFi underneath
     * the still-active provisioning session. */
    enterState(fsm, fsm->usbActive ? PANEL_NET_SUSPENDED : PANEL_NET_JOINING);
  }
}

void panelNetFsmJoinResult(PanelNetFsm *fsm, bool success, uint32_t nowMs) {
  if (fsm == NULL || fsm->state != PANEL_NET_JOINING) return;
  if (success) enterState(fsm, PANEL_NET_ADDRESSING);
  else failFrom(fsm, PANEL_NET_JOINING, PANEL_NET_JOINING, nowMs);
}

void panelNetFsmAddressResult(PanelNetFsm *fsm, bool success, uint32_t nowMs) {
  if (fsm == NULL || fsm->state != PANEL_NET_ADDRESSING) return;
  if (success)
    enterState(fsm, fsm->timeValid ? PANEL_NET_CONNECTING : PANEL_NET_TIMESYNC);
  else
    failFrom(fsm, PANEL_NET_ADDRESSING, PANEL_NET_ADDRESSING, nowMs);
}

void panelNetFsmTimeResult(PanelNetFsm *fsm, bool success,
                           uint64_t wallEpoch, uint32_t nowMs) {
  if (fsm == NULL || fsm->state != PANEL_NET_TIMESYNC) return;
  fsm->timeValid = success && wallEpoch > fsm->buildEpoch;
  if (fsm->timeValid) enterState(fsm, PANEL_NET_CONNECTING);
  else failFrom(fsm, PANEL_NET_TIMESYNC, PANEL_NET_TIMESYNC, nowMs);
}

void panelNetFsmTlsResult(PanelNetFsm *fsm, bool success, uint32_t nowMs) {
  if (fsm == NULL || fsm->state != PANEL_NET_CONNECTING) return;
  if (success) {
    fsm->state = PANEL_NET_LINKED;
    fsm->actionPending = false;
    fsm->backoffBaseMs = PANEL_NET_BACKOFF_MIN_MS;
  } else {
    failFrom(fsm, PANEL_NET_CONNECTING, PANEL_NET_CONNECTING, nowMs);
  }
}

void panelNetFsmApLost(PanelNetFsm *fsm, uint32_t nowMs) {
  if (fsm == NULL || fsm->state == PANEL_NET_OFF ||
      fsm->state == PANEL_NET_SUSPENDED ||
      fsm->state == PANEL_NET_BACKOFF) return;
  closeTransport(fsm);
  enterBackoff(fsm, PANEL_NET_JOINING, nowMs);
}

void panelNetFsmServerClosed(PanelNetFsm *fsm, uint32_t nowMs) {
  if (fsm == NULL || (fsm->state != PANEL_NET_LINKED &&
                      fsm->state != PANEL_NET_CONNECTING)) return;
  closeTransport(fsm);
  enterBackoff(fsm, PANEL_NET_CONNECTING, nowMs);
}

void panelNetFsmUsbFrameSeen(PanelNetFsm *fsm, bool typeKnown,
                             uint32_t nowMs) {
  if (fsm == NULL || !typeKnown) return;
  /* D14: first CRC-valid KNOWN host->panel frame closes WiFi immediately.
   * Unknown CRC-valid types deliberately do not refresh this clock. */
  fsm->lastUsbMs = nowMs;
  fsm->usbActive = true;
  if (fsm->state != PANEL_NET_OFF && fsm->state != PANEL_NET_SUSPENDED) {
    closeTransport(fsm);
    fsm->state = PANEL_NET_SUSPENDED;
    fsm->actionPending = false;
  }
}

void panelNetFsmCdcDisconnected(PanelNetFsm *fsm, uint32_t nowMs) {
  (void)nowMs;
  if (fsm == NULL) return;
  fsm->usbActive = false;
  if (fsm->state == PANEL_NET_SUSPENDED && fsm->provisioned)
    enterState(fsm, PANEL_NET_JOINING);
}
