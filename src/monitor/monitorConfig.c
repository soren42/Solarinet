/*
 * monitorConfig.c - monitor .conf mapping + target parsing (sec 8, 13).
 */
#define _GNU_SOURCE

#include "monitor.h"
#include "solari/solariConfig.h"

#include <netdb.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

solariStatus monitorHostFqdn(char *out, size_t cap)
{
    char host[256];
    struct addrinfo hints, *res = NULL;
    if (!out || !cap) return ERR_INVALID_ARG;
    if (gethostname(host, sizeof host) != 0) return ERR_PLATFORM;
    host[sizeof host - 1] = '\0';
    if (strchr(host, '.')) { strncpy(out, host, cap - 1); out[cap - 1] = '\0'; return SOLARI_OK; }
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_UNSPEC;
    hints.ai_flags  = AI_CANONNAME;
    if (getaddrinfo(host, NULL, &hints, &res) == 0 && res && res->ai_canonname) {
        strncpy(out, res->ai_canonname, cap - 1); out[cap - 1] = '\0';
        freeaddrinfo(res);
        return SOLARI_OK;
    }
    if (res) freeaddrinfo(res);
    strncpy(out, host, cap - 1); out[cap - 1] = '\0';
    return SOLARI_OK;
}

void monitorConfigDefaults(monitorConfig *out)
{
    memset(out, 0, sizeof *out);
    out->roundIntervalSec = 30;
    out->probesPerRound   = 5;
    out->probeTimeoutMs   = 1000;
    out->replFactor       = 2;
    out->gossipIntervalSec = 20;
    out->peerTtlSec        = 90;
}

static probeProto protoFromStr(const char *s)
{
    if (!strcmp(s, "tcp"))  return PROBE_TCP;
    if (!strcmp(s, "udp"))  return PROBE_UDP;
    if (!strcmp(s, "icmp")) return PROBE_ICMP;
    return (probeProto)0;
}

solariStatus monitorParseTarget(const char *spec, monitorTarget *out)
{
    char work[MONITOR_TARGETID_MAX];
    const char *sep;
    char *c1, *c2, *rest;
    size_t leftLen, L;

    if (!spec || !out) return ERR_INVALID_ARG;
    memset(out, 0, sizeof *out);

    sep = strstr(spec, " : ");                       /* "<spec> : <label>" */
    leftLen = sep ? (size_t)(sep - spec) : strlen(spec);
    if (leftLen >= sizeof work) leftLen = sizeof work - 1;
    memcpy(work, spec, leftLen);
    work[leftLen] = '\0';
    if (sep) { strncpy(out->label, sep + 3, sizeof out->label - 1); }

    L = strlen(work);
    while (L > 0 && work[L - 1] == ' ') work[--L] = '\0';   /* trim */

    c1 = strchr(work, ':');                          /* proto:rest */
    if (!c1) return ERR_INVALID_ARG;
    *c1 = '\0';
    out->proto = protoFromStr(work);
    if (out->proto == 0) return ERR_INVALID_ARG;
    rest = c1 + 1;

    if (out->proto == PROBE_ICMP) {
        strncpy(out->host, rest, sizeof out->host - 1);
        out->port = 0;
        snprintf(out->targetId, sizeof out->targetId, "icmp:%s", out->host);
    } else {
        c2 = strrchr(rest, ':');                     /* host:port */
        if (!c2) return ERR_INVALID_ARG;
        *c2 = '\0';
        strncpy(out->host, rest, sizeof out->host - 1);
        out->port = (uint16_t)strtoul(c2 + 1, NULL, 10);
        snprintf(out->targetId, sizeof out->targetId, "%s:%s:%u",
                 out->proto == PROBE_TCP ? "tcp" : "udp", out->host, (unsigned)out->port);
    }
    return SOLARI_OK;
}

solariStatus monitorConfigFromFile(const char *path, monitorConfig *out)
{
    solariConfig *c = NULL;
    solariStatus rc;
    size_t n, i;

    if (!path || !out) return ERR_INVALID_ARG;
    monitorConfigDefaults(out);
    rc = solariConfigLoad(path, &c);
    if (rc != SOLARI_OK) return rc;

    strncpy(out->hostFqdn, solariConfigGetStr(c, "identity", "hostFqdn", ""),
            sizeof out->hostFqdn - 1);
    { const char *nid = solariConfigGetStr(c, "identity", "nodeId", "");
      if (nid[0]) out->nodeId = (uint64_t)strtoull(nid, NULL, 0); }

    out->roundIntervalSec = (uint32_t)solariConfigGetInt(c, "probe", "roundIntervalSec", 30);
    out->probesPerRound   = (uint16_t)solariConfigGetInt(c, "probe", "probesPerRound", 5);
    out->probeTimeoutMs   = (uint32_t)solariConfigGetInt(c, "probe", "probeTimeoutMs", 1000);
    out->replFactor       = (uint8_t) solariConfigGetInt(c, "probe", "replFactorHint", 2);

    strncpy(out->primaryUrl,  solariConfigGetStr(c, "server", "primaryUrl", ""),  sizeof out->primaryUrl - 1);
    strncpy(out->failoverUrl, solariConfigGetStr(c, "server", "failoverUrl", ""), sizeof out->failoverUrl - 1);
    out->useTls = strncmp(out->primaryUrl, "tls+", 4) == 0;
    strncpy(out->caFile,   solariConfigGetStr(c, "tls", "caFile", ""),   sizeof out->caFile - 1);
    strncpy(out->certFile, solariConfigGetStr(c, "tls", "certFile", ""), sizeof out->certFile - 1);
    strncpy(out->keyFile,  solariConfigGetStr(c, "tls", "keyFile", ""),  sizeof out->keyFile - 1);
    strncpy(out->spoolDb,  solariConfigGetStr(c, "probe", "spoolDb", ""), sizeof out->spoolDb - 1);

    n = solariConfigCount(c, "probe", "target");
    for (i = 0; i < n && out->targetCount < MONITOR_MAX_TARGETS; i++) {
        const char *t = solariConfigGetStrAt(c, "probe", "target", i, "");
        if (t[0] == '\0') continue;
        if (monitorParseTarget(t, &out->targets[out->targetCount]) == SOLARI_OK)
            out->targetCount++;
    }

    /* [peer] mesh */
    strncpy(out->gossipUrl, solariConfigGetStr(c, "peer", "gossipUrl", ""),
            sizeof out->gossipUrl - 1);
    out->gossipIntervalSec = (uint32_t)solariConfigGetInt(c, "peer", "gossipIntervalSec", 20);
    out->peerTtlSec        = (uint32_t)solariConfigGetInt(c, "peer", "peerTtlSec", 90);
    n = solariConfigCount(c, "peer", "peer");
    for (i = 0; i < n && out->peerCount < MONITOR_MAX_FLEET; i++) {
        const char *u = solariConfigGetStrAt(c, "peer", "peer", i, "");
        if (u[0] == '\0') continue;
        strncpy(out->peerUrls[out->peerCount], u, sizeof out->peerUrls[0] - 1);
        out->peerCount++;
    }

    solariConfigFree(c);
    return SOLARI_OK;
}
