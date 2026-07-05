/*
 * serverDiscovery.c - DISCOVERY_ADVERT consumer (Handoff §7.1).
 *
 * Consumes SCP_MSG_DISCOVERY_ADVERT (0x13) from clients and monitors and turns
 * each observed neighbour into a 'discovered' candidate the operator can adopt:
 *
 *   - Parses the discovery TLV stream (TLV_DISC_SOURCE/ENTITY/SERVICE, 0x12xx).
 *     A single TLV_DISC_SOURCE (uint8 via-code) prefaces the advert and labels
 *     every entity in it; each TLV_DISC_ENTITY ("ip|mac|iface") opens a new
 *     candidate, and any TLV_DISC_SERVICE ("proto|port|banner") that follows is
 *     attached to the most-recently-opened entity as a services[] element.
 *   - De-dupes against the node table via serverDbDiscoveredIsKnown so already
 *     enrolled/monitored entities never surface as "new".
 *   - Upserts each survivor into 'discovered' keyed (ip, kind) through
 *     serverDbUpsertDiscovered, which inserts new rows or bumps seenCount.
 *   - Honours cfg->autoDiscover (gather at all) and cfg->autoEnroll (auto-open
 *     an enrollment vs. hold for the operator); both default conservative
 *     (discover yes, auto-enroll no).
 *
 * serverDiscoveryAdopt / serverDiscoveryIgnore are the operator actions the
 * /api/discovery POSTs (§6) route through solariCtl: adopt promotes a candidate
 * (via serverProvision), ignore suppresses it from the "new" list.
 *
 * Parsing, field-splitting and the via/kind state machine are factored into
 * static pure helpers so they are unit-testable without a live MariaDB; only the
 * upsert / dedup / status mutations touch serverDb.
 */
#include "server.h"

#include "solari/solariTlv.h"
#include "solari/solariLog.h"
#include "solari/solariTime.h"

#include <string.h>
#include <stdio.h>

/* An advert may legitimately carry many neighbours; cap the per-frame work so a
 * hostile or runaway producer cannot make us loop unboundedly. Survivors beyond
 * this are dropped and re-offered on the next periodic advert. */
#define DISC_MAX_ENTITIES_PER_ADVERT 256

/* ===================================================================== */
/* Pure helpers (no DB) - kept static and side-effect free for testing.   */
/* ===================================================================== */

/* Map a TLV_DISC_SOURCE wire code (1=mDNS 2=ARP 3=advert 4=portscan 5=LLDP) to
 * the 'discovered.via' enum text. Unknown codes fall back to "scp_advert" - the
 * advert reached us over SCP regardless, so that is the honest provenance. */
static const char *discViaFromSource(uint8_t source)
{
    switch (source) {
    case 1:  return "mdns";
    case 2:  return "arp";
    case 3:  return "scp_advert";
    case 4:  return "portscan";
    case 5:  return "lldp";
    default: return "scp_advert";
    }
}

/* Choose the 'discovered.kind' enum for an entity. ARP/mDNS/LLDP neighbours are
 * whole hosts; a port-scan finding describes a service; everything else is
 * "other". (A later refinement may flip an entity to "service" once a
 * TLV_DISC_SERVICE attaches, but the source already disambiguates the common
 * cases.) */
static const char *discKindFromSource(uint8_t source)
{
    switch (source) {
    case 1:  return "host";      /* mDNS     */
    case 2:  return "host";      /* ARP      */
    case 4:  return "service";   /* portscan */
    case 5:  return "host";      /* LLDP     */
    default: return "host";      /* advert: typically a peer host */
    }
}

/* Copy at most field [n] (0-based) of a '|'-delimited record into out (always
 * NUL-terminated). A missing field yields "". Returns the field length written
 * (excluding the NUL). This is the inverse of the client's snprintf("%s|%s|%s").
 */
static size_t discSplitField(const char *rec, size_t recLen, unsigned n,
                             char *out, size_t cap)
{
    size_t i = 0, field = 0, start = 0, end;

    if (!out || cap == 0) return 0;
    out[0] = '\0';
    if (!rec) return 0;

    /* Walk to the start of field n. */
    while (field < n && i < recLen) {
        if (rec[i] == '|') field++;
        i++;
    }
    if (field < n) return 0;          /* fewer fields than requested */
    start = i;
    /* Find the end of this field. */
    end = start;
    while (end < recLen && rec[end] != '|') end++;

    {
        size_t flen = end - start;
        if (flen > cap - 1) flen = cap - 1;
        if (flen > 0) memcpy(out, rec + start, flen);
        out[flen] = '\0';
        return flen;
    }
}

