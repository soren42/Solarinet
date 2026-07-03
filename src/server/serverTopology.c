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

/* Deep managed-gear walk: bound a single device's LLDP-MIB neighbour table so a
 * misbehaving switch cannot make the normalizer loop unboundedly. */
#define TOPO_MAX_MIB_NEIGH        4096
/* Uplink-chain assembly: cap the hop count so a cyclic uplinkGearId graph (bad
 * data) cannot livelock the walker. */
#define TOPO_MAX_UPLINK_HOPS      64

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

/* Forward declarations: the gear/edge builders + persist live with the shallow
 * path further down, but the deep-discovery normalizer (next) reuses them. */
static bool topoBuildGear(serverNetGear *g, const char *gearId,
                          const char *name, const char *kind,
                          const char *mgmtIp, const char *segId, bool wireless);
static void topoBuildEdge(serverLldpEdge *e, uint64_t nodeId, const char *gearId,
                          const char *localIf, const char *peerPort,
                          bool wireless, int speedMbps, bool viaLldp);
static solariStatus topoPersist(serverContext *ctx, const serverNetGear *gear,
                                const serverLldpEdge *edge, uint64_t whenUnixMs);

/* ===================================================================== */
/* Deep managed-gear (LLDP-MIB) normalization - pure, SNMP-free logic.    */
/*                                                                        */
/* The SNMP walk that actually FETCHES these rows from a managed device is */
/* isolated behind topoSnmpFetchLldp() below (OFF by default). Everything  */
/* here operates on already-fetched, in-memory LLDP-MIB records so it is   */
/* fully unit-testable with no net-snmp dependency.                        */
/* ===================================================================== */

/* LLDP chassis/port id subtypes we care about (LLDP-MIB lldpChassisIdSubtype /
 * lldpPortIdSubtype). Values match the standard MIB enumerations; only the ones
 * we normalize specially are named, the rest fall through to a verbatim copy. */
typedef enum {
    TOPO_ID_SUB_CHASSIS_MAC   = 4,  /* macAddress           */
    TOPO_ID_SUB_NETWORK_ADDR  = 5,  /* networkAddress (IP)  */
    TOPO_ID_SUB_IFNAME        = 5,  /* portId: interfaceName (alias of 5)   */
    TOPO_ID_SUB_LOCAL         = 7   /* local (free-form)    */
} topoIdSubtype;

/* One normalized LLDP-MIB remote-systems-table row, as produced by an SNMP walk
 * of lldpRemTable on a managed device. All strings already decoded to UTF-8 /
 * printable form by the fetch seam; this struct is the clean boundary the pure
 * normalizer consumes. */
typedef struct {
    char     chassisId[64];     /* lldpRemChassisId (MAC, addr, or name)  */
    int      chassisSubtype;    /* lldpRemChassisIdSubtype                 */
    char     portId[64];        /* lldpRemPortId                          */
    int      portSubtype;       /* lldpRemPortIdSubtype                    */
    char     sysName[96];       /* lldpRemSysName                         */
    char     sysDesc[160];      /* lldpRemSysDesc                         */
    char     mgmtIp[SERVER_IP_MAX]; /* lldpRemManAddr (IPv4/IPv6), "" ok  */
    char     localIf[48];       /* the local port the neighbour was seen on */
    int      capabilities;      /* lldpRemSysCapEnabled bitmap (see below) */
    bool     wireless;          /* link is wireless (WLAN cap / radio if)  */
    int      speedMbps;         /* link speed if known, else 0            */
} topoLldpMibRecord;

/* LLDP system-capability bits (LLDP-MIB LldpSystemCapabilitiesMap). We only key
 * gear-kind inference off a small subset. */
enum {
    TOPO_CAP_REPEATER = 0x0002,
    TOPO_CAP_BRIDGE   = 0x0004,  /* switch        */
    TOPO_CAP_WLAN_AP  = 0x0008,  /* access point  */
    TOPO_CAP_ROUTER   = 0x0010,
    TOPO_CAP_PHONE    = 0x0020,
    TOPO_CAP_STATION  = 0x0080   /* end host      */
};

/* Case-insensitive substring search (haystack contains needle). Pure, no libc
 * strcasestr (which is a GNU extension and not C99). */
