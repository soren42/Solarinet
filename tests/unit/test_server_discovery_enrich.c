/* Pure discovery enrichment tests: no DB, no nmap/SNMP/avahi required. */
#include "unity.h"
#include "serverDiscoveryEnrich.h"
#include "serverOui.h"

#include <string.h>

void setUp(void) {}
void tearDown(void) {}

static void test_infer_role_printer(void)
{
    TEST_ASSERT_EQUAL_STRING("printer",
        discoverInferRole("[\"ipp:631\"]", "", "", ""));
    TEST_ASSERT_EQUAL_STRING("printer",
        discoverInferRole("[]", "Office Printer", "", ""));
}

static void test_infer_role_nas(void)
{
    TEST_ASSERT_EQUAL_STRING("nas",
        discoverInferRole("[\"smb:445\",\"http:5000\"]", "Synology DSM", "Synology", ""));
}

static void test_infer_role_camera(void)
{
    TEST_ASSERT_EQUAL_STRING("camera",
        discoverInferRole("[\"rtsp:554\"]", "", "", ""));
}

static void test_infer_role_network_ap_gateway(void)
{
    TEST_ASSERT_EQUAL_STRING("ap",
        discoverInferRole("[\"https:443\"]", "UniFi AP bridge", "Ubiquiti", "_ubnt._tcp"));
    TEST_ASSERT_EQUAL_STRING("gateway",
        discoverInferRole("[\"https:443\"]", "EdgeRouter gateway", "Ubiquiti", ""));
    TEST_ASSERT_EQUAL_STRING("network",
        discoverInferRole("[]", "managed switch bridge", "Cisco", ""));
}

static void test_infer_role_iot_workstation_server_host(void)
{
    TEST_ASSERT_EQUAL_STRING("iot",
        discoverInferRole("[\"mqtt:1883\"]", "", "Espressif", ""));
    TEST_ASSERT_EQUAL_STRING("workstation",
        discoverInferRole("[\"rdp:3389\"]", "", "Microsoft", ""));
    TEST_ASSERT_EQUAL_STRING("server",
        discoverInferRole("[\"ssh:22\",\"https:443\"]", "", "Dell", ""));
    TEST_ASSERT_EQUAL_STRING("host",
        discoverInferRole("[]", "", "", ""));
}

static void test_lookup_oui_known_and_unknown(void)
{
    TEST_ASSERT_TRUE(serverOuiSubsetSize() >= 40);
    TEST_ASSERT_EQUAL_STRING("Ubiquiti", lookupOui("04:18:d6:12:34:56"));
    TEST_ASSERT_EQUAL_STRING("Raspberry Pi", lookupOui("b8-27-eb-aa-bb-cc"));
    TEST_ASSERT_EQUAL_STRING("VMware", lookupOui("00:0c:29:00:00:01"));
    TEST_ASSERT_EQUAL_STRING("", lookupOui("12:34:56:78:90:ab"));
}

static void test_parse_nmap_xml(void)
{
    const char *xml =
        "<nmaprun><host>"
        "<address addr=\"192.168.1.20\" addrtype=\"ipv4\"/>"
        "<address addr=\"04:18:D6:AA:BB:CC\" addrtype=\"mac\" vendor=\"Ubiquiti Inc.\"/>"
        "<ports><port protocol=\"tcp\" portid=\"22\"><service name=\"ssh\" product=\"OpenSSH\" version=\"9.6\"/></port></ports>"
        "<os><osmatch name=\"Linux 5.10 - 6.1\" accuracy=\"96\"/></os>"
        "</host></nmaprun>";
    char mac[18], vendor[64], os[64], svc[128];

    discoverParseNmapXml(xml, mac, sizeof mac, vendor, sizeof vendor,
                         os, sizeof os, svc, sizeof svc);
    TEST_ASSERT_EQUAL_STRING("04:18:D6:AA:BB:CC", mac);
    TEST_ASSERT_EQUAL_STRING("Ubiquiti Inc.", vendor);
    TEST_ASSERT_EQUAL_STRING("Linux 5.10 - 6.1", os);
    TEST_ASSERT_EQUAL_STRING("OpenSSH 9.6", svc);
}

static void test_parse_arp_mac(void)
{
    const char *arp =
        "IP address       HW type     Flags       HW address            Mask     Device\n"
        "192.168.1.10     0x1         0x2         b8:27:eb:aa:bb:cc     *        eth0\n";
    char mac[18];
    TEST_ASSERT_EQUAL_INT(1, discoverParseArpMac(arp, "192.168.1.10", mac, sizeof mac));
    TEST_ASSERT_EQUAL_STRING("b8:27:eb:aa:bb:cc", mac);
    TEST_ASSERT_EQUAL_INT(0, discoverParseArpMac(arp, "192.168.1.11", mac, sizeof mac));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_infer_role_printer);
    RUN_TEST(test_infer_role_nas);
    RUN_TEST(test_infer_role_camera);
    RUN_TEST(test_infer_role_network_ap_gateway);
    RUN_TEST(test_infer_role_iot_workstation_server_host);
    RUN_TEST(test_lookup_oui_known_and_unknown);
    RUN_TEST(test_parse_nmap_xml);
    RUN_TEST(test_parse_arp_mac);
    return UNITY_END();
}