/* Append one "proto:port" service token (derived from a TLV_DISC_SERVICE record
 * "proto|port|banner") to a JSON array string being accumulated in [json, cap).
 * The array is maintained without enclosing brackets in *json (we frame it on
 * finalize); tokens are comma-separated and individually quoted. Returns true if
 * the token fit, false if it was dropped for space (the array stays valid). */
static bool discServiceAppend(char *json, size_t cap, size_t *jsonLen,
                              const char *proto, const char *port)
{
    char token[160];
    int n;
    size_t need;

    if (!json || !jsonLen) return false;
    /* "proto:port" - both already field-split & bounded by the caller. */
    n = snprintf(token, sizeof token, "%s\"%s:%s\"",
                 (*jsonLen > 0) ? "," : "", proto, port);
    if (n < 0) return false;
    need = (size_t)n;
    if (*jsonLen + need + 1 > cap) return false;   /* +1 for the NUL */
    memcpy(json + *jsonLen, token, need + 1);
    *jsonLen += need;
    return true;
}

/* Wrap the accumulated comma-separated body in '[' ... ']' into out. An empty
 * body becomes "" (the schema's services column is nullable / "" sentinel). */
static void discServicesFinalize(const char *body, char *out, size_t cap)
{
    if (!out || cap == 0) return;
    if (!body || body[0] == '\0') { out[0] = '\0'; return; }
    snprintf(out, cap, "[%s]", body);
}

/* ===================================================================== */
/* DB-touching core                                                       */
/* ===================================================================== */

/* Persist one fully-built candidate: dedup against the node table, then upsert.
 * Returns SOLARI_OK on a stored/bumped row, ERR_CONN_RETRY (benign) when the
 * entity is already a known node and was skipped, or a DB error to propagate. */
static solariStatus discPersistEntity(serverContext *ctx,
                                      const serverDiscEntity *e,
                                      uint64_t whenUnixMs, uint64_t *discIdOut)
{
    bool known = false;
    uint64_t discId = 0;
    solariStatus rc;

    if (discIdOut) *discIdOut = 0;
    if (!ctx || !ctx->db || !e) return ERR_INVALID_ARG;
    if (e->ip[0] == '\0') return ERR_INVALID_ARG;   /* ip is the unique key */

    /* De-dupe against enrolled nodes so monitored entities never appear new. */
    rc = serverDbDiscoveredIsKnown(ctx->db, e->ip, e->kind, &known);
    if (rc != SOLARI_OK) return rc;
    if (known) return ERR_CONN_RETRY;               /* skip, not an error */

    rc = serverDbUpsertDiscovered(ctx->db, e, whenUnixMs, &discId);
    if (rc != SOLARI_OK) return rc;
    if (discIdOut) *discIdOut = discId;

    /* autoEnroll: optionally open an enrollment immediately instead of holding
     * the candidate for an operator. Conservative default is off; we only act
     * for host-kind candidates (services/networks are adopted as probe targets,
     * not enrolled as agent-bearing nodes). */
    if (ctx->cfg && ctx->cfg->autoEnroll && strcmp(e->kind, "host") == 0) {
        solariStatus arc = serverDiscoveryAdopt(ctx, discId, "host");
        if (arc != SOLARI_OK)
            solariLogf(SOLARI_LOG_WARN,
                       "discovery: autoEnroll of discId=%llu (%s) failed rc=%d",
                       (unsigned long long)discId, e->ip, (int)arc);
    }
    return SOLARI_OK;
}

/* ===================================================================== */
/* Public API                                                             */
/* ===================================================================== */

