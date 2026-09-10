/*
 * panelNet.h — CYW43/lwIP/mbedTLS adapter for PanelNetFsm.
 *
 * The public surface intentionally contains no SDK types.  This keeps the
 * policy helpers available to the host suite while all device state remains
 * private to panelNet.c.
 */

#ifndef SOLARI_PANEL_NET_H
#define SOLARI_PANEL_NET_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "panelLink.h"
#include "panelNetFsm.h"
#include "panelProv.h"
#include "panelProvStore.h"
#include "../protocol.h"

typedef struct PanelNet PanelNet;

/* Platform-free contract decisions, host-tested in panelNetTest.c. */
bool panelNetKnownHostType(uint8_t type);
bool panelNetEpochPlausible(uint64_t wallEpoch, uint64_t buildEpoch);
bool panelNetUseWifiTx(PanelNetState state, bool tlsUp);

/* D18: initializes CYW43 unconditionally, even when store is invalid. */
PanelNet *panelNetInit(const PanelProvStore *store, PanelParser *wifiParser,
                       PanelFrameCb frameCb, void *frameUser,
                       PanelLink *link);
void panelNetAttachFsm(PanelNet *net, PanelNetFsm *fsm);
PanelNetOps panelNetOps(PanelNet *net);
void panelNetCredentialsChanged(PanelNet *net);

/* One bounded, non-blocking poll per 40 ms firmware tick. */
void panelNetPoll(PanelNet *net, uint32_t nowMs);
void panelNetClose(PanelNet *net);
bool panelNetTlsUp(const PanelNet *net);
bool panelNetWrite(PanelNet *net, const uint8_t *bytes, size_t len);

/* AON wall-clock seed for D7. */
bool panelNetWallTime(const PanelNet *net, uint64_t *epochOut);

/* Real mbedTLS 2.x validation seam used by provisioning commits. */
const PanelProvCrypto *panelNetProvCrypto(const PanelNet *net);

#endif /* SOLARI_PANEL_NET_H */
