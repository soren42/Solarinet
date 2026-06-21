/*
 * serverTopology.c - TOPOLOGY_REPORT consumer (Handoff §7.2).
 *
 * Consumes SCP_MSG_TOPOLOGY_REPORT (0x14) from clients and monitors and turns
 * each node's self-reported link-layer view into the network-hierarchy
 * projection that backs /api/topology?view=network:
 *
 *   - TLV_TOPO_UPLINK (0x1301, "localIf|gatewayIp|speedMbps") is the node's own
 *     default uplink. We synthesize a networkGear row for the gateway it points
 *     at (keyed by a gearId derived from the gateway IP) and record an lldpEdge
 *     from this node (nodeId) to that gear over localIf, viaLldp=false (it came
 *     from routing/ARP, not the LLDP-MIB).
 *   - TLV_TOPO_LLDP_NEIGH (0x1302, repeated "chassis|port|localIf") is a true
 *     LLDP neighbour the OS exposed. Each opens a networkGear row for the
 *     neighbour chassis (a switch/AP) and an lldpEdge from this node to it over
 *     localIf, peerPort=port, viaLldp=true.
 *   - TLV_TOPO_SEGMENT (0x1303, repeated "ifName|cidr") binds an interface to a
 *     CIDR segment. We carry the most-recent segment's id forward so gear/edges
 *     reported in the same frame inherit a best-effort segId. (Full per-iface
 *     segment resolution against the segment table is a later enhancement; here
 *     we use the CIDR-derived id as the node-self-reported segment hint.)
 *
 * Deep managed-gear interrogation (full LLDP-MIB / SNMP walks that populate
 * model/ports/wireless from the device itself) is the most environment-specific
 * piece and is explicitly out of scope for this pass (Handoff §7.2 "start with
 * what nodes self-report"); the synthesized gear rows carry kind + mgmtIp and a
 * derived name, leaving model/ports/wireless for the later walk to enrich via
 * the same serverDbUpsertNetGear upsert.
 *
 * Parsing, field-splitting, gearId derivation, and the edge/gear builders are
 * factored into static pure helpers so they are unit-testable without a live
 * MariaDB; only the upsert calls touch serverDb.
 */
#include "server.h"

#include "solari/solariTlv.h"
#include "solari/solariLog.h"
#include "solari/solariTime.h"

#include <string.h>
#include <stdio.h>
#include <ctype.h>

/* A single report should describe one node's local adjacency; cap the per-frame
 * work so a hostile or runaway producer cannot make us loop unboundedly. */
#define TOPO_MAX_NEIGH_PER_REPORT 256
#define TOPO_MAX_SEG_PER_REPORT   64

/* ===================================================================== */
/* Pure helpers (no DB) - kept static and side-effect free for testing.   */
/* ===================================================================== */

/* Copy at most field [n] (0-based) of a '|'-delimited record into out (always
 * NUL-terminated). A missing field yields "". Returns the field length written
 * (excluding the NUL). Mirrors the inverse of the node's snprintf("%s|%s|%s").
 */