static bool topoContainsCi(const char *hay, const char *needle)
{
    size_t nl;
    if (!hay || !needle) return false;
    nl = strlen(needle);
    if (nl == 0) return true;
    for (; *hay; hay++) {
        size_t i = 0;
        while (i < nl &&
               tolower((unsigned char)hay[i]) == tolower((unsigned char)needle[i]))
            i++;
        if (i == nl) return true;
    }
    return false;
}

/* Infer the schema gear-kind ("gateway"|"switch"|"ap"|"router"|"other") from an
 * LLDP-MIB record. Capability bits are authoritative when present (they come
 * straight from the device); sysName/sysDesc keywords are the fallback when a
 * device reports no/empty capabilities (common on cheap managed gear). The order
 * mirrors operator intent: an AP that is also a bridge is still an "ap"; a router
 * that also bridges is a "router"; a pure bridge is a "switch". Returns a pointer
 * to a static string literal. */
static const char *topoInferGearKind(const topoLldpMibRecord *rec)
{
    int caps;
    if (!rec) return "other";
    caps = rec->capabilities;

    if (caps != 0) {
        if (caps & TOPO_CAP_WLAN_AP)  return "ap";
        if (caps & TOPO_CAP_ROUTER)   return "router";
        if (caps & TOPO_CAP_BRIDGE)   return "switch";
        if (caps & TOPO_CAP_REPEATER) return "switch";
        /* Router/phone/station-only with no bridging -> not infra gear. */
        return "other";
    }

    /* No capability hint: lean on the human-readable identity strings. */
    if (topoContainsCi(rec->sysName, "ap")    ||
        topoContainsCi(rec->sysDesc, "access point") ||
        topoContainsCi(rec->sysDesc, "wireless")     ||
        topoContainsCi(rec->sysDesc, "wifi")         ||
        topoContainsCi(rec->sysDesc, "wi-fi")) return "ap";
    if (topoContainsCi(rec->sysName, "gw")    ||
        topoContainsCi(rec->sysName, "gateway") ||
        topoContainsCi(rec->sysDesc, "gateway")) return "gateway";
    if (topoContainsCi(rec->sysName, "rtr")   ||
        topoContainsCi(rec->sysName, "router") ||
        topoContainsCi(rec->sysDesc, "router")) return "router";
    if (topoContainsCi(rec->sysName, "sw")    ||
        topoContainsCi(rec->sysName, "switch") ||
        topoContainsCi(rec->sysDesc, "switch") ||
        topoContainsCi(rec->sysDesc, "ethernet switch")) return "switch";
    return "other";
}

/* Choose the stable gearId seed + prefix for an LLDP-MIB record. Preference:
 *   1. sysName (human-stable, survives MAC churn on stacked/virtual chassis)
 *   2. mgmtIp  (stable management address)
 *   3. chassisId (MAC / local id - last resort)
 * The prefix encodes inferred kind so ids read naturally (sw-/ap-/gw-/rtr-/nd-).
 * Writes the derived id into out. Returns the seed actually used (for callers
 * that want to record provenance); never NULL. */
static const char *topoMibGearId(const topoLldpMibRecord *rec, const char *kind,
                                 char *out, size_t cap)
{
    const char *prefix = "nd-";
    const char *seed;

    if (kind) {
        if      (strcmp(kind, "switch")  == 0) prefix = "sw-";
        else if (strcmp(kind, "ap")      == 0) prefix = "ap-";
        else if (strcmp(kind, "gateway") == 0) prefix = "gw-";
        else if (strcmp(kind, "router")  == 0) prefix = "rtr-";
    }

    if (rec && rec->sysName[0] != '\0')       seed = rec->sysName;
    else if (rec && rec->mgmtIp[0] != '\0')   seed = rec->mgmtIp;
    else if (rec && rec->chassisId[0] != '\0')seed = rec->chassisId;
    else                                       seed = "";

    topoDeriveGearId(prefix, seed, out, cap);
    return seed;
}

/* Normalize ONE LLDP-MIB remote-systems row into a networkGear inventory row +
 * the lldpEdge connecting `nodeId` (the SolariNet node hosting the SNMP walk, or
 * 0 if the walk ran against an out-of-band collector) to that gear.
 *
 * This is the heart of the deep path: it turns raw MIB columns into the same
 * schema rows the shallow self-report path produces, so both feed one inventory.
 * model is filled from sysDesc (truncated to schema width); the edge carries
 * peerPort = the neighbour's portId, viaLldp=true (it came from the LLDP-MIB).
 *
 * Returns false (and leaves the out-params untouched) if the record has no
 * usable identity (empty chassisId AND sysName AND mgmtIp) - nothing to persist.
 */
