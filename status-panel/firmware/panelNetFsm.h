/*
 * panelNetFsm.h — platform-free standalone WiFi transport state machine.
 *
 * The firmware owns policy and timing here. CYW43, lwIP, SNTP and mbedTLS
 * remain behind PanelNetOps, allowing the same state machine to run under the
 * host test suite without SDK headers or a network.
 */

#ifndef SOLARI_PANEL_NET_FSM_H
#define SOLARI_PANEL_NET_FSM_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define PANEL_NET_USB_SILENCE_MS 15000u
#define PANEL_NET_BACKOFF_MIN_MS 1000u
#define PANEL_NET_BACKOFF_MAX_MS 60000u

typedef enum {
  PANEL_NET_OFF = 0,
  PANEL_NET_JOINING,
  PANEL_NET_ADDRESSING,
  PANEL_NET_TIMESYNC,
  PANEL_NET_CONNECTING,
  PANEL_NET_LINKED,
  PANEL_NET_BACKOFF,
  PANEL_NET_SUSPENDED
} PanelNetState;

typedef struct PanelNetOps {
  void (*startJoin)(void *user);
  void (*startAddressing)(void *user);
  void (*startTimeSync)(void *user);
  void (*startTls)(void *user);
  void (*close)(void *user);
  uint8_t (*randByte)(void *user);
  void *user;
} PanelNetOps;

typedef struct PanelNetFsm {
  PanelNetOps ops;
  PanelNetState state;
  PanelNetState retryState;
  uint64_t buildEpoch;
  uint32_t deadlineMs;
  uint32_t lastUsbMs;
  uint32_t backoffBaseMs;
  uint32_t lastBackoffMs;
  bool provisioned;
  bool timeValid;
  bool usbActive;
  bool actionPending;
} PanelNetFsm;

/* Seed policy inputs. wallEpoch is considered valid only when timeValid is
 * true AND it is later than buildEpoch (CONTRACT-SW D7). */
void panelNetFsmInit(PanelNetFsm *fsm, const PanelNetOps *ops,
                     bool provisioned, bool timeValid, uint64_t wallEpoch,
                     uint64_t buildEpoch);

/* Drive one pending platform action or an expired backoff/suspend deadline. */
void panelNetFsmPoll(PanelNetFsm *fsm, uint32_t nowMs);

/* Credential commit/wipe notification. An invalid store always returns OFF. */
void panelNetFsmSetProvisioned(PanelNetFsm *fsm, bool provisioned);

/* Completion events for the asynchronous operations in PanelNetOps. */
void panelNetFsmJoinResult(PanelNetFsm *fsm, bool success, uint32_t nowMs);
void panelNetFsmAddressResult(PanelNetFsm *fsm, bool success, uint32_t nowMs);
void panelNetFsmTimeResult(PanelNetFsm *fsm, bool success,
                           uint64_t wallEpoch, uint32_t nowMs);
void panelNetFsmTlsResult(PanelNetFsm *fsm, bool success, uint32_t nowMs);
void panelNetFsmApLost(PanelNetFsm *fsm, uint32_t nowMs);
void panelNetFsmServerClosed(PanelNetFsm *fsm, uint32_t nowMs);

/* Called only after CRC validation. typeKnown is true exactly for SNAPSHOT,
 * PING, HELLOREQ, CONTROL and PROVISION (CONTRACT-SW D14). */
void panelNetFsmUsbFrameSeen(PanelNetFsm *fsm, bool typeKnown,
                             uint32_t nowMs);
void panelNetFsmCdcDisconnected(PanelNetFsm *fsm, uint32_t nowMs);

#endif /* SOLARI_PANEL_NET_FSM_H */
