/*
 * clientDiscovery.c - client-side discovery & topology hooks (Handoff sec 4, 10).
 *
 * The client cheaply observes its surroundings and feeds them to the server's
 * discovery/topology engines (Phase 5):
 *   - DISCOVERY_ADVERT: ARP neighbors it can see (source = ARP), so the server
 *     can surface un-enrolled hosts as candidate monitor targets.
 *   - TOPOLOGY_REPORT: its own uplink (default route) and interface->segment
 *     (CIDR) bindings, the cheap always-available basis for the network view.
 *
 * Both are best-effort and NOT spooled: they are periodic and self-refreshing,
 * so a missed advert is re-sent next cycle rather than queued. Compiled only
 * when reporting is available (CLIENT_WITH_REPORTING).
 */
#include "client.h"
#include "platOS.h"

#include "solari/solariFrame.h"
#include "solari/solariReporter.h"
#include "solari/solariTlv.h"
#include "solari/solariLog.h"

#include <stdio.h>
#include <string.h>

#define DISC_PAYLOAD_CAP 16384
#define DISC_FRAME_CAP   (DISC_PAYLOAD_CAP + 128)
#define DISC_MAX_NEIGH   64

solariStatus clientBuildDiscoveryFrame(clientContext *ctx, uint8_t *out, size_t cap,
                                       size_t *outLen, uint8_t *nOut)
{
    platNeighbor nb[DISC_MAX_NEIGH];
    uint8_t cnt = 0, i;
    solariTlvWriter w;
    uint8_t payload[DISC_PAYLOAD_CAP];
    solariStatus rc;

    if (!ctx || !out || !outLen) return ERR_INVALID_ARG;
    rc = platArpNeighbors(nb, (uint8_t)(sizeof nb / sizeof *nb), &cnt);
    if (rc != SOLARI_OK) return rc;

    solariTlvWriterInit(&w, payload, sizeof payload);
    solariTlvAppendU8(&w, TLV_DISC_SOURCE, 2);           /* 2 = ARP */
    for (i = 0; i < cnt; i++) {
        char ent[128];
        snprintf(ent, sizeof ent, "%s|%s|%s", nb[i].ip, nb[i].mac, nb[i].iface);
        if (solariTlvAppendStr(&w, TLV_DISC_ENTITY, ent) != SOLARI_OK) break;
    }
    if (nOut) *nOut = cnt;
    return solariReporterFrame(ctx->rep, SCP_MSG_DISCOVERY_ADVERT, SCP_FLAG_NONE,
                               payload, w.len, w.count, out, cap, outLen);
}

solariStatus clientBuildTopologyFrame(clientContext *ctx, uint8_t *out, size_t cap,
                                      size_t *outLen, uint8_t *nOut)
{
    platIfaceCidr ifs[SOLARI_MAX_IFACES];
    platUplink up;
    uint8_t cnt = 0, i;
    solariTlvWriter w;
    uint8_t payload[DISC_PAYLOAD_CAP];

    if (!ctx || !out || !outLen) return ERR_INVALID_ARG;

    solariTlvWriterInit(&w, payload, sizeof payload);
    if (platDefaultUplink(&up) == SOLARI_OK) {
        char u[128];
        snprintf(u, sizeof u, "%s|%s|%llu",
                 up.localIf, up.gatewayIp, (unsigned long long)up.speedMbps);
        solariTlvAppendStr(&w, TLV_TOPO_UPLINK, u);
    }
    if (platIfaceCidrs(ifs, (uint8_t)(sizeof ifs / sizeof *ifs), &cnt) == SOLARI_OK) {
        for (i = 0; i < cnt; i++) {
            char seg[96];
            snprintf(seg, sizeof seg, "%s|%s", ifs[i].ifName, ifs[i].cidr);
            if (solariTlvAppendStr(&w, TLV_TOPO_SEGMENT, seg) != SOLARI_OK) break;
        }
    }
    if (nOut) *nOut = cnt;
    return solariReporterFrame(ctx->rep, SCP_MSG_TOPOLOGY_REPORT, SCP_FLAG_NONE,
                               payload, w.len, w.count, out, cap, outLen);
}

solariStatus clientSendDiscovery(clientContext *ctx)
{
    uint8_t frame[DISC_FRAME_CAP];
    size_t flen = 0;
    uint8_t n = 0;
    solariStatus rc;

    if (!ctx) return ERR_INVALID_ARG;
    if (!solariReporterConnected(ctx->rep)) return ERR_CONN_RETRY;  /* best-effort */
    rc = clientBuildDiscoveryFrame(ctx, frame, sizeof frame, &flen, &n);
    if (rc != SOLARI_OK) return rc;
    rc = solariReporterSendEphemeral(ctx->rep, frame, flen);
    if (rc == SOLARI_OK)
        solariLogf(SOLARI_LOG_DEBUG, "discovery advert sent (%u neighbors)", (unsigned)n);
    return rc;
}

solariStatus clientSendTopology(clientContext *ctx)
{
    uint8_t frame[DISC_FRAME_CAP];
    size_t flen = 0;
    uint8_t n = 0;
    solariStatus rc;

    if (!ctx) return ERR_INVALID_ARG;
    if (!solariReporterConnected(ctx->rep)) return ERR_CONN_RETRY;
    rc = clientBuildTopologyFrame(ctx, frame, sizeof frame, &flen, &n);
    if (rc != SOLARI_OK) return rc;
    rc = solariReporterSendEphemeral(ctx->rep, frame, flen);
    if (rc == SOLARI_OK)
        solariLogf(SOLARI_LOG_DEBUG, "topology report sent (%u segments)", (unsigned)n);
    return rc;
}
