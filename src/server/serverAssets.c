/*
 * serverAssets.c - monitored-system (asset) + pool orchestration (§10 ext).
 *
 * Authoritative write path for adopting discovered hosts into first-class assets
 * and keeping their probe-target catalog in sync. Reached only via the solariCtl
 * bridge (ASSET_SET / ADOPT verbs); the dashboard never writes these tables.
 */
#include "serverAssets.h"

#include "solari/solariLog.h"
#include "solari/solariTime.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ASSET_REMOVE_TARGET_CAP 512

/* Map a well-known service port to an app-layer probe type. These checks verify
 * the server actually speaks the protocol WITHOUT authenticating (LDAP accepts
 * any BindResponse incl. an anonymous-bind rejection; MySQL/AMQP just validate
 * the server greeting/handshake), so they never false-down on auth-required
 * services. Everything else stays a plain TCP-connect. TLS-wrapped ports (636
 * ldaps, 5671 amqps) are intentionally excluded — the checks are plaintext. */
static const char *probeTypeForPort(int port)
{
    switch (port) {
        case 389:  return "ldap";
        case 3306: return "mysql";
        case 5672: return "amqp";
        default:   return "tcp";
    }
}

/* Upsert a service target for an asset. TCP by transport; the probeType column
 * upgrades well-known ports to an app-layer health check (see probeTypeForPort).
 * The "tcp:" tid prefix is a stable namespace for service targets (opaque key),
 * not the check type — kept stable so probeType upgrades update in place. */
static void assetAddService(serverContext *ctx, const char *ip, int port,
                            const char *name, const char *segId, uint64_t assetId)
{
    char tid[160], label[160];
    const char *probeType;
    if (port <= 0 || port > 65535) return;
    probeType = probeTypeForPort(port);
    (void)snprintf(tid, sizeof tid, "tcp:%s:%d", ip, port);
    (void)snprintf(label, sizeof label, "%s (%s)", (name && name[0]) ? name : "tcp", ip);
    (void)serverDbUpsertProbeTarget(ctx->db, tid, ip, port, "tcp", 2, label,
                                    segId, assetId, probeType, NULL);
}

/* Add ports from an explicit CSV ("22,53,80") as TCP targets. */
static void assetAddCsvServices(serverContext *ctx, const char *ip,
                                const char *csv, const char *segId, uint64_t assetId)
{
    const char *p = csv;
    while (p && *p) {
        int port = 0;
        while (*p && (*p < '0' || *p > '9')) p++;
        while (*p >= '0' && *p <= '9') { port = port * 10 + (*p - '0'); p++; }
        if (port > 0 && port <= 65535) assetAddService(ctx, ip, port, "tcp", segId, assetId);
    }
}

/* Add every "name:port" token from a discovered services JSON array as a TCP
 * target (e.g. ["ssh:22","https:443"]). */
static void assetAddDiscoveredServices(serverContext *ctx, const char *ip,
                                       const char *json, const char *segId,
                                       uint64_t assetId)
{
    const char *p = json;
    while (p && *p) {
        if (*p == '"') {
            const char *s = ++p;
            char tok[80];
            size_t len;
            char *colon;
            while (*p && *p != '"') p++;
            len = (size_t)(p - s);
            if (len >= sizeof tok) len = sizeof tok - 1;
            memcpy(tok, s, len);
            tok[len] = '\0';
            colon = strchr(tok, ':');
            if (colon) {
                *colon = '\0';
                assetAddService(ctx, ip, atoi(colon + 1), tok, segId, assetId);
            }
            if (*p == '"') p++;
        } else {
            p++;
        }
    }
}

/* Add or remove the ICMP host-up target per the heartbeat flag. */
static void assetSyncHeartbeat(serverContext *ctx, const char *ip, bool on,
                               const char *segId, uint64_t assetId)
{
    char tid[80];
    (void)snprintf(tid, sizeof tid, "icmp:%s", ip);
    if (on) {
        char label[96];
        (void)snprintf(label, sizeof label, "ping (%s)", ip);
        (void)serverDbUpsertProbeTarget(ctx->db, tid, ip, 0, "icmp", 2, label,
                                        segId, assetId, "icmp", NULL);
    } else {
        (void)serverDbDeleteProbeTarget(ctx->db, tid);
    }
}

