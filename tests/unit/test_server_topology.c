/*
 * test_server_topology.c - pure parsing/derivation helpers of serverTopology.c
 * (§7.2): the '|'-delimited field splitter, decimal-int parse with clamping, and
 * the schema-legal gearId/segId derivation ([a-z0-9-], collapsed runs, trimmed).
 */
#include "unity.h"

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "serverTopology.c"

#include <string.h>

void setUp(void) {}
void tearDown(void) {}

static void test_split_field(void)
{
    const char *rec = "chassisA|Gi0/1|eth0";
    size_t rl = strlen(rec);
    char out[64];

    TEST_ASSERT_EQUAL_UINT(8, topoSplitField(rec, rl, 0, out, sizeof out));
    TEST_ASSERT_EQUAL_STRING("chassisA", out);
    topoSplitField(rec, rl, 1, out, sizeof out);
    TEST_ASSERT_EQUAL_STRING("Gi0/1", out);
    topoSplitField(rec, rl, 2, out, sizeof out);
    TEST_ASSERT_EQUAL_STRING("eth0", out);

    /* field beyond the record -> "" */
    topoSplitField(rec, rl, 5, out, sizeof out);
    TEST_ASSERT_EQUAL_STRING("", out);

    /* an empty middle field */
    const char *rec2 = "a||c";
    topoSplitField(rec2, strlen(rec2), 1, out, sizeof out);
    TEST_ASSERT_EQUAL_STRING("", out);
}

static void test_parse_int(void)
{
    int v = -1;
    TEST_ASSERT_TRUE(topoParseInt("1000", &v));
    TEST_ASSERT_EQUAL_INT(1000, v);
    TEST_ASSERT_TRUE(topoParseInt("  42abc", &v)); /* leading space, trailing junk */
    TEST_ASSERT_EQUAL_INT(42, v);
    TEST_ASSERT_FALSE(topoParseInt("notanumber", &v));
    TEST_ASSERT_EQUAL_INT(0, v);                   /* false -> 0 */
    /* clamp guards against absurd values */
    TEST_ASSERT_TRUE(topoParseInt("99999999999", &v));
    TEST_ASSERT_EQUAL_INT(1000000000, v);
}

static void test_derive_gear_id(void)
{
    char out[SERVER_GEARID_MAX];

    topoDeriveGearId("gw-", "192.168.1.1", out, sizeof out);
    TEST_ASSERT_EQUAL_STRING("gw-192-168-1-1", out);

    topoDeriveGearId("sw-", "aa:BB:cc:DD:ee:FF", out, sizeof out);
    TEST_ASSERT_EQUAL_STRING("sw-aa-bb-cc-dd-ee-ff", out);   /* lowercased */

    /* trailing punctuation trimmed, runs collapsed */
    topoDeriveGearId("gw-", "10.0.0.0/", out, sizeof out);
    TEST_ASSERT_EQUAL_STRING("gw-10-0-0-0", out);

    /* empty seed -> just the trimmed prefix (prefix has no trailing dash kept) */
    topoDeriveGearId("gw-", "", out, sizeof out);
    TEST_ASSERT_EQUAL_STRING("gw", out);
}

static void test_derive_seg_id(void)
{
    char out[SERVER_SEGID_MAX];
    topoDeriveSegId("10.0.0.0/24", out, sizeof out);
    TEST_ASSERT_EQUAL_STRING("seg-10-0-0-0-24", out);
    /* empty cidr -> "" */
    topoDeriveSegId("", out, sizeof out);
    TEST_ASSERT_EQUAL_STRING("", out);
}

/* ---- deep managed-gear (LLDP-MIB) normalization (§7.2 enhancement) ---- */

static void test_contains_ci(void)
{
    TEST_ASSERT_TRUE(topoContainsCi("Cisco Catalyst Switch", "switch"));
    TEST_ASSERT_TRUE(topoContainsCi("UniFi AP-AC-Pro", "ap"));
    TEST_ASSERT_TRUE(topoContainsCi("anything", ""));   /* empty needle */
    TEST_ASSERT_FALSE(topoContainsCi("router", "switch"));
    TEST_ASSERT_FALSE(topoContainsCi(NULL, "x"));
}

