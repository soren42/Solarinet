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
