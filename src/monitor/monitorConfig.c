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

typedef struct {
    const char   *name;
    probeProto    proto;
    probeAppCheck appCheck;
    uint16_t      defaultPort;
} targetProtoDesc;

static const targetProtoDesc *protoFromStr(const char *s)
{
    static const targetProtoDesc protos[] = {
        { "tcp",   PROBE_TCP,  APP_CHECK_NONE, 0    },
        { "udp",   PROBE_UDP,  APP_CHECK_NONE, 0    },
        { "icmp",  PROBE_ICMP, APP_CHECK_NONE, 0    },
        { "dns",   PROBE_UDP,  APP_CHECK_DNS,  53   },
        { "ldap",  PROBE_TCP,  APP_CHECK_LDAP, 389  },
        { "http",  PROBE_TCP,  APP_CHECK_HTTP, 0    },
        { "mysql", PROBE_TCP,  APP_CHECK_MYSQL, 3306 },
        { "amqp",  PROBE_TCP,  APP_CHECK_AMQP, 5672 }
    };
    size_t i;
    for (i = 0; i < sizeof protos / sizeof protos[0]; i++)
        if (!strcmp(s, protos[i].name)) return &protos[i];
    return NULL;
}

solariStatus monitorParseTarget(const char *spec, monitorTarget *out)
{
    char work[MONITOR_TARGETID_MAX];
    const char *sep;
    const targetProtoDesc *pd;
    char *c1, *c2, *c3, *rest;
    size_t leftLen, L;
    int n;

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
    pd = protoFromStr(work);
    if (!pd) return ERR_INVALID_ARG;
    out->proto = pd->proto;
    out->appCheck = pd->appCheck;
    rest = c1 + 1;

    if (out->proto == PROBE_ICMP) {
        strncpy(out->host, rest, sizeof out->host - 1);
        out->port = 0;
        snprintf(out->targetId, sizeof out->targetId, "%s:%s", pd->name, out->host);
    } else if (out->appCheck == APP_CHECK_HTTP) {
        if (rest[0] == '[') {                         /* [ipv6]:port[:path|status] */
            c2 = strchr(rest + 1, ']');
            if (!c2 || c2[1] != ':') return ERR_INVALID_ARG;
            *c2 = '\0';
            c2++;
            strncpy(out->host, rest + 1, sizeof out->host - 1);
        } else {
            c2 = strchr(rest, ':');                   /* host:port[:path|status] */
            if (!c2) return ERR_INVALID_ARG;
            *c2 = '\0';
            strncpy(out->host, rest, sizeof out->host - 1);
        }
        c3 = strchr(c2 + 1, ':');
        if (c3) *c3 = '\0';
        out->port = (uint16_t)strtoul(c2 + 1, NULL, 10);
        if (out->port == 0) return ERR_INVALID_ARG;
        n = snprintf(out->appArg, sizeof out->appArg, "%s", (c3 && c3[1]) ? c3 + 1 : "/");
        if (n < 0 || n >= (int)sizeof out->appArg) return ERR_BUFFER_FULL;
        L = (size_t)snprintf(out->targetId, sizeof out->targetId, "%s:%s:%u:",
                             pd->name, out->host, (unsigned)out->port);
        if (L >= sizeof out->targetId) return ERR_BUFFER_FULL;
        strncat(out->targetId, out->appArg, sizeof out->targetId - L - 1);
    } else if (out->appCheck != APP_CHECK_NONE) {
        c2 = strrchr(rest, ':');                      /* host[:port] */
        if (c2) {
            *c2 = '\0';
            out->port = (uint16_t)strtoul(c2 + 1, NULL, 10);
        } else {
            out->port = pd->defaultPort;
        }
        if (out->port == 0) return ERR_INVALID_ARG;
        strncpy(out->host, rest, sizeof out->host - 1);
        snprintf(out->targetId, sizeof out->targetId, "%s:%s:%u",
                 pd->name, out->host, (unsigned)out->port);
    } else {
        c2 = strrchr(rest, ':');                     /* host:port */
        if (!c2) return ERR_INVALID_ARG;
        *c2 = '\0';
        strncpy(out->host, rest, sizeof out->host - 1);
        out->port = (uint16_t)strtoul(c2 + 1, NULL, 10);
        snprintf(out->targetId, sizeof out->targetId, "%s:%s:%u",
                 pd->name, out->host, (unsigned)out->port);
    }
    return SOLARI_OK;
}

solariStatus monitorAddTarget(monitorConfig *cfg, const char *spec)
{
    monitorTarget t;
    solariStatus rc;
    uint8_t i;

    if (!cfg || !spec) return ERR_INVALID_ARG;
    rc = monitorParseTarget(spec, &t);
    if (rc != SOLARI_OK) return rc;

    for (i = 0; i < cfg->targetCount; i++)               /* idempotent by targetId */
        if (!strcmp(cfg->targets[i].targetId, t.targetId)) return SOLARI_OK;

    if (cfg->targetCount >= MONITOR_MAX_TARGETS) return ERR_BUFFER_FULL;
    cfg->targets[cfg->targetCount++] = t;
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
    out->configEpoch = solariConfigEpoch(c);

    out->roundIntervalSec = (uint32_t)solariConfigGetInt(c, "probe", "roundIntervalSec", 30);
    out->probesPerRound   = (uint16_t)solariConfigGetInt(c, "probe", "probesPerRound", 5);
    out->probeTimeoutMs   = (uint32_t)solariConfigGetInt(c, "probe", "probeTimeoutMs", 1000);
    out->replFactor       = (uint8_t) solariConfigGetInt(c, "probe", "replFactorHint", 2);

    strncpy(out->primaryUrl,  solariConfigGetStr(c, "server", "primaryUrl", ""),  sizeof out->primaryUrl - 1);
    strncpy(out->failoverUrl, solariConfigGetStr(c, "server", "failoverUrl", ""), sizeof out->failoverUrl - 1);
    strncpy(out->subUrl,      solariConfigGetStr(c, "server", "subUrl", ""),      sizeof out->subUrl - 1);
    strncpy(out->ctrlStateFile, solariConfigGetStr(c, "control", "stateFile", ""),
            sizeof out->ctrlStateFile - 1);
    out->useTls = strncmp(out->primaryUrl, "tls+", 4) == 0;
    strncpy(out->caFile,   solariConfigGetStr(c, "tls", "caFile", ""),   sizeof out->caFile - 1);
    strncpy(out->certFile, solariConfigGetStr(c, "tls", "certFile", ""), sizeof out->certFile - 1);
    strncpy(out->keyFile,  solariConfigGetStr(c, "tls", "keyFile", ""),  sizeof out->keyFile - 1);
    strncpy(out->spoolDb,  solariConfigGetStr(c, "probe", "spoolDb", ""), sizeof out->spoolDb - 1);

    n = solariConfigCount(c, "probe", "target");
    for (i = 0; i < n && out->targetCount < MONITOR_MAX_TARGETS; i++) {
        const char *t = solariConfigGetStrAt(c, "probe", "target", i, "");
        if (t[0] == '\0') continue;
        monitorAddTarget(out, t);                        /* parse + idempotent append */
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