static void test_infer_gear_kind_caps(void)
{
    topoLldpMibRecord r;

    /* Capability bits are authoritative when present. */
    memset(&r, 0, sizeof r);
    r.capabilities = TOPO_CAP_BRIDGE;
    TEST_ASSERT_EQUAL_STRING("switch", topoInferGearKind(&r));

    r.capabilities = TOPO_CAP_WLAN_AP | TOPO_CAP_BRIDGE; /* AP wins over bridge */
    TEST_ASSERT_EQUAL_STRING("ap", topoInferGearKind(&r));

    r.capabilities = TOPO_CAP_ROUTER | TOPO_CAP_BRIDGE;  /* router over bridge  */
    TEST_ASSERT_EQUAL_STRING("router", topoInferGearKind(&r));

    r.capabilities = TOPO_CAP_STATION;                   /* end host -> other   */
    TEST_ASSERT_EQUAL_STRING("other", topoInferGearKind(&r));
}

static void test_infer_gear_kind_strings(void)
{
    topoLldpMibRecord r;

    /* No capability bits -> fall back to sysName/sysDesc keywords. */
    memset(&r, 0, sizeof r);
    snprintf(r.sysName, sizeof r.sysName, "%s", "sw-core-01");
    TEST_ASSERT_EQUAL_STRING("switch", topoInferGearKind(&r));

    memset(&r, 0, sizeof r);
    snprintf(r.sysDesc, sizeof r.sysDesc, "%s", "UniFi Access Point");
    TEST_ASSERT_EQUAL_STRING("ap", topoInferGearKind(&r));

    memset(&r, 0, sizeof r);
    snprintf(r.sysName, sizeof r.sysName, "%s", "edge-gateway");
    TEST_ASSERT_EQUAL_STRING("gateway", topoInferGearKind(&r));

    memset(&r, 0, sizeof r);
    snprintf(r.sysDesc, sizeof r.sysDesc, "%s", "Generic Ethernet Switch v2");
    TEST_ASSERT_EQUAL_STRING("switch", topoInferGearKind(&r));

    memset(&r, 0, sizeof r);   /* nothing identifying -> other */
    TEST_ASSERT_EQUAL_STRING("other", topoInferGearKind(&r));
}

static void test_normalize_mib_record(void)
{
    topoLldpMibRecord r;
    serverNetGear g;
    serverLldpEdge e;

    memset(&r, 0, sizeof r);
    snprintf(r.chassisId, sizeof r.chassisId, "%s", "aa:bb:cc:dd:ee:ff");
    r.chassisSubtype = TOPO_ID_SUB_CHASSIS_MAC;
    snprintf(r.portId,  sizeof r.portId,  "%s", "Gi1/0/24");
    snprintf(r.sysName, sizeof r.sysName, "%s", "sw-core-01");
    snprintf(r.sysDesc, sizeof r.sysDesc, "%s", "Cisco IOS Switch");
    snprintf(r.mgmtIp,  sizeof r.mgmtIp,  "%s", "10.0.0.2");
    snprintf(r.localIf, sizeof r.localIf, "%s", "eth0");
    r.capabilities = TOPO_CAP_BRIDGE;
    r.speedMbps = 1000;

    TEST_ASSERT_TRUE(topoNormalizeMibRecord(&r, 0xABCDULL, "seg-10-0-0-0-24",
                                            &g, &e));
    /* gearId derives from sysName (preferred over MAC) with the kind prefix. */
    TEST_ASSERT_EQUAL_STRING("sw-sw-core-01", g.gearId);
    TEST_ASSERT_EQUAL_STRING("sw-core-01", g.name);
    TEST_ASSERT_EQUAL_STRING("switch", g.kind);
    TEST_ASSERT_EQUAL_STRING("10.0.0.2", g.mgmtIp);
    TEST_ASSERT_EQUAL_STRING("Cisco IOS Switch", g.model);  /* deep-only enrich */
    TEST_ASSERT_EQUAL_STRING("seg-10-0-0-0-24", g.segId);

    /* edge: node -> gear over localIf, peerPort = neighbour portId, viaLldp. */
    TEST_ASSERT_EQUAL_UINT64(0xABCDULL, e.nodeId);
    TEST_ASSERT_EQUAL_STRING("sw-sw-core-01", e.gearId);
    TEST_ASSERT_EQUAL_STRING("eth0", e.localIf);
    TEST_ASSERT_EQUAL_STRING("Gi1/0/24", e.peerPort);
    TEST_ASSERT_EQUAL_STRING("wired", e.linkType);
    TEST_ASSERT_EQUAL_INT(1000, e.speedMbps);
    TEST_ASSERT_TRUE(e.viaLldp);

    /* A record with no identity at all is rejected (nothing to persist). */
    memset(&r, 0, sizeof r);
    TEST_ASSERT_FALSE(topoNormalizeMibRecord(&r, 1, "", &g, &e));
}

