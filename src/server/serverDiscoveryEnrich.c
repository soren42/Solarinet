/* serverDiscoveryEnrich.c - pure parsers and role inference for discovery. */
#include "serverDiscoveryEnrich.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

static int containsNoCase(const char *hay, const char *needle)
{
    size_t nlen, i;
    if (!hay || !needle || !needle[0]) return 0;
    nlen = strlen(needle);
    for (i = 0; hay[i]; i++) {
        size_t j = 0;
        while (j < nlen && hay[i + j] &&
               tolower((unsigned char)hay[i + j]) == tolower((unsigned char)needle[j]))
            j++;
        if (j == nlen) return 1;
    }
    return 0;
}

static int hasPort(const char *servicesJson, const char *port)
{
    char needle[16];
    snprintf(needle, sizeof needle, ":%s", port);
    return containsNoCase(servicesJson, needle);
}

const char *discoverInferRole(const char *servicesJson, const char *sysDescr,
                              const char *vendor, const char *mdnsTypes)
{
    if (hasPort(servicesJson, "9100") || hasPort(servicesJson, "631") ||
        containsNoCase(sysDescr, "printer") || containsNoCase(mdnsTypes, "printer"))
        return "printer";

    if (containsNoCase(servicesJson, "rtsp") || hasPort(servicesJson, "554") ||
        containsNoCase(sysDescr, "camera") || containsNoCase(vendor, "hikvision") ||
        containsNoCase(vendor, "dahua"))
        return "camera";

    if ((hasPort(servicesJson, "445") || hasPort(servicesJson, "2049") ||
         hasPort(servicesJson, "5000")) &&
        (containsNoCase(vendor, "synology") || containsNoCase(sysDescr, "synology") ||
         containsNoCase(sysDescr, "nas")))
        return "nas";

    if (containsNoCase(vendor, "ubiquiti") &&
        (containsNoCase(sysDescr, "ap") || containsNoCase(sysDescr, "unifi") ||
         containsNoCase(mdnsTypes, "_ubnt") || containsNoCase(mdnsTypes, "_http")))
        return "ap";

    if (containsNoCase(sysDescr, "gateway") || containsNoCase(sysDescr, "router") ||
        containsNoCase(mdnsTypes, "gateway"))
        return "gateway";

    if (containsNoCase(vendor, "ubiquiti") || containsNoCase(vendor, "cisco") ||
        containsNoCase(vendor, "netgear") || containsNoCase(vendor, "tp-link") ||
        containsNoCase(sysDescr, "switch") || containsNoCase(sysDescr, "bridge"))
        return "network";

    if (containsNoCase(vendor, "espressif") || containsNoCase(vendor, "tuya") ||
        containsNoCase(servicesJson, "mqtt") || hasPort(servicesJson, "1883"))
        return "iot";

    if (hasPort(servicesJson, "3389") || containsNoCase(sysDescr, "windows workstation") ||
        containsNoCase(mdnsTypes, "_device-info"))
        return "workstation";

    if (hasPort(servicesJson, "22") || hasPort(servicesJson, "80") ||
        hasPort(servicesJson, "443"))
        return "server";

    return "host";
}

static void copyAttr(const char *tag, const char *attr, char *out, size_t cap)
{
    const char *p, *q;
    size_t alen, n;
    if (!tag || !attr || !out || cap == 0 || out[0]) return;
    alen = strlen(attr);
    p = tag;
    while ((p = strstr(p, attr)) != NULL) {
        if (p == tag || p[-1] == ' ' || p[-1] == '\t' || p[-1] == '<') {
            p += alen;
            if (p[0] == '=' && p[1] == '"') {
                p += 2;
                q = strchr(p, '"');
                if (!q) return;
                n = (size_t)(q - p);
                if (n >= cap) n = cap - 1;
                memcpy(out, p, n);
                out[n] = '\0';
                return;
            }
        } else {
            p += alen;
        }
    }
}

