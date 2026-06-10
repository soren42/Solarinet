/*
 * test_platlinux.c - exercises the Linux PAL (src/client/plat/platLinux.c)
 * against the live host. Asserts each call returns SOLARI_OK and yields
 * structurally sane values; not a golden test (host state varies), a sanity
 * and contract test.
 */
#include "unity.h"
#include "platOS.h"
#include "solari/solariError.h"

#include <stdio.h>
#include <string.h>
#include <unistd.h>

void setUp(void) {}
void tearDown(void) {}

static void test_identity(void)
{
    char fqdn[SOLARI_FQDN_MAX], os[SOLARI_OSNAME_MAX], arch[SOLARI_ARCH_MAX];
    uint8_t cores = 0;

    TEST_ASSERT_EQUAL_INT(SOLARI_OK, platHostFqdn(fqdn, sizeof fqdn));
    TEST_ASSERT_TRUE(strlen(fqdn) > 0);

    TEST_ASSERT_EQUAL_INT(SOLARI_OK, platOsName(os, sizeof os));
    TEST_ASSERT_EQUAL_STRING_LEN("Linux", os, 5);   /* "Linux <release>" */

    TEST_ASSERT_EQUAL_INT(SOLARI_OK, platArch(arch, sizeof arch));
    TEST_ASSERT_TRUE(strlen(arch) > 0);

    TEST_ASSERT_EQUAL_INT(SOLARI_OK, platCpuCount(&cores));
    TEST_ASSERT_TRUE(cores >= 1);
}

static void test_invalid_args(void)
{
    uint8_t cores = 0;
    TEST_ASSERT_EQUAL_INT(ERR_INVALID_ARG, platHostFqdn(NULL, 10));
    TEST_ASSERT_EQUAL_INT(ERR_INVALID_ARG, platOsName(NULL, 10));
    TEST_ASSERT_EQUAL_INT(ERR_INVALID_ARG, platCpuCount(NULL));
    TEST_ASSERT_EQUAL_INT(SOLARI_OK, platCpuCount(&cores));   /* sanity */
}

static void test_cpu_load(void)
{
    uint32_t load[SOLARI_MAX_CORES];
    uint8_t n = 0, i;
    TEST_ASSERT_EQUAL_INT(SOLARI_OK, platCpuLoad(load, SOLARI_MAX_CORES, &n));
    TEST_ASSERT_TRUE(n >= 1);
    for (i = 0; i < n; i++) TEST_ASSERT_TRUE(load[i] <= 1000u);  /* permille */
}

static void test_mem(void)
{
    uint64_t ru = 0, rt = 0, su = 0, st = 0;
    TEST_ASSERT_EQUAL_INT(SOLARI_OK, platMemInfo(&ru, &rt, &su, &st));
    TEST_ASSERT_TRUE(rt > 0);
    TEST_ASSERT_TRUE(ru <= rt);
    TEST_ASSERT_TRUE(su <= st);
}

static void test_disks(void)
{
    solariDiskEntry d[SOLARI_MAX_DISKS];
    uint8_t n = 0, i;
    TEST_ASSERT_EQUAL_INT(SOLARI_OK, platDiskFree(d, SOLARI_MAX_DISKS, &n));
    TEST_ASSERT_TRUE(n >= 1);                       /* at least "/" */
    for (i = 0; i < n; i++) {
        TEST_ASSERT_TRUE(d[i].totalKb > 0);
        TEST_ASSERT_TRUE(d[i].freeKb <= d[i].totalKb);
        TEST_ASSERT_TRUE(strlen(d[i].mount) > 0);
    }
}

static void test_net_and_usb(void)
{
    solariIfaceEntry ifs[SOLARI_MAX_IFACES];
    solariUsbEntry usb[SOLARI_MAX_USB];
    uint8_t n = 0;
    TEST_ASSERT_EQUAL_INT(SOLARI_OK, platNetIfaces(ifs, SOLARI_MAX_IFACES, &n));
    /* count may be 0 in a minimal namespace; just require a clean return */
    TEST_ASSERT_EQUAL_INT(SOLARI_OK, platUsbThroughput(usb, SOLARI_MAX_USB, &n));
    TEST_ASSERT_EQUAL_UINT8(0, n);                  /* reference reports none */
}