static void test_normalize_mib_id_fallback(void)
{
    topoLldpMibRecord r;
    serverNetGear g;
    serverLldpEdge e;

    /* No sysName -> gearId seeds from mgmtIp. */
    memset(&r, 0, sizeof r);
    snprintf(r.mgmtIp,    sizeof r.mgmtIp,    "%s", "10.0.0.9");
    snprintf(r.chassisId, sizeof r.chassisId, "%s", "11:22:33:44:55:66");
    r.capabilities = TOPO_CAP_WLAN_AP;
    r.wireless = true;
    TEST_ASSERT_TRUE(topoNormalizeMibRecord(&r, 7, "", &g, &e));
    TEST_ASSERT_EQUAL_STRING("ap-10-0-0-9", g.gearId);
    TEST_ASSERT_TRUE(g.wireless);
    TEST_ASSERT_EQUAL_STRING("wireless", e.linkType);

    /* No sysName, no mgmtIp -> seeds from chassis id. */
    memset(&r, 0, sizeof r);
    snprintf(r.chassisId, sizeof r.chassisId, "%s", "11:22:33:44:55:66");
    r.capabilities = TOPO_CAP_BRIDGE;
    TEST_ASSERT_TRUE(topoNormalizeMibRecord(&r, 7, "", &g, &e));
    TEST_ASSERT_EQUAL_STRING("sw-11-22-33-44-55-66", g.gearId);
}

/* ---- /api/netgear assembly: attached counts + uplink chain + dedupe ---- */

static void mkgear(serverNetGear *g, const char *id, const char *kind,
                   const char *uplink)
{
    memset(g, 0, sizeof *g);
    snprintf(g->gearId, sizeof g->gearId, "%s", id);
    snprintf(g->name,   sizeof g->name,   "%s", id);
    snprintf(g->kind,   sizeof g->kind,   "%s", kind);
    if (uplink) snprintf(g->uplinkGearId, sizeof g->uplinkGearId, "%s", uplink);
}

static void mkedge(serverLldpEdge *e, uint64_t nodeId, const char *gearId)
{
    memset(e, 0, sizeof *e);
    e->nodeId = nodeId;
    snprintf(e->gearId, sizeof e->gearId, "%s", gearId);
}

static int viewIdx(const topoNetGearView *v, size_t n, const char *id)
{
    return topoFindGear(v, n, id);
}

static void test_assemble_netgear(void)
{
    /* gw <- sw-core <- sw-edge ; nodes attach to sw-edge. */
    serverNetGear gear[3];
    serverLldpEdge edges[5];
    topoNetGearView view[8];
    size_t count = 8;
    int i;

    mkgear(&gear[0], "gw", "gateway", "");
    mkgear(&gear[1], "sw-core", "switch", "gw");
    mkgear(&gear[2], "sw-edge", "switch", "sw-core");

    /* node 100 attaches to sw-edge over two interfaces (counts once); node 101
     * also on sw-edge; node 102 on sw-core; one gear-to-gear edge (nodeId 0). */
    mkedge(&edges[0], 100, "sw-edge");
    mkedge(&edges[1], 100, "sw-edge");   /* duplicate node on same gear */
    mkedge(&edges[2], 101, "sw-edge");
    mkedge(&edges[3], 102, "sw-core");
    mkedge(&edges[4], 0,   "gw");        /* gear-to-gear, not an attached node */

    TEST_ASSERT_EQUAL_INT(SOLARI_OK,
        topoAssembleNetGear(gear, 3, edges, 5, view, &count));
    TEST_ASSERT_EQUAL_UINT(3, count);

    i = viewIdx(view, count, "sw-edge");
    TEST_ASSERT_TRUE(i >= 0);
    TEST_ASSERT_EQUAL_UINT(2, view[i].attachedNodes);  /* 100 (once) + 101 */
    TEST_ASSERT_EQUAL_UINT(2, view[i].uplinkDepth);    /* edge->core->gw      */

    i = viewIdx(view, count, "sw-core");
    TEST_ASSERT_EQUAL_UINT(1, view[i].attachedNodes);  /* 102                 */
    TEST_ASSERT_EQUAL_UINT(1, view[i].uplinkDepth);    /* core->gw            */

    i = viewIdx(view, count, "gw");
    TEST_ASSERT_EQUAL_UINT(0, view[i].attachedNodes);  /* only gear edge      */
    TEST_ASSERT_EQUAL_UINT(0, view[i].uplinkDepth);    /* root                */
}

