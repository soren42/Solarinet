/*
 * clientReport.c - client reporting on the shared transport (sec 7.3).
 *
 * The push-or-spool fault tolerance now lives in solariReporter (libsolari);
 * this file is the thin client-specific layer: derive the node id, build the
 * CLIENT_REPORT / HELLO payloads, and hand frames to the reporter. Compiled only
 * when reporting is available (CLIENT_WITH_REPORTING).
 */
#include "client.h"
#include "platOS.h"

#include "solari/solariCrypto.h"
#include "solari/solariFrame.h"
#include "solari/solariLog.h"

#include <stdio.h>
#include <string.h>

#define CLIENT_PAYLOAD_CAP 32768
#define CLIENT_FRAME_CAP   (CLIENT_PAYLOAD_CAP + 128)

uint64_t clientNodeId(const clientConfig *cfg)
{
    char fqdn[SOLARI_FQDN_MAX];
    char buf[SOLARI_FQDN_MAX + 8];
    int n;
    if (cfg->nodeId) return cfg->nodeId;          /* enrolled id wins */
    if (cfg->hostFqdn[0]) {
        strncpy(fqdn, cfg->hostFqdn, sizeof fqdn - 1);
        fqdn[sizeof fqdn - 1] = '\0';
    } else if (platHostFqdn(fqdn, sizeof fqdn) != SOLARI_OK) {
        strncpy(fqdn, "unknown", sizeof fqdn - 1);
        fqdn[sizeof fqdn - 1] = '\0';
    }
    n = snprintf(buf, sizeof buf, "%s|%d", fqdn, (int)ROLE_CLIENT);
    return solariFnv1a64(buf, (size_t)(n > 0 ? n : 0));
}

solariStatus clientContextInit(clientContext *ctx, const clientConfig *cfg)
{
    solariReporterOpts o;
    if (!ctx || !cfg) return ERR_INVALID_ARG;
    memset(ctx, 0, sizeof *ctx);
    ctx->cfg    = cfg;
    ctx->nodeId = clientNodeId(cfg);

    memset(&o, 0, sizeof o);
    o.primaryUrl  = cfg->primaryUrl;
    o.failoverUrl = cfg->failoverUrl[0] ? cfg->failoverUrl : NULL;
    o.useTls      = cfg->useTls;
    o.caFile      = cfg->caFile[0]   ? cfg->caFile   : NULL;
    o.certFile    = cfg->certFile[0] ? cfg->certFile : NULL;
    o.keyFile     = cfg->keyFile[0]  ? cfg->keyFile  : NULL;
    o.spoolDb     = cfg->spoolDb[0]  ? cfg->spoolDb  : NULL;
    o.nodeId      = ctx->nodeId;
    return solariReporterOpen(&o, &ctx->rep);
}

void clientContextClose(clientContext *ctx)
{
    if (ctx && ctx->rep) { solariReporterClose(ctx->rep); ctx->rep = NULL; }
}

solariStatus clientConnect(clientContext *ctx)
{
    if (!ctx) return ERR_INVALID_ARG;
    return solariReporterConnect(ctx->rep);
}

solariStatus clientSendHello(clientContext *ctx)
{
    solariHello hi;
    uint8_t payload[1024], frame[1152];
    size_t plen = 0, flen = 0;
    uint16_t tc = 0;
    solariStatus rc;

    if (!ctx) return ERR_INVALID_ARG;
    if (!solariReporterConnected(ctx->rep)) return ERR_CONN_RETRY;

    memset(&hi, 0, sizeof hi);
    hi.role = ROLE_CLIENT;
    strncpy(hi.agentVersion, ctx->cfg->agentVersion, sizeof hi.agentVersion - 1);
    if (ctx->cfg->hostFqdn[0])
        strncpy(hi.hostFqdn, ctx->cfg->hostFqdn, sizeof hi.hostFqdn - 1);
    else
        platHostFqdn(hi.hostFqdn, sizeof hi.hostFqdn);
    hi.configEpoch = ctx->cfg->configEpoch;

    rc = solariMsgBuildHello(&hi, payload, sizeof payload, &plen, &tc);
    if (rc != SOLARI_OK) return rc;
    rc = solariReporterFrame(ctx->rep, SCP_MSG_HELLO, SCP_FLAG_ACK_REQ,
                             payload, plen, tc, frame, sizeof frame, &flen);
    if (rc != SOLARI_OK) return rc;
    return solariReporterSendEphemeral(ctx->rep, frame, flen);   /* WELCOME: Phase 5 */
}

solariStatus clientReportSend(clientContext *ctx, const solariClientReport *rep)
{
    uint8_t payload[CLIENT_PAYLOAD_CAP], frame[CLIENT_FRAME_CAP];
    size_t plen = 0, flen = 0;
    uint16_t tc = 0;
    solariStatus rc;

    if (!ctx || !rep) return ERR_INVALID_ARG;
    rc = solariMsgBuildClientReport(rep, payload, sizeof payload, &plen, &tc);
    if (rc != SOLARI_OK) {
        solariLogf(SOLARI_LOG_ERROR, "report build failed: %s", solariStrError(rc));
        return rc;
    }
    rc = solariReporterFrame(ctx->rep, SCP_MSG_CLIENT_REPORT, SCP_FLAG_NONE,
                             payload, plen, tc, frame, sizeof frame, &flen);
    if (rc != SOLARI_OK) return rc;
    return solariReporterSend(ctx->rep, frame, flen);   /* push or durably spool */
}

solariStatus clientDrainSpool(clientContext *ctx)
{
    if (!ctx) return ERR_INVALID_ARG;
    return solariReporterDrain(ctx->rep);
}