solariStatus serverDiscoveryOnAdvert(serverContext *ctx, uint64_t nodeId,
                                     const uint8_t *payload, size_t len)
{
    solariTlvReader r;
    uint16_t type = 0, vlen = 0;
    const uint8_t *val = NULL;
    uint8_t source = 3;                 /* default "advert" until a SOURCE seen */
    const char *via = "scp_advert";
    const char *kind = "host";
    uint64_t now;
    solariStatus rc;
    unsigned stored = 0, skipped = 0, entities = 0;
    bool haveEntity = false;
    serverDiscEntity cur;               /* the entity currently being assembled */
    char svcBody[480];                  /* accumulated services[] body (no [])  */
    size_t svcLen = 0;

    if (!ctx) return ERR_INVALID_ARG;
    if (len > 0 && !payload) return ERR_INVALID_ARG;

    /* autoDiscover off: drop the advert silently (still a successful no-op so
     * the ingest pipeline does not treat it as a transport error). */
    if (ctx->cfg && !ctx->cfg->autoDiscover) {
        solariLogf(SOLARI_LOG_DEBUG,
                   "discovery: advert from 0x%llx dropped (autoDiscover off)",
                   (unsigned long long)nodeId);
        return SOLARI_OK;
    }
    if (!ctx->db) return ERR_DB;

    now = solariNowUnixMs();
    memset(&cur, 0, sizeof cur);
    svcBody[0] = '\0';

    solariTlvReaderInit(&r, payload, len);
    for (;;) {
        rc = solariTlvNext(&r, &type, &val, &vlen);
        if (rc == ERR_TLV_END) break;
        if (rc != SOLARI_OK) {
            solariLogf(SOLARI_LOG_WARN,
                       "discovery: malformed advert from 0x%llx (rc=%d)",
                       (unsigned long long)nodeId, (int)rc);
            return rc;          /* ERR_TLV_TRUNCATED - reject the whole frame */
        }

        switch (type) {
        case TLV_DISC_SOURCE: {
            uint8_t s = 0;
            if (solariTlvReadU8(val, vlen, &s) == SOLARI_OK) {
                source = s;
                via    = discViaFromSource(source);
                kind   = discKindFromSource(source);
            }
            break;
        }

        case TLV_DISC_ENTITY: {
            /* A new entity closes the previous one (flush it). */
            if (haveEntity) {
                discServicesFinalize(svcBody, cur.servicesJson,
                                     sizeof cur.servicesJson);
                rc = discPersistEntity(ctx, &cur, now, NULL);
                if (rc == SOLARI_OK)            stored++;
                else if (rc == ERR_CONN_RETRY)  skipped++;
                else return rc;                 /* propagate real DB failure */
            }
            if (entities >= DISC_MAX_ENTITIES_PER_ADVERT) {
                solariLogf(SOLARI_LOG_WARN,
                           "discovery: advert from 0x%llx exceeds %d entities; "
                           "truncating", (unsigned long long)nodeId,
                           DISC_MAX_ENTITIES_PER_ADVERT);
                haveEntity = false;
                break;
            }
            entities++;

            /* Start a fresh candidate from "ip|mac|iface". */
            memset(&cur, 0, sizeof cur);
            svcBody[0] = '\0';
            svcLen = 0;
            discSplitField((const char *)val, vlen, 0, cur.ip, sizeof cur.ip);
            discSplitField((const char *)val, vlen, 1, cur.mac, sizeof cur.mac);
            /* field 2 (iface) is informational only and not stored. */
            snprintf(cur.kind, sizeof cur.kind, "%s", kind);
            snprintf(cur.via,  sizeof cur.via,  "%s", via);
            haveEntity = (cur.ip[0] != '\0');
            if (!haveEntity)
                solariLogf(SOLARI_LOG_DEBUG,
                           "discovery: entity with empty ip ignored");
            break;
        }

        case TLV_DISC_SERVICE: {
            /* "proto|port|banner" - attach proto:port to the current entity and
             * promote its kind to a service. */
            char proto[32], port[16];
            if (!haveEntity) break;     /* service with no entity context */
            discSplitField((const char *)val, vlen, 0, proto, sizeof proto);
            discSplitField((const char *)val, vlen, 1, port,  sizeof port);
            if (proto[0] != '\0' || port[0] != '\0') {
                discServiceAppend(svcBody, sizeof svcBody, &svcLen, proto, port);
                snprintf(cur.kind, sizeof cur.kind, "%s", "service");
            }
            break;
        }

        default:
            /* Unknown TLVs are skipped, never fatal (forward-compat, §5.6). */
            break;
        }
    }

    /* Flush the final open entity. */
    if (haveEntity) {
        discServicesFinalize(svcBody, cur.servicesJson, sizeof cur.servicesJson);
        rc = discPersistEntity(ctx, &cur, now, NULL);
        if (rc == SOLARI_OK)            stored++;
        else if (rc == ERR_CONN_RETRY)  skipped++;
        else return rc;
    }

    solariLogf(SOLARI_LOG_INFO,
               "discovery: advert from 0x%llx via=%s entities=%u stored=%u "
               "skipped(known)=%u", (unsigned long long)nodeId, via,
               entities, stored, skipped);
    return SOLARI_OK;
}

