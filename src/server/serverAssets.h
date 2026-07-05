/*
 * serverAssets.h - monitored-system (asset) + pool orchestration (§10 ext).
 *
 * A discovered host is adopted into a first-class "asset" that owns many probe
 * targets (one per selected service) plus an optional ICMP heartbeat, carries
 * operator metadata (name/class/tags/notes), and belongs to a functional pool.
 * All of this is authoritative WRITE-path logic and therefore lives in the C
 * server (the dashboard reaches it only through the solariCtl bridge; PHP never
 * writes). Pool create/update and global-config/rule writes are simple enough to
 * call serverDb directly from the bridge; the multi-step adopt + metadata sync
 * is orchestrated here.
 */
#ifndef SOLARI_SERVER_ASSETS_H
#define SOLARI_SERVER_ASSETS_H

#include "server.h"

/* Options for adopting a discovered entity into a monitored asset. Empty string
 * fields fall back to a sensible default (name<-host/ip, class<-"host", all
 * discovered services when servicesCsv is empty). */
typedef struct {
    uint64_t discId;            /* discovered row to adopt (required)        */
    char     displayName[128];
    char     className[32];
    uint64_t poolId;            /* 0 = unassigned                            */
    char     tagsJson[256];     /* "" = none                                 */
    char     notes[256];
    bool     heartbeat;         /* create the ICMP host-up target            */
    char     servicesCsv[256];  /* "22,53,80" override; "" = all discovered  */
} serverAdoptOpts;

/* Adopt a discovered entity: upsert the asset, (re)provision a TCP target per
 * selected service + the heartbeat, mark the discovered row 'adopted'. *assetId
 * returns the asset row id. */
solariStatus serverAssetsAdopt(serverContext *ctx, const serverAdoptOpts *o,
                               uint64_t *assetId);

/* Update an existing asset's metadata (keyed by ip) and sync its heartbeat
 * target to monitorHost. Empty-string fields are written as-is (the caller
 * sends the full desired metadata). */
solariStatus serverAssetsSetMeta(serverContext *ctx, const char *ip,
                                 const char *displayName, const char *className,
                                 uint64_t poolId, const char *tagsJson,
                                 const char *notes, bool monitorHost);

/* Remove an asset by asset id or ip. Purges every owned probe target's current
 * and history state, deletes the target catalog rows, deletes the asset row, and
 * writes an audit alertEvent. *removedTargets returns the target purge count. */
solariStatus serverAssetsRemove(serverContext *ctx, const char *assetKey,
                                bool byIp, const char *operator_,
                                size_t *removedTargets);

#endif /* SOLARI_SERVER_ASSETS_H */