static void test_proc_inspect(void)
{
    solariProcEntry p;
    char comm[64];
    FILE *f;

    /* a name that cannot be running -> "watched but down" sentinel */
    TEST_ASSERT_EQUAL_INT(SOLARI_OK, platProcInspect("nope_no_such_proc_zzz", &p));
    TEST_ASSERT_EQUAL_INT(-1, p.pid);

    /* our own comm -> found, with a real pid and at least stdin/out/err open */
    f = fopen("/proc/self/comm", "r");
    TEST_ASSERT_NOT_NULL(f);
    TEST_ASSERT_NOT_NULL(fgets(comm, sizeof comm, f));
    fclose(f);
    comm[strcspn(comm, "\n")] = '\0';
    TEST_ASSERT_EQUAL_INT(SOLARI_OK, platProcInspect(comm, &p));
    TEST_ASSERT_TRUE(p.pid > 0);
    TEST_ASSERT_TRUE(p.nFiles >= 1);
}

static void test_proc_alive(void)
{
    bool alive = false;
    TEST_ASSERT_EQUAL_INT(SOLARI_OK, platProcessAlive((int64_t)getpid(), &alive));
    TEST_ASSERT_TRUE(alive);
    TEST_ASSERT_EQUAL_INT(ERR_INVALID_ARG, platProcessAlive(-1, &alive));
}

static void test_discovery_topology(void)
{
    platNeighbor nb[16];
    platIfaceCidr cidrs[8];
    platUplink up;
    uint8_t n = 0, i;

    /* ARP table: count may legitimately be 0, but the call must succeed */
    TEST_ASSERT_EQUAL_INT(SOLARI_OK, platArpNeighbors(nb, 16, &n));

    /* at least one non-loopback IPv4 interface, each with a CIDR */
    TEST_ASSERT_EQUAL_INT(SOLARI_OK, platIfaceCidrs(cidrs, 8, &n));
    TEST_ASSERT_TRUE(n >= 1);
    for (i = 0; i < n; i++) {
        TEST_ASSERT_NOT_NULL(strchr(cidrs[i].cidr, '/'));
        TEST_ASSERT_TRUE(strlen(cidrs[i].ifName) > 0);
    }

    /* a default route exists on this host */
    TEST_ASSERT_EQUAL_INT(SOLARI_OK, platDefaultUplink(&up));
    TEST_ASSERT_TRUE(strlen(up.gatewayIp) > 0);
    TEST_ASSERT_TRUE(strlen(up.localIf) > 0);
}

static void test_log_stat(void)
{
    char path[64];
    uint64_t sizeNow = 0, off = 0;
    uint32_t matches = 0;
    FILE *f;

    snprintf(path, sizeof path, "/tmp/solari_logtest_%ld", (long)getpid());
    f = fopen(path, "w");
    TEST_ASSERT_NOT_NULL(f);
    fputs("info: ok\nERROR: boom\nwarn: meh\nERROR: again\n", f);
    fclose(f);

    /* first pass: 2 lines match "ERROR" */
    TEST_ASSERT_EQUAL_INT(SOLARI_OK, platLogStat(path, "ERROR", &sizeNow, &matches, &off));
    TEST_ASSERT_TRUE(sizeNow > 0);
    TEST_ASSERT_EQUAL_UINT32(2, matches);
    TEST_ASSERT_EQUAL_UINT64(sizeNow, off);

    /* append one matching line: only the new content is counted */
    f = fopen(path, "a");
    fputs("ERROR: third\n", f);
    fclose(f);
    TEST_ASSERT_EQUAL_INT(SOLARI_OK, platLogStat(path, "ERROR", &sizeNow, &matches, &off));
    TEST_ASSERT_EQUAL_UINT32(1, matches);

    /* NULL regex counts every new line (none new now) */
    TEST_ASSERT_EQUAL_INT(SOLARI_OK, platLogStat(path, NULL, &sizeNow, &matches, &off));
    TEST_ASSERT_EQUAL_UINT32(0, matches);

    remove(path);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_identity);
    RUN_TEST(test_invalid_args);
    RUN_TEST(test_cpu_load);
    RUN_TEST(test_mem);
    RUN_TEST(test_disks);
    RUN_TEST(test_net_and_usb);
    RUN_TEST(test_proc_inspect);
    RUN_TEST(test_proc_alive);
    RUN_TEST(test_discovery_topology);
    RUN_TEST(test_log_stat);
    return UNITY_END();
}