solariStatus serverDiscoveryAdopt(serverContext *ctx, uint64_t discId,
                                  const char *roleOrSpec)
{
    serverDiscEntity e;
    solariStatus rc;

    if (!ctx || !ctx->db) return ERR_INVALID_ARG;
    if (discId == 0) return ERR_INVALID_ARG;

    /* Resolve the candidate so we can route by kind. */
    memset(&e, 0, sizeof e);
    rc = serverDbGetDiscovered(ctx->db, discId, &e);
    if (rc != SOLARI_OK) {
        solariLogf(SOLARI_LOG_WARN,
                   "discovery: adopt discId=%llu - load failed rc=%d",
                   (unsigned long long)discId, (int)rc);
        return rc;
    }

    /* Mark the row 'adopting' before handing off; serverProvision advances it to
     * 'adopted' (probe target) or the enrollment flow takes over for hosts. A
     * failure to set the transient status is non-fatal to the adoption itself,
     * but we log it. */
    rc = serverDbSetDiscoveredStatus(ctx->db, discId, "adopting");
    if (rc != SOLARI_OK)
        solariLogf(SOLARI_LOG_WARN,
                   "discovery: adopt discId=%llu - status set failed rc=%d",
                   (unsigned long long)discId, (int)rc);

    /* Route by kind (Handoff §7.1/§7.4): services/networks become live probe
     * targets via the monitor adopt path; hosts open an enrollment. The caller's
     * roleOrSpec carries the probe spec (for targets) or the desired role (for
     * hosts); pass it through to serverProvision. */
    if (strcmp(e.kind, "service") == 0 || strcmp(e.kind, "network") == 0) {
        rc = serverProvisionAdoptTarget(ctx, discId, roleOrSpec);
    } else {
        /* Host adoption: open an enrollment so the node can present a CSR. The
         * provisioning subsystem owns the enrollment row + node upsert; we hand
         * it the discovered context implicitly via discId. */
        serverEnrollment enr;
        uint64_t enrId = 0;
        memset(&enr, 0, sizeof enr);
        snprintf(enr.host, sizeof enr.host, "%s", e.host);
        snprintf(enr.ip,   sizeof enr.ip,   "%s", e.ip);
        enr.role   = ROLE_CLIENT;              /* default; operator may refine */
        enr.status = ENROLL_TOKEN;
        rc = serverDbEnrollCreate(ctx->db, &enr, solariNowUnixMs(), &enrId);
        if (rc == SOLARI_OK)
            solariLogf(SOLARI_LOG_INFO,
                       "discovery: adopt discId=%llu opened enrollment enrId=%llu "
                       "(host %s)", (unsigned long long)discId,
                       (unsigned long long)enrId, e.ip);
    }

    if (rc != SOLARI_OK) {
        /* Roll the row back to 'new' so the operator can retry. */
        (void)serverDbSetDiscoveredStatus(ctx->db, discId, "new");
        solariLogf(SOLARI_LOG_ERROR,
                   "discovery: adopt discId=%llu failed rc=%d",
                   (unsigned long long)discId, (int)rc);
    }
    return rc;
}

solariStatus serverDiscoveryIgnore(serverContext *ctx, uint64_t discId)
{
    solariStatus rc;

    if (!ctx || !ctx->db) return ERR_INVALID_ARG;
    if (discId == 0) return ERR_INVALID_ARG;

    rc = serverDbSetDiscoveredStatus(ctx->db, discId, "ignored");
    if (rc == SOLARI_OK)
        solariLogf(SOLARI_LOG_INFO, "discovery: discId=%llu marked ignored",
                   (unsigned long long)discId);
    else
        solariLogf(SOLARI_LOG_WARN,
                   "discovery: ignore discId=%llu failed rc=%d",
                   (unsigned long long)discId, (int)rc);
    return rc;
}