static void test_assemble_dedupe(void)
{
    /* Same gearId reported twice collapses to one row; attached counts still
     * accumulate across all matching edges. */
    serverNetGear gear[3];
    serverLldpEdge edges[2];
    topoNetGearView view[8];
    size_t count = 8;
    int i;

    mkgear(&gear[0], "sw-1", "switch", "");
    mkgear(&gear[1], "sw-1", "switch", "");   /* duplicate gearId */
    mkgear(&gear[2], "",     "other",  "");   /* empty id skipped */

    mkedge(&edges[0], 5, "sw-1");
    mkedge(&edges[1], 6, "sw-1");

    TEST_ASSERT_EQUAL_INT(SOLARI_OK,
        topoAssembleNetGear(gear, 3, edges, 2, view, &count));
    TEST_ASSERT_EQUAL_UINT(1, count);                  /* deduped + empty drop */
    i = viewIdx(view, count, "sw-1");
    TEST_ASSERT_EQUAL_UINT(2, view[i].attachedNodes);
}

static void test_assemble_cycle_and_overflow(void)
{
    serverNetGear gear[2];
    topoNetGearView view[1];
    size_t count;

    /* Cyclic uplinks: a->b->a must terminate at the hop cap, not loop. */
    mkgear(&gear[0], "a", "switch", "b");
    mkgear(&gear[1], "b", "switch", "a");
    count = 8;
    {
        topoNetGearView big[2];
        size_t c = 2;
        TEST_ASSERT_EQUAL_INT(SOLARI_OK,
            topoAssembleNetGear(gear, 2, NULL, 0, big, &c));
        TEST_ASSERT_EQUAL_UINT(2, c);
        TEST_ASSERT_EQUAL_UINT(TOPO_MAX_UPLINK_HOPS, big[0].uplinkDepth);
    }

    /* Capacity too small -> ERR_BUFFER_FULL. */
    count = 1;
    TEST_ASSERT_EQUAL_INT(ERR_BUFFER_FULL,
        topoAssembleNetGear(gear, 2, NULL, 0, view, &count));
}

static void test_snmp_seam_off_by_default(void)
{
    topoLldpMibRecord recs[4];
    size_t n = 4;
    /* With SOLARI_WITH_SNMP undefined the fetch seam is a clean no-op. */
    TEST_ASSERT_EQUAL_INT(ERR_PLATFORM,
        topoSnmpFetchLldp("10.0.0.1", "public", recs, &n));
    TEST_ASSERT_EQUAL_UINT(0, n);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_split_field);
    RUN_TEST(test_parse_int);
    RUN_TEST(test_derive_gear_id);
    RUN_TEST(test_derive_seg_id);
    RUN_TEST(test_contains_ci);
    RUN_TEST(test_infer_gear_kind_caps);
    RUN_TEST(test_infer_gear_kind_strings);
    RUN_TEST(test_normalize_mib_record);
    RUN_TEST(test_normalize_mib_id_fallback);
    RUN_TEST(test_assemble_netgear);
    RUN_TEST(test_assemble_dedupe);
    RUN_TEST(test_assemble_cycle_and_overflow);
    RUN_TEST(test_snmp_seam_off_by_default);
    return UNITY_END();
}