static size_t topoSplitField(const char *rec, size_t recLen, unsigned n,
                             char *out, size_t cap)
{
    size_t i = 0, field = 0, start, end;

    if (!out || cap == 0) return 0;
    out[0] = '\0';
    if (!rec) return 0;

    /* Walk to the start of field n. */
    while (field < n && i < recLen) {
        if (rec[i] == '|') field++;
        i++;
    }
    if (field < n) return 0;           /* fewer fields than requested */
    start = i;
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

/* Parse a decimal integer field into *out (clamped to a sane port-speed range).
 * Returns true if at least one digit was consumed; non-digit/garbage yields 0
 * and false so the caller can leave the column NULL-ish (0). */
static bool topoParseInt(const char *s, int *out)
{
    long v = 0;
    bool any = false;
    const char *p = s;

    if (out) *out = 0;
    if (!s) return false;
    while (*p == ' ') p++;
    while (*p >= '0' && *p <= '9') {
        v = v * 10 + (*p - '0');
        if (v > 1000000000L) v = 1000000000L;   /* clamp (1 Tbps in Mbps) */
        any = true;
        p++;
    }
    if (any && out) *out = (int)v;
    return any;
}

/* Derive a stable, schema-legal gearId (VARCHAR(48), [a-z0-9-]) from a seed
 * string + a prefix. Lowercases, maps any non [a-z0-9] run to a single '-', and
 * trims leading/trailing '-'. Produces e.g. ("gw-", "192.168.1.1") ->
 * "gw-192-168-1-1" and ("sw-", "aa:bb:cc:dd:ee:ff") -> "sw-aa-bb-cc-dd-ee-ff".
 * An empty/degenerate seed yields just the trimmed prefix. NUL-terminates out.
 */
static void topoDeriveGearId(const char *prefix, const char *seed,
                             char *out, size_t cap)
{
    size_t o = 0;
    bool lastDash = false;
    const char *p;

    if (!out || cap == 0) return;
    out[0] = '\0';
    if (cap < 2) return;

    /* Copy the (already legal) prefix verbatim, bounded. */
    for (p = prefix; p && *p && o < cap - 1; p++)
        out[o++] = *p;

    for (p = seed; p && *p && o < cap - 1; p++) {
        unsigned char c = (unsigned char)*p;
        if (isalnum(c)) {
            out[o++] = (char)tolower(c);
            lastDash = false;
        } else if (!lastDash && o > 0) {
            out[o++] = '-';
            lastDash = true;
        }
    }
    /* Trim a trailing '-'. */
    while (o > 0 && out[o - 1] == '-') o--;
    out[o] = '\0';
}

/* Derive a lightweight, deterministic segId hint from a CIDR ("10.0.0.0/24" ->
 * "seg-10-0-0-0-24"). Used only as the node-self-reported segment binding until
 * the segment table is authoritative. NUL-terminates out. */
static void topoDeriveSegId(const char *cidr, char *out, size_t cap)
{
    if (!out || cap == 0) return;
    out[0] = '\0';
    if (!cidr || cidr[0] == '\0') return;
    topoDeriveGearId("seg-", cidr, out, cap);
}

/* Build a synthesized networkGear row for a gateway/switch the node points at.
 * `kind` is the schema enum ("gateway"|"switch"|"ap"|...). `mgmtIp` may be ""
 * (LLDP chassis ids are often MACs, not IPs). `segId` may be "" if unknown. The
 * row is intentionally sparse: model/ports/wireless stay zero/"" for the later
 * managed-gear walk to enrich via the same upsert key (gearId). Returns false
 * if a usable gearId could not be derived (nothing to persist). */
static bool topoBuildGear(serverNetGear *g, const char *gearId,
                          const char *name, const char *kind,
                          const char *mgmtIp, const char *segId, bool wireless)
{
    if (!g || !gearId || gearId[0] == '\0') return false;
    memset(g, 0, sizeof *g);
    snprintf(g->gearId, sizeof g->gearId, "%s", gearId);
    snprintf(g->name,   sizeof g->name,   "%s",
             (name && name[0]) ? name : gearId);
    snprintf(g->kind,   sizeof g->kind,   "%s", kind ? kind : "other");
    if (mgmtIp) snprintf(g->mgmtIp, sizeof g->mgmtIp, "%s", mgmtIp);
    if (segId)  snprintf(g->segId,  sizeof g->segId,  "%s", segId);
    g->wireless = wireless;
    /* uplinkGearId left "" - the self-referential hierarchy is populated by the
     * managed-gear walk, not derivable from a single node's self-report. */
    return true;
}

/* Build an lldpEdge from this node to a piece of gear. `peerPort` may be "".
 * linkType follows `wireless`. viaLldp records whether this came from the LLDP
 * neighbour table (true) versus routing/ARP-derived uplink (false). */
static void topoBuildEdge(serverLldpEdge *e, uint64_t nodeId, const char *gearId,
                          const char *localIf, const char *peerPort,
                          bool wireless, int speedMbps, bool viaLldp)
{
    memset(e, 0, sizeof *e);
    e->nodeId = nodeId;
    if (gearId)   snprintf(e->gearId,   sizeof e->gearId,   "%s", gearId);
    if (localIf)  snprintf(e->localIf,  sizeof e->localIf,  "%s", localIf);
    if (peerPort) snprintf(e->peerPort, sizeof e->peerPort, "%s", peerPort);
    snprintf(e->linkType, sizeof e->linkType, "%s",
             wireless ? "wireless" : "wired");
    e->speedMbps = speedMbps;
    e->rssi      = 0;                  /* not exposed by self-report TLVs yet */
    e->viaLldp   = viaLldp;
}

/* ===================================================================== */
/* DB-touching core                                                       */
/* ===================================================================== */

/* Persist one gear + the edge to it in the natural order (gear first so the
 * edge's gearId always references an existing inventory row). Either persist
 * may legitimately have an empty gearId edge (unknown gear) - in that case we
 * still record the edge. Propagates the first DB error. */
static solariStatus topoPersist(serverContext *ctx, const serverNetGear *gear,
                                const serverLldpEdge *edge, uint64_t whenUnixMs)
{
    solariStatus rc;

    if (!ctx || !ctx->db) return ERR_DB;

    if (gear && gear->gearId[0] != '\0') {
        rc = serverDbUpsertNetGear(ctx->db, gear, whenUnixMs);
        if (rc != SOLARI_OK) return rc;
    }
    if (edge) {
        rc = serverDbWriteLldpEdge(ctx->db, edge, whenUnixMs);
        if (rc != SOLARI_OK) return rc;
    }
    return SOLARI_OK;
}

/* ===================================================================== */
/* Public API                                                             */
/* ===================================================================== */

solariStatus serverTopologyOnReport(serverContext *ctx, uint64_t nodeId,
                                    const uint8_t *payload, size_t len)
{
    solariTlvReader r;
    uint16_t type = 0, vlen = 0;
    const uint8_t *val = NULL;
    uint64_t now;
    solariStatus rc;
    unsigned uplinks = 0, neighbours = 0, segments = 0, edges = 0;
    char curSeg[SERVER_SEGID_MAX];     /* most-recent segment hint, carried fwd */

    if (!ctx) return ERR_INVALID_ARG;
    if (len > 0 && !payload) return ERR_INVALID_ARG;
    if (!ctx->db) return ERR_DB;

    now = solariNowUnixMs();
    curSeg[0] = '\0';

    solariTlvReaderInit(&r, payload, len);
    for (;;) {
        rc = solariTlvNext(&r, &type, &val, &vlen);
        if (rc == ERR_TLV_END) break;
        if (rc != SOLARI_OK) {
            solariLogf(SOLARI_LOG_WARN,
                       "topology: malformed report from 0x%llx (rc=%d)",
                       (unsigned long long)nodeId, (int)rc);
            return rc;          /* ERR_TLV_TRUNCATED - reject the whole frame */
        }

        switch (type) {
        case TLV_TOPO_UPLINK: {
            /* "localIf|gatewayIp|speedMbps" - the node's default uplink. */
            char localIf[48], gwIp[SERVER_IP_MAX], speedStr[16], gearId[SERVER_GEARID_MAX];
            serverNetGear gear;
            serverLldpEdge edge;
            int speed = 0;

            topoSplitField((const char *)val, vlen, 0, localIf, sizeof localIf);
            topoSplitField((const char *)val, vlen, 1, gwIp,    sizeof gwIp);
            topoSplitField((const char *)val, vlen, 2, speedStr, sizeof speedStr);
            topoParseInt(speedStr, &speed);

            if (gwIp[0] == '\0') {
                solariLogf(SOLARI_LOG_DEBUG,
                           "topology: uplink from 0x%llx with empty gateway ignored",
                           (unsigned long long)nodeId);
                break;
            }
            uplinks++;
            topoDeriveGearId("gw-", gwIp, gearId, sizeof gearId);
            /* The default gateway is modelled as a 'gateway' gear, mgmtIp=gwIp. */
            if (topoBuildGear(&gear, gearId, gwIp, "gateway", gwIp,
                              curSeg, false)) {
                topoBuildEdge(&edge, nodeId, gearId, localIf, "",
                              false, speed, false);
                rc = topoPersist(ctx, &gear, &edge, now);
                if (rc != SOLARI_OK) return rc;
                edges++;
            }
            break;
        }

        case TLV_TOPO_LLDP_NEIGH: {
            /* "chassis|port|localIf" - a true LLDP neighbour (switch/AP). */
            char chassis[64], port[48], localIf[48], gearId[SERVER_GEARID_MAX];
            serverNetGear gear;
            serverLldpEdge edge;

            if (neighbours >= TOPO_MAX_NEIGH_PER_REPORT) {
                solariLogf(SOLARI_LOG_WARN,
                           "topology: report from 0x%llx exceeds %d neighbours; "
                           "truncating", (unsigned long long)nodeId,
                           TOPO_MAX_NEIGH_PER_REPORT);
                break;
            }
            topoSplitField((const char *)val, vlen, 0, chassis, sizeof chassis);
            topoSplitField((const char *)val, vlen, 1, port,    sizeof port);
            topoSplitField((const char *)val, vlen, 2, localIf, sizeof localIf);

            if (chassis[0] == '\0') {
                solariLogf(SOLARI_LOG_DEBUG,
                           "topology: lldp neighbour from 0x%llx with empty "
                           "chassis ignored", (unsigned long long)nodeId);
                break;
            }
            neighbours++;
            /* The chassis id (often a MAC) seeds a 'switch' gear; mgmtIp is
             * unknown from LLDP alone so it is left "" for the later walk. */
            topoDeriveGearId("sw-", chassis, gearId, sizeof gearId);
            if (topoBuildGear(&gear, gearId, chassis, "switch", "",
                              curSeg, false)) {
                topoBuildEdge(&edge, nodeId, gearId, localIf, port,
                              false, 0, true);
                rc = topoPersist(ctx, &gear, &edge, now);
                if (rc != SOLARI_OK) return rc;
                edges++;
            }
            break;
        }

        case TLV_TOPO_SEGMENT: {
            /* "ifName|cidr" - bind an interface to a CIDR segment. We carry the
             * derived segId forward as the hint for subsequent gear/edges. */
            char ifName[48], cidr[64];
            if (segments >= TOPO_MAX_SEG_PER_REPORT) break;
            topoSplitField((const char *)val, vlen, 0, ifName, sizeof ifName);
            topoSplitField((const char *)val, vlen, 1, cidr,   sizeof cidr);
            if (cidr[0] != '\0') {
                topoDeriveSegId(cidr, curSeg, sizeof curSeg);
                segments++;
                solariLogf(SOLARI_LOG_DEBUG,
                           "topology: 0x%llx segment %s on %s -> segId=%s",
                           (unsigned long long)nodeId, cidr, ifName, curSeg);
            }
            break;
        }

        default:
            /* Unknown TLVs are skipped, never fatal (forward-compat, §5.6). */
            break;
        }
    }

    solariLogf(SOLARI_LOG_INFO,
               "topology: report from 0x%llx uplinks=%u neighbours=%u "
               "segments=%u edges=%u", (unsigned long long)nodeId,
               uplinks, neighbours, segments, edges);
    return SOLARI_OK;
}