static bool topoNormalizeMibRecord(const topoLldpMibRecord *rec, uint64_t nodeId,
                                   const char *segId,
                                   serverNetGear *g, serverLldpEdge *e)
{
    const char *kind;
    char gearId[SERVER_GEARID_MAX];
    const char *name;

    if (!rec || !g || !e) return false;
    if (rec->chassisId[0] == '\0' && rec->sysName[0] == '\0' &&
        rec->mgmtIp[0] == '\0')
        return false;

    kind = topoInferGearKind(rec);
    topoMibGearId(rec, kind, gearId, sizeof gearId);
    if (gearId[0] == '\0') return false;

    /* Prefer the human-readable sysName for the display name; fall back to the
     * mgmtIp, then the chassis id. topoBuildGear defaults name->gearId if all
     * are empty. */
    name = (rec->sysName[0] != '\0') ? rec->sysName
         : (rec->mgmtIp[0]  != '\0') ? rec->mgmtIp
         : rec->chassisId;

    if (!topoBuildGear(g, gearId, name, kind, rec->mgmtIp, segId, rec->wireless))
        return false;

    /* Enrich beyond the shallow path: the deep walk knows the model (sysDesc).
     * sysDesc can exceed model's width; copy at most model-1 bytes (explicit
     * precision keeps the truncation intentional and warning-free). */
    if (rec->sysDesc[0] != '\0')
        snprintf(g->model, sizeof g->model, "%.*s",
                 (int)(sizeof g->model - 1), rec->sysDesc);

    topoBuildEdge(e, nodeId, gearId, rec->localIf, rec->portId,
                  rec->wireless, rec->speedMbps, true);
    return true;
}

/* ===================================================================== */
/* /api/netgear assembly - attached-node counts + uplink chain.           */
/*                                                                        */
/* Pure aggregation over an in-memory snapshot of the networkGear +       */
/* lldpEdge tables, so the projection that backs GET /api/netgear (and    */
/* the view=network hierarchy) is unit-testable without MariaDB. The web  */
/* tier reads the same columns straight from the DB; this logic documents */
/* and validates the exact rollup it must perform.                        */
/* ===================================================================== */

/* One row of the /api/netgear projection: a gear plus its derived rollups. */
typedef struct {
    char     gearId[SERVER_GEARID_MAX];
    char     name[96];
    char     kind[12];
    char     uplinkGearId[SERVER_GEARID_MAX]; /* immediate parent, "" if root */
    unsigned attachedNodes;   /* distinct SolariNet nodes edged to this gear  */
    unsigned uplinkDepth;     /* hops to a root (0 = root / no uplink)         */
} topoNetGearView;

/* Find the index of gear `gearId` in the rows array, or -1. */
static int topoFindGear(const topoNetGearView *rows, size_t n, const char *gearId)
{
    size_t i;
    if (!gearId || gearId[0] == '\0') return -1;
    for (i = 0; i < n; i++)
        if (strcmp(rows[i].gearId, gearId) == 0) return (int)i;
    return -1;
}

/* Compute, for one gear row, the number of hops up its uplinkGearId chain to a
 * root (a gear with empty uplink or one that is not present in the set). Caps at
 * TOPO_MAX_UPLINK_HOPS so a cyclic graph (corrupt data) returns the cap instead
 * of looping forever. Pure - reads only the rows array. */
static unsigned topoUplinkDepth(const topoNetGearView *rows, size_t n, size_t start)
{
    unsigned depth = 0;
    int cur = (int)start;

    while (depth < TOPO_MAX_UPLINK_HOPS) {
        const char *up;
        int next;
        if (cur < 0 || (size_t)cur >= n) break;
        up = rows[cur].uplinkGearId;
        if (up[0] == '\0') break;            /* reached a root */
        next = topoFindGear(rows, n, up);
        if (next < 0) break;                 /* parent not in set -> root-ish */
        if (next == (int)start) { depth = TOPO_MAX_UPLINK_HOPS; break; } /* cycle */
        depth++;
        cur = next;
    }
    return depth;
}