void discoverParseNmapXml(const char *xml, char *mac, size_t macCap,
                          char *vendor, size_t vendorCap,
                          char *osName, size_t osCap,
                          char *serviceDescr, size_t svcCap)
{
    const char *p;

    if (mac && macCap) mac[0] = '\0';
    if (vendor && vendorCap) vendor[0] = '\0';
    if (osName && osCap) osName[0] = '\0';
    if (serviceDescr && svcCap) serviceDescr[0] = '\0';
    if (!xml) return;

    p = xml;
    while ((p = strstr(p, "<address")) != NULL) {
        const char *end = strchr(p, '>');
        char tag[512];
        size_t n;
        if (!end) break;
        n = (size_t)(end - p + 1);
        if (n >= sizeof tag) n = sizeof tag - 1;
        memcpy(tag, p, n);
        tag[n] = '\0';
        if (containsNoCase(tag, "addrtype=\"mac\"")) {
            copyAttr(tag, "addr", mac, macCap);
            copyAttr(tag, "vendor", vendor, vendorCap);
            break;
        }
        p = end + 1;
    }

    p = strstr(xml, "<osmatch");
    if (p) {
        const char *end = strchr(p, '>');
        char tag[768];
        size_t n;
        if (end) {
            n = (size_t)(end - p + 1);
            if (n >= sizeof tag) n = sizeof tag - 1;
            memcpy(tag, p, n);
            tag[n] = '\0';
            copyAttr(tag, "name", osName, osCap);
        }
    }
    if (osName && osCap && !osName[0]) {
        p = strstr(xml, "<osclass");
        if (p) {
            const char *end = strchr(p, '>');
            char tag[768], family[128], gen[64];
            size_t n;
            family[0] = gen[0] = '\0';
            if (end) {
                n = (size_t)(end - p + 1);
                if (n >= sizeof tag) n = sizeof tag - 1;
                memcpy(tag, p, n);
                tag[n] = '\0';
                copyAttr(tag, "osfamily", family, sizeof family);
                copyAttr(tag, "osgen", gen, sizeof gen);
                if (family[0] && gen[0]) snprintf(osName, osCap, "%s %s", family, gen);
                else if (family[0]) snprintf(osName, osCap, "%s", family);
            }
        }
    }

    p = xml;
    while ((p = strstr(p, "<service")) != NULL) {
        const char *end = strchr(p, '>');
        char tag[1024], product[192], version[128];
        size_t n;
        product[0] = version[0] = '\0';
        if (!end) break;
        n = (size_t)(end - p + 1);
        if (n >= sizeof tag) n = sizeof tag - 1;
        memcpy(tag, p, n);
        tag[n] = '\0';
        copyAttr(tag, "product", product, sizeof product);
        copyAttr(tag, "version", version, sizeof version);
        if (product[0]) {
            if (version[0]) snprintf(serviceDescr, svcCap, "%s %s", product, version);
            else snprintf(serviceDescr, svcCap, "%s", product);
            return;
        }
        p = end + 1;
    }
}

int discoverParseArpMac(const char *arpText, const char *ip, char *mac, size_t macCap)
{
    const char *line;
    if (mac && macCap) mac[0] = '\0';
    if (!arpText || !ip || !mac || macCap == 0) return 0;
    line = arpText;
    while (*line) {
        const char *next = strchr(line, '\n');
        size_t len = next ? (size_t)(next - line) : strlen(line);
        char row[256], ipField[64], hw[32], flags[32], macField[32];
        if (len >= sizeof row) len = sizeof row - 1;
        memcpy(row, line, len);
        row[len] = '\0';
        ipField[0] = hw[0] = flags[0] = macField[0] = '\0';
        if (sscanf(row, "%63s %31s %31s %31s", ipField, hw, flags, macField) == 4 &&
            strcmp(ipField, ip) == 0 && strcmp(macField, "00:00:00:00:00:00") != 0) {
            snprintf(mac, macCap, "%s", macField);
            return 1;
        }
        if (!next) break;
        line = next + 1;
    }
    return 0;
}
