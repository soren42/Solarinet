/*
 * monitorGossip.c - peerAlive gossip transport (sec 8.2).
 *
 * Each monitor listens for PEER_ALIVE frames on its own gossip URL (PULL) and
 * announces itself to every configured peer (PUSH). A received PEER_ALIVE
 * refreshes the sender in the live-membership registry; the registry's fleet is
 * what HRW uses to assign targets, so a monitor joining or leaving reshapes
 * ownership within a TTL with no coordinator.
 *
 * Peers are dialed LAZILY (and re-dialed) in each tick, so start-up ordering
 * doesn't matter and a peer that comes up later is picked up automatically.
 * Compiled only when the transport is available (MONITOR_WITH_REPORTING).
 */
#include "monitor.h"

#include "solari/solariNet.h"
#include "solari/solariFrame.h"
#include "solari/solariLog.h"

#include <stdlib.h>
#include <string.h>

#define GOSSIP_DRAIN_MAX 256

struct monitorGossip {
    uint64_t    selfNodeId;
    solariConn *listener;
    int         peerCount;
    char        peerUrls[MONITOR_MAX_FLEET][256];
    solariConn *peers[MONITOR_MAX_FLEET];        /* NULL until dialed */
    bool        useTls, hasCa, hasCert, hasKey;
    char        caFile[256], certFile[256], keyFile[256];
    uint32_t    seq;
};

static void applyTls(const monitorGossip *g, solariConnOpts *o)
{
    o->useTls   = g->useTls;
    o->caFile   = g->hasCa   ? g->caFile   : NULL;
    o->certFile = g->hasCert ? g->certFile : NULL;
    o->keyFile  = g->hasKey  ? g->keyFile  : NULL;
}

static void dialPeer(monitorGossip *g, int i)
{
    solariConnOpts o;
    memset(&o, 0, sizeof o);
    o.url = g->peerUrls[i];
    o.pattern = SOLARI_PATTERN_PUSH;
    o.sendTimeoutMs = 500;
    applyTls(g, &o);
    solariConnOpen(&o, &g->peers[i]);            /* leaves NULL on failure */
}

solariStatus monitorGossipOpen(const monitorConfig *cfg, uint64_t selfNodeId,
                               monitorGossip **out)
{
    monitorGossip *g;
    uint8_t i;

    if (!cfg || !out) return ERR_INVALID_ARG;
    *out = NULL;
    if (!cfg->gossipUrl[0]) return SOLARI_OK;        /* gossip disabled */

    g = calloc(1, sizeof *g);
    if (!g) return ERR_PLATFORM;
    g->selfNodeId = selfNodeId;
    g->useTls = cfg->useTls;
    if (cfg->caFile[0])   { strncpy(g->caFile,   cfg->caFile,   sizeof g->caFile   - 1); g->hasCa   = true; }
    if (cfg->certFile[0]) { strncpy(g->certFile, cfg->certFile, sizeof g->certFile - 1); g->hasCert = true; }
    if (cfg->keyFile[0])  { strncpy(g->keyFile,  cfg->keyFile,  sizeof g->keyFile  - 1); g->hasKey  = true; }

    for (i = 0; i < cfg->peerCount && g->peerCount < MONITOR_MAX_FLEET; i++) {
        strncpy(g->peerUrls[g->peerCount], cfg->peerUrls[i], sizeof g->peerUrls[0] - 1);
        g->peerCount++;
    }

    {
        solariConnOpts o;
        memset(&o, 0, sizeof o);
        o.url = cfg->gossipUrl;
        o.pattern = SOLARI_PATTERN_PULL;
        o.recvTimeoutMs = 50;
        applyTls(g, &o);
        if (solariConnListen(&o, &g->listener) != SOLARI_OK) {
            solariLogf(SOLARI_LOG_WARN, "gossip listen failed (%s)", cfg->gossipUrl);
            g->listener = NULL;
        }
    }
    solariLogf(SOLARI_LOG_INFO, "gossip up: listen %s, %d configured peer(s)",
               cfg->gossipUrl, g->peerCount);
    *out = g;
    return SOLARI_OK;
}

void monitorGossipClose(monitorGossip *g)
{
    int i;
    if (!g) return;
    if (g->listener) solariConnClose(g->listener);
    for (i = 0; i < g->peerCount; i++) if (g->peers[i]) solariConnClose(g->peers[i]);
    free(g);
}

solariStatus monitorGossipTick(monitorGossip *g, monitorPeers *reg, uint64_t nowMs)
{
    int i, drained = 0;
    uint8_t frame[64];
    size_t flen = 0;
    solariFrameHeader h;

    if (!g) return SOLARI_OK;                          /* gossip disabled - no-op */
    if (!reg) return ERR_INVALID_ARG;

    for (i = 0; i < g->peerCount; i++)                 /* lazily (re)dial peers */
        if (!g->peers[i]) dialPeer(g, i);

    if (g->listener) {                                 /* drain inbound PEER_ALIVE */
        const uint8_t *rx = NULL;
        size_t rlen = 0;
        while (drained < GOSSIP_DRAIN_MAX &&
               solariConnRecv(g->listener, &rx, &rlen) == SOLARI_OK) {
            solariFrameHeader rh;
            if (solariFrameParse(rx, rlen, &rh, NULL, NULL, NULL) == SOLARI_OK &&
                rh.msgType == SCP_MSG_PEER_ALIVE)
                monitorPeersHeard(reg, rh.sourceNodeId, nowMs);
            drained++;
        }
    }

    memset(&h, 0, sizeof h);                           /* announce self to peers */
    h.magic[0] = SCP_MAGIC_0;
    h.magic[1] = SCP_MAGIC_1;
    h.protoVersion   = SCP_PROTO_VERSION;
    h.msgType        = SCP_MSG_PEER_ALIVE;
    h.sourceNodeId   = g->selfNodeId;
    h.sendTimeUnixMs = nowMs;
    h.seqNo          = ++g->seq;
    if (solariFrameBuild(&h, NULL, 0, frame, sizeof frame, &flen) == SOLARI_OK)
        for (i = 0; i < g->peerCount; i++)
            if (g->peers[i]) solariConnSend(g->peers[i], frame, flen);
    return SOLARI_OK;
}