solariStatus serverAssetsAdopt(serverContext *ctx, const serverAdoptOpts *o,
                               uint64_t *assetId)
{
    serverDiscEntity e;
    uint64_t     aid = 0;
    const char  *name, *cls;
    solariStatus st;

    if (assetId) *assetId = 0;
    if (!ctx || !ctx->db || !o || o->discId == 0) return ERR_INVALID_ARG;

    memset(&e, 0, sizeof e);
    st = serverDbGetDiscovered(ctx->db, o->discId, &e);
    if (st != SOLARI_OK) {
        solariLogf(SOLARI_LOG_ERROR, "assets/adopt: discovered %llu not found: %s",
                   (unsigned long long)o->discId, solariStrError(st));
        return st;
    }
    if (!e.ip[0]) return ERR_INVALID_ARG;

    name = o->displayName[0] ? o->displayName : (e.host[0] ? e.host : e.ip);
    cls  = o->className[0]   ? o->className   : "host";

    st = serverDbUpsertAsset(ctx->db, e.ip, e.host[0] ? e.host : NULL, name, cls,
                             o->poolId, o->tagsJson[0] ? o->tagsJson : NULL,
                             o->notes[0] ? o->notes : NULL, o->heartbeat, &aid);
    if (st != SOLARI_OK) {
        solariLogf(SOLARI_LOG_ERROR, "assets/adopt: upsert asset %s failed: %s",
                   e.ip, solariStrError(st));
        return st;
    }

    if (o->servicesCsv[0])
        assetAddCsvServices(ctx, e.ip, o->servicesCsv, e.segId, aid);
    else
        assetAddDiscoveredServices(ctx, e.ip, e.servicesJson, e.segId, aid);

    assetSyncHeartbeat(ctx, e.ip, o->heartbeat, e.segId, aid);

    (void)serverDbSetDiscoveredStatus(ctx->db, o->discId, "adopted");

    if (assetId) *assetId = aid;
    solariLogf(SOLARI_LOG_INFO,
               "assets/adopt: disc=%llu ip=%s -> asset=%llu pool=%llu hb=%d (%s)",
               (unsigned long long)o->discId, e.ip, (unsigned long long)aid,
               (unsigned long long)o->poolId, (int)o->heartbeat, name);
    return SOLARI_OK;
}

solariStatus serverAssetsSetMeta(serverContext *ctx, const char *ip,
                                 const char *displayName, const char *className,
                                 uint64_t poolId, const char *tagsJson,
                                 const char *notes, bool monitorHost)
{
    uint64_t     aid = 0;
    solariStatus st;

    if (!ctx || !ctx->db || !ip || !ip[0]) return ERR_INVALID_ARG;

    st = serverDbUpsertAsset(ctx->db, ip, NULL, displayName,
                             (className && className[0]) ? className : "host",
                             poolId, (tagsJson && tagsJson[0]) ? tagsJson : NULL,
                             (notes && notes[0]) ? notes : NULL, monitorHost, &aid);
    if (st != SOLARI_OK) return st;

    assetSyncHeartbeat(ctx, ip, monitorHost, NULL, aid);
    solariLogf(SOLARI_LOG_INFO, "assets/setmeta: ip=%s asset=%llu pool=%llu hb=%d",
               ip, (unsigned long long)aid, (unsigned long long)poolId, (int)monitorHost);
    return SOLARI_OK;
}

static solariStatus assetParseId(const char *s, uint64_t *out)
{
    char *endp = NULL;
    unsigned long long v;
    if (out) *out = 0;
    if (!s || !s[0] || !out) return ERR_INVALID_ARG;
    errno = 0;
    v = strtoull(s, &endp, 10);
    if (errno != 0 || !endp || *endp != '\0' || v == 0) return ERR_INVALID_ARG;
    *out = (uint64_t)v;
    return SOLARI_OK;
}

solariStatus serverAssetsRemove(serverContext *ctx, const char *assetKey,
                                bool byIp, const char *operator_,
                                size_t *removedTargets)
{
    char targets[ASSET_REMOVE_TARGET_CAP][SERVER_TARGETID_MAX];
    uint64_t assetId = 0;
    size_t count = 0, i;
    char detail[256];
    solariStatus st;

    if (removedTargets) *removedTargets = 0;
    if (!ctx || !ctx->db || !assetKey || !assetKey[0] ||
        !operator_ || !operator_[0]) {
        return ERR_INVALID_ARG;
    }

    st = byIp ? serverDbGetAssetIdByIp(ctx->db, assetKey, &assetId)
              : assetParseId(assetKey, &assetId);
    if (st != SOLARI_OK) return st;
    if (assetId == 0) return ERR_INVALID_ARG;

    st = serverDbListAssetTargets(ctx->db, assetId, targets,
                                  ASSET_REMOVE_TARGET_CAP, &count);
    if (st != SOLARI_OK) return st;

    snprintf(detail, sizeof detail, "asset removed by %s asset=%llu targets=%zu",
             operator_, (unsigned long long)assetId, count);
    st = serverDbWriteAlertEvent(ctx->db, 0, 0, NULL, "warn",
                                 detail, solariNowUnixMs());
    if (st != SOLARI_OK) {
        solariLogf(SOLARI_LOG_ERROR,
                   "assets/remove: audit row write failed asset=%llu: %s",
                   (unsigned long long)assetId, solariStrError(st));
        return st;
    }

    for (i = 0; i < count; i++) {
        st = serverDbPurgeProbeState(ctx->db, targets[i]);
        if (st != SOLARI_OK) return st;
        st = serverDbDeleteProbeTarget(ctx->db, targets[i]);
        if (st != SOLARI_OK) return st;
    }

    st = serverDbDeleteAsset(ctx->db, assetId);
    if (st != SOLARI_OK) return st;
    if (removedTargets) *removedTargets = count;

    solariLogf(SOLARI_LOG_WARN, "assets/remove: asset=%llu targets=%zu by=%s",
               (unsigned long long)assetId, count, operator_);
    return SOLARI_OK;
}