/* Assemble the /api/netgear projection from a gear inventory snapshot + the edge
 * list. For each gear row we (a) copy identity + uplinkGearId, (b) count the
 * DISTINCT nodeIds attached via lldpEdge rows that reference it (an edge with
 * nodeId==0 is gear-to-gear and is NOT counted as an attached node), and
 * (c) derive the uplink-chain depth. Gear is de-duplicated by gearId: a repeated
 * gearId in `gear` collapses onto the first occurrence (attached counts still
 * accumulate from all matching edges). Writes up to *count rows into out and
 * updates *count to the number written. Returns ERR_BUFFER_FULL if out is too
 * small for the de-duplicated gear set, else SOLARI_OK. */
static solariStatus topoAssembleNetGear(const serverNetGear *gear, size_t gearN,
                                        const serverLldpEdge *edges, size_t edgeN,
                                        topoNetGearView *out, size_t *count)
{
    size_t i, j, rows = 0;
    size_t cap = (count ? *count : 0);

    if (!out || !count) return ERR_INVALID_ARG;
    if (gearN && !gear) return ERR_INVALID_ARG;
    if (edgeN && !edges) return ERR_INVALID_ARG;

    /* Pass 1: de-duplicate gear by gearId into the projection rows. */
    for (i = 0; i < gearN; i++) {
        if (gear[i].gearId[0] == '\0') continue;
        if (topoFindGear(out, rows, gear[i].gearId) >= 0) continue; /* dupe */
        if (rows >= cap) { *count = rows; return ERR_BUFFER_FULL; }
        memset(&out[rows], 0, sizeof out[rows]);
        snprintf(out[rows].gearId, sizeof out[rows].gearId, "%s", gear[i].gearId);
        snprintf(out[rows].name,   sizeof out[rows].name,   "%s", gear[i].name);
        snprintf(out[rows].kind,   sizeof out[rows].kind,   "%s", gear[i].kind);
        snprintf(out[rows].uplinkGearId, sizeof out[rows].uplinkGearId, "%s",
                 gear[i].uplinkGearId);
        rows++;
    }

    /* Pass 2: attached-node counts. We count DISTINCT (gear, nodeId) pairs, so a
     * node reporting the same gear over several interfaces counts once. With the
     * small per-gear edge fan-in expected here a linear distinct check is fine. */
    for (i = 0; i < rows; i++) {
        for (j = 0; j < edgeN; j++) {
            size_t k;
            bool seen = false;
            if (edges[j].nodeId == 0) continue;            /* gear-to-gear edge */
            if (strcmp(edges[j].gearId, out[i].gearId) != 0) continue;
            for (k = 0; k < j; k++) {
                if (edges[k].nodeId == edges[j].nodeId &&
                    strcmp(edges[k].gearId, out[i].gearId) == 0) {
                    seen = true;
                    break;
                }
            }
            if (!seen) out[i].attachedNodes++;
        }
    }

    /* Pass 3: uplink-chain depth (needs the full row set to resolve parents). */
    for (i = 0; i < rows; i++)
        out[i].uplinkDepth = topoUplinkDepth(out, rows, i);

    *count = rows;
    return SOLARI_OK;
}

/* Public projection accessor backing GET /api/netgear: given snapshots of the
 * networkGear + lldpEdge tables (read by the control/API tier), fill `out` with
 * the de-duplicated gear set decorated with attached-node counts and uplink-chain
 * depth. Thin, deliberate wrapper around the pure assembler so the rollup lives
 * in one tested place rather than being re-derived in SQL/PHP. Not declared in
 * server.h (this header is frozen for subsystem implementers); the API tier links
 * it by prototype. *count is in/out: capacity in, rows written out. */
solariStatus serverTopologyNetGearView(const serverNetGear *gear, size_t gearN,
                                       const serverLldpEdge *edges, size_t edgeN,
                                       topoNetGearView *out, size_t *count)
{
    return topoAssembleNetGear(gear, gearN, edges, edgeN, out, count);
}

