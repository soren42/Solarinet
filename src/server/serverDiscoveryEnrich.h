/* serverDiscoveryEnrich.h - pure discovery enrichment helpers. */
#ifndef SOLARI_SERVER_DISCOVERY_ENRICH_H
#define SOLARI_SERVER_DISCOVERY_ENRICH_H

#include <stddef.h>

const char *discoverInferRole(const char *servicesJson, const char *sysDescr,
                              const char *vendor, const char *mdnsTypes);
void discoverParseNmapXml(const char *xml, char *mac, size_t macCap,
                          char *vendor, size_t vendorCap,
                          char *osName, size_t osCap,
                          char *serviceDescr, size_t svcCap);
int discoverParseArpMac(const char *arpText, const char *ip,
                        char *mac, size_t macCap);

#endif /* SOLARI_SERVER_DISCOVERY_ENRICH_H */
