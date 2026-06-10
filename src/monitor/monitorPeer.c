/*
 * monitorPeer.c - rendezvous (HRW) target ownership (sec 8.2).
 *
 * Each (target, monitor) pair gets a weight = FNV-1a-64(targetId || nodeId).
 * A target is owned by the k monitors with the highest weight. monitorOwnsTarget
 * answers "is self among that top-k" without any coordinator: it counts how many
 * fleet members outrank self for the target; self owns iff fewer than k do.
 * Ties (astronomically rare at 64 bits) break by nodeId.
 *
 * Churn is minimal by construction: removing a monitor only reassigns the
 * targets it owned; every other target's owner set is untouched.
 */
#include "monitor.h"
#include "solari/solariCrypto.h"

#include <stdio.h>
#include <string.h>

static uint64_t hrwWeight(const char *targetId, uint64_t nodeId)
{
    uint8_t buf[MONITOR_TARGETID_MAX + 8];
    size_t n = strlen(targetId);
    int i;
    if (n > MONITOR_TARGETID_MAX) n = MONITOR_TARGETID_MAX;
    memcpy(buf, targetId, n);
    for (i = 0; i < 8; i++) buf[n + i] = (uint8_t)(nodeId >> (i * 8));   /* mix in node */
    return solariFnv1a64(buf, n + 8);
}

bool monitorOwnsTarget(uint64_t self, const uint64_t *fleet, size_t fleetLen,
                       const char *targetId, uint8_t k)
{
    uint64_t ws;
    size_t higher = 0, i;
    bool selfInFleet = false;

    if (!fleet || fleetLen == 0 || !targetId || k == 0) return false;
    ws = hrwWeight(targetId, self);

    for (i = 0; i < fleetLen; i++) {
        uint64_t w;
        if (fleet[i] == self) { selfInFleet = true; continue; }
        w = hrwWeight(targetId, fleet[i]);
        if (w > ws || (w == ws && fleet[i] > self)) higher++;
    }
    if (!selfInFleet) return false;          /* self isn't a fleet member */
    return higher < k;
}

uint64_t monitorNodeId(const monitorConfig *cfg)
{
    char fqdn[SOLARI_FQDN_MAX];
    char buf[SOLARI_FQDN_MAX + 8];
    int n;
    if (cfg->nodeId) return cfg->nodeId;
    if (cfg->hostFqdn[0]) {
        strncpy(fqdn, cfg->hostFqdn, sizeof fqdn - 1);
        fqdn[sizeof fqdn - 1] = '\0';
    } else if (monitorHostFqdn(fqdn, sizeof fqdn) != SOLARI_OK) {
        strncpy(fqdn, "unknown", sizeof fqdn - 1);
        fqdn[sizeof fqdn - 1] = '\0';
    }
    n = snprintf(buf, sizeof buf, "%s|%d", fqdn, (int)ROLE_MONITOR);
    return solariFnv1a64(buf, (size_t)(n > 0 ? n : 0));
}

/* ---- peer registry ---- */

void monitorPeersInit(monitorPeers *p, uint64_t selfNodeId)
{
    if (!p) return;
    p->selfNodeId = selfNodeId;
    p->count = 0;
}

void monitorPeersHeard(monitorPeers *p, uint64_t nodeId, uint64_t nowMs)
{
    size_t i;
    if (!p || nodeId == 0 || nodeId == p->selfNodeId) return;   /* never track self */
    for (i = 0; i < p->count; i++) {
        if (p->peers[i].nodeId == nodeId) { p->peers[i].lastHeardMs = nowMs; return; }
    }
    if (p->count < MONITOR_MAX_FLEET) {
        p->peers[p->count].nodeId = nodeId;
        p->peers[p->count].lastHeardMs = nowMs;
        p->count++;
    }
}

void monitorPeersPrune(monitorPeers *p, uint64_t nowMs, uint32_t ttlSec)
{
    size_t i = 0;
    uint64_t ttlMs = (uint64_t)ttlSec * 1000u;
    if (!p) return;
    while (i < p->count) {
        if (nowMs >= p->peers[i].lastHeardMs &&
            nowMs - p->peers[i].lastHeardMs > ttlMs) {
            p->peers[i] = p->peers[p->count - 1];   /* swap-remove */
            p->count--;
        } else {
            i++;
        }
    }
}

size_t monitorPeersFleet(const monitorPeers *p, uint64_t nowMs, uint32_t ttlSec,
                         uint64_t *fleetOut, size_t cap)
{
    size_t n = 0, i;
    uint64_t ttlMs = (uint64_t)ttlSec * 1000u;
    if (!p || !fleetOut || cap == 0) return 0;
    fleetOut[n++] = p->selfNodeId;
    for (i = 0; i < p->count && n < cap; i++) {
        if (nowMs < p->peers[i].lastHeardMs ||
            nowMs - p->peers[i].lastHeardMs <= ttlMs)
            fleetOut[n++] = p->peers[i].nodeId;
    }
    return n;
}

size_t monitorOwnedTargets(const monitorConfig *cfg, uint64_t self,
                           const uint64_t *fleet, size_t fleetLen,
                           uint8_t *ownedIdx, size_t cap)
{
    size_t n = 0;
    uint8_t i;
    if (!cfg || !fleet || !ownedIdx) return 0;
    for (i = 0; i < cfg->targetCount && n < cap; i++) {
        if (monitorOwnsTarget(self, fleet, fleetLen, cfg->targets[i].targetId, cfg->replFactor))
            ownedIdx[n++] = i;
    }
    return n;
}