/* ===================================================================== */
/* SNMP fetch seam - the ONLY place a real net-snmp dependency would live. */
/*                                                                        */
/* OFF by default: with SOLARI_WITH_SNMP undefined the build adds ZERO    */
/* dependencies and this returns ERR_PLATFORM ("no SNMP backend"). The    */
/* normalization logic above is fully exercised by the unit tests without */
/* ever calling this. A real implementation (guarded by the macro) would  */
/* walk lldpRemTable / lldpLocPortTable / sysName / sysDescr and fill the  */
/* records[] array, then the caller runs topoNormalizeMibRecord() on each. */
/* ===================================================================== */
#ifdef SOLARI_WITH_SNMP
/* LLDP-MIB neighbor walk. The IF-MIB interface poller (serverSnmp.c) is the
 * implemented SNMP feature; a full lldpRemTable decode is a documented follow-up,
 * so this returns "walked, no neighbors" rather than fabricating rows — the
 * caller safely handles an empty set. */
static solariStatus topoSnmpFetchLldpImpl(const char *mgmtIp, const char *community,
                                          topoLldpMibRecord *records, size_t *count)
{
    (void)mgmtIp; (void)community; (void)records;
    if (count) *count = 0;
    return SOLARI_OK;
}
#endif

static solariStatus topoSnmpFetchLldp(const char *mgmtIp, const char *community,
                                      topoLldpMibRecord *records, size_t *count)
{
#ifdef SOLARI_WITH_SNMP
    /* Real net-snmp walk goes here (compiled in only when explicitly enabled).
     * It must bound the result at TOPO_MAX_MIB_NEIGH and decode each column into
     * the topoLldpMibRecord fields exactly as the normalizer expects. */
    return topoSnmpFetchLldpImpl(mgmtIp, community, records, count);
#else
    (void)mgmtIp;
    (void)community;
    (void)records;
    if (count) *count = 0;
    /* No SNMP backend compiled in - the deep path is a no-op enhancement here. */
    return ERR_PLATFORM;
#endif
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

/* Deep managed-gear interrogation entry point (Handoff §7.2 enhancement). Runs
 * the SNMP/LLDP-MIB walk against `mgmtIp` (through the OFF-by-default seam),
 * normalizes every returned neighbour into networkGear + lldpEdge rows, and
 * upserts them. `nodeId` is the SolariNet node that performed the walk (the L2
 * peer of the gear), or 0 for an out-of-band collector. `segId` may be "".
 *
 * With no SNMP backend compiled in, the fetch seam returns ERR_PLATFORM and this
 * is a clean no-op (returns ERR_PLATFORM, persists nothing) - the build carries
 * zero extra dependencies and the shallow self-report path remains the source of
 * the host->switch hierarchy. The normalization + assembly logic this would
 * drive is fully unit-tested independently of the seam. */
solariStatus serverTopologyDeepWalk(serverContext *ctx, uint64_t nodeId,
                                    const char *mgmtIp, const char *community,
                                    const char *segId)
{
    static topoLldpMibRecord records[TOPO_MAX_MIB_NEIGH];
    size_t count = TOPO_MAX_MIB_NEIGH, i, persisted = 0;
    solariStatus rc;
    uint64_t now;

    if (!ctx) return ERR_INVALID_ARG;
    if (!mgmtIp || mgmtIp[0] == '\0') return ERR_INVALID_ARG;
    if (!ctx->db) return ERR_DB;

    rc = topoSnmpFetchLldp(mgmtIp, community, records, &count);
    if (rc != SOLARI_OK) {
        solariLogf(SOLARI_LOG_DEBUG,
                   "topology: deep walk of %s skipped (rc=%d, %s)",
                   mgmtIp, (int)rc, solariStrError(rc));
        return rc;          /* ERR_PLATFORM when no SNMP backend is built in */
    }
    if (count > TOPO_MAX_MIB_NEIGH) count = TOPO_MAX_MIB_NEIGH;

    now = solariNowUnixMs();
    for (i = 0; i < count; i++) {
        serverNetGear gear;
        serverLldpEdge edge;
        if (!topoNormalizeMibRecord(&records[i], nodeId,
                                    segId ? segId : "", &gear, &edge))
            continue;
        rc = topoPersist(ctx, &gear, &edge, now);
        if (rc != SOLARI_OK) return rc;
        persisted++;
    }

    solariLogf(SOLARI_LOG_INFO,
               "topology: deep walk of %s normalized %u/%zu LLDP-MIB neighbours",
               mgmtIp, (unsigned)persisted, count);
    return SOLARI_OK;
}
