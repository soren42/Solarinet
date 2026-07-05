/*
 * serverOui.c - small OUI lookup table for homelab discovery.
 *
 * This is an intentionally curated subset of common vendors seen in home and
 * small-lab networks, not the full IEEE registry. Extend the table as needed.
 */
#include "serverOui.h"

#include <string.h>

typedef struct {
    const char *prefix;
    const char *vendor;
} serverOuiEntry;

static const serverOuiEntry OUI_TABLE[] = {
    { "0418D6", "Ubiquiti" }, { "24A43C", "Ubiquiti" },
    { "788A20", "Ubiquiti" }, { "F09FC2", "Ubiquiti" },
    { "74ACB9", "Ubiquiti" },
    { "B827EB", "Raspberry Pi" }, { "DCA632", "Raspberry Pi" },
    { "E45F01", "Raspberry Pi" },
    { "001B21", "Intel" }, { "3CFDFE", "Intel" }, { "F44D30", "Intel" },
    { "D067E5", "Dell" }, { "F8B156", "Dell" }, { "18DBF2", "Dell" },
    { "3C52A1", "HP" }, { "0025B3", "HP" }, { "6CC217", "HPE" },
    { "0016CB", "Apple" }, { "3C0754", "Apple" }, { "A4C361", "Apple" },
    { "24A160", "Espressif" }, { "AC67B2", "Espressif" },
    { "C8C9A3", "Espressif" }, { "7C9EBD", "Espressif" },
    { "001132", "Synology" }, { "9009D0", "Synology" },
    { "50C7BF", "TP-Link" }, { "A42BB0", "TP-Link" },
    { "F4F26D", "TP-Link" },
    { "A040A0", "Netgear" }, { "C03F0E", "Netgear" },
    { "9C3DCF", "Netgear" },
    { "001B54", "Cisco" }, { "0026CB", "Cisco" }, { "F872EA", "Cisco" },
    { "000C29", "VMware" }, { "005056", "VMware" }, { "001C14", "VMware" },
    { "00155D", "Microsoft" }, { "7CED8D", "Microsoft" },
    { "F4F5D8", "Google/Nest" }, { "641666", "Google/Nest" },
    { "D85E0B", "Google/Nest" },
    { "F0D2F1", "Amazon" }, { "68DBCA", "Amazon" },
    { "001632", "Samsung" }, { "FC8F90", "Samsung" },
    { "F48139", "Canon" }, { "3C2AF4", "Brother" },
    { "C056E3", "Hikvision" }, { "3C16DB", "Dahua" },
    { "105A17", "Tuya" }, { "60F677", "Lenovo" },
    { "2C4D54", "ASUSTek" }
};

static char ouiHex(char c)
{
    if (c >= '0' && c <= '9') return c;
    if (c >= 'a' && c <= 'f') return (char)(c - 'a' + 'A');
    if (c >= 'A' && c <= 'F') return c;
    return '\0';
}

const char *lookupOui(const char *mac)
{
    char key[7];
    size_t i, n = 0;

    if (!mac) return "";
    for (i = 0; mac[i] && n < 6; i++) {
        char h = ouiHex(mac[i]);
        if (h) key[n++] = h;
    }
    if (n != 6) return "";
    key[6] = '\0';

    for (i = 0; i < sizeof OUI_TABLE / sizeof OUI_TABLE[0]; i++)
        if (strcmp(key, OUI_TABLE[i].prefix) == 0) return OUI_TABLE[i].vendor;
    return "";
}

unsigned serverOuiSubsetSize(void)
{
    return (unsigned)(sizeof OUI_TABLE / sizeof OUI_TABLE[0]);
}
