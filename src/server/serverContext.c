/*
 * serverContext.c - server world wiring + config (§9, §13).
 *
 * Holds serverConfig defaults/load, serverNodeId derivation, and the
 * serverContext init/close that thread the db, ctl, and transport handles
 * through every subsystem. Mirrors monitorConfig/monitorContext.
 *
 * Three responsibilities:
 *   1. serverConfigDefaults  - the §13 conservative defaults (dbPort 3306,
 *      pool 4, lease 5/15s, autoDiscover on, autoEnroll off).
 *   2. serverConfigFromFile  - parse a server .conf via solariConfigLoad and
 *      map the [identity]/[bind]/[lease]/[db]/[discovery] keys (also accepts
 *      the [server] alias for the bind/transport block). The DB password is
 *      never read from the file: it comes from $SOLARI_DB_PASS (§13).
 *   3. serverNodeId          - the stable id: configured id wins, else
 *      FNV-1a-64("<fqdn>|<ROLE_SERVER>") per §5.5, matching monitorNodeId /
 *      clientNodeId exactly so a single host derives a deterministic id.
 *
 * serverContextInit only wires cfg into ctx, derives nodeId, and zeroes the
 * handle fields; opening the db (serverDbOpen) and binding the transport
 * (serverApplyRole) happen in main/lease, by contract. serverContextClose
 * tears the transport + ctl down but leaves the db to its owner.
 *
 * The string-mapping logic is factored into small static helpers with no I/O
 * so the config parsing is unit-testable against an in-memory solariConfig
 * (solariConfigParse) without a live MariaDB or any sockets.
 */
#define _GNU_SOURCE   /* gethostname, getaddrinfo under -std=c99 */

#include "server.h"

#include "solari/solariConfig.h"
#include "solari/solariCrypto.h"
#include "solari/solariLog.h"

#include <netdb.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* ------------------------------------------------------------------ helpers */

/* Bounded copy that always NUL-terminates (strncpy leaves no terminator when
 * the source fills the buffer). Used for every cfg string field. */
static void serverStrCpy(char *dst, size_t cap, const char *src)
{
    if (!dst || cap == 0) return;
    if (!src) { dst[0] = '\0'; return; }
    strncpy(dst, src, cap - 1);
    dst[cap - 1] = '\0';
}

/* Best-effort fully-qualified hostname, mirroring monitorHostFqdn: prefer a
 * name that already contains a dot, else canonicalize via getaddrinfo, else
 * fall back to the bare hostname or "unknown". Never fails the caller. */
static void serverHostFqdn(char *out, size_t cap)
{
    char host[256];
    struct addrinfo hints, *res = NULL;

    if (!out || cap == 0) return;
    if (gethostname(host, sizeof host) != 0) {
        serverStrCpy(out, cap, "unknown");
        return;
    }
    host[sizeof host - 1] = '\0';
    if (strchr(host, '.')) {                       /* already qualified */
        serverStrCpy(out, cap, host);
        return;
    }
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_UNSPEC;
    hints.ai_flags  = AI_CANONNAME;
    if (getaddrinfo(host, NULL, &hints, &res) == 0 && res && res->ai_canonname) {
        serverStrCpy(out, cap, res->ai_canonname);
        freeaddrinfo(res);
        return;
    }
    if (res) freeaddrinfo(res);
    serverStrCpy(out, cap, host[0] ? host : "unknown");
}

/* True when a transport URL designates TLS ("tls+tcp://..."), matching the
 * monitor's convention so cfg->useTls tracks the configured scheme. */
static bool serverUrlIsTls(const char *url)
{
    return url && strncmp(url, "tls+", 4) == 0;
}

/* Read a string key from either the canonical section or an alias section,
 * returning the alias only when the canonical key is absent. Lets the same
 * loader honor both the spec's [bind] block and the contract's [server]
 * alias without duplicating every lookup. */
static const char *serverCfgStr(const solariConfig *c,
                                const char *section, const char *alias,
                                const char *key, const char *dflt)
{
    const char *v = solariConfigGetStr(c, section, key, NULL);
    if (v && v[0] != '\0') return v;
    if (alias) {
        v = solariConfigGetStr(c, alias, key, NULL);
        if (v && v[0] != '\0') return v;
    }
    return dflt;
}

/* ----------------------------------------------------------------- defaults */

void serverConfigDefaults(serverConfig *out)
{
    if (!out) return;
    memset(out, 0, sizeof *out);

    /* identity / transport (§13 config/server.conf.sample) */
    serverStrCpy(out->ingestUrl,  sizeof out->ingestUrl,  "tls+tcp://0.0.0.0:7701");
    serverStrCpy(out->surveyUrl,  sizeof out->surveyUrl,  "tls+tcp://0.0.0.0:7702");
    serverStrCpy(out->pubUrl,     sizeof out->pubUrl,     "tls+tcp://0.0.0.0:7703");
    serverStrCpy(out->controlUrl, sizeof out->controlUrl, "tls+tcp://0.0.0.0:7702");
    out->useTls = true;
    serverStrCpy(out->ctlSocket,  sizeof out->ctlSocket,  "/run/solari/solariCtl.sock");
    serverStrCpy(out->caMode,     sizeof out->caMode,      "local");

    /* database (MariaDB / Connector-C) */
    serverStrCpy(out->dbHost, sizeof out->dbHost, "127.0.0.1");
    out->dbPort     = 3306;
    serverStrCpy(out->dbName, sizeof out->dbName, "solarinet");
    serverStrCpy(out->dbUser, sizeof out->dbUser, "solari");
    out->dbPoolSize = 4;

    /* failover / lease (§9.2) */
    out->leaseRenewSec = 5;
    out->leaseTtlSec   = 15;

    /* discovery / provisioning policy (Handoff §7.1) - conservative default */
    out->autoDiscover = true;
    out->autoEnroll   = false;
}

/* -------------------------------------------------------------- file loader */

solariStatus serverConfigFromFile(const char *path, serverConfig *out)
{
    solariConfig *c = NULL;
    solariStatus  rc;
    const char   *s;
    const char   *envPass;

    if (!path || !out) return ERR_INVALID_ARG;

    serverConfigDefaults(out);
    rc = solariConfigLoad(path, &c);
    if (rc != SOLARI_OK) return rc;

    /* identity */
    serverStrCpy(out->hostFqdn, sizeof out->hostFqdn,
                 solariConfigGetStr(c, "identity", "hostFqdn", out->hostFqdn));
    s = solariConfigGetStr(c, "identity", "nodeId", "");
    if (s[0] != '\0') out->nodeId = (uint64_t)strtoull(s, NULL, 0);

    /* transport binds: spec uses [bind]; accept [server] as a contract alias */
    serverStrCpy(out->ingestUrl, sizeof out->ingestUrl,
                 serverCfgStr(c, "bind", "server", "ingestUrl",  out->ingestUrl));
    serverStrCpy(out->surveyUrl, sizeof out->surveyUrl,
                 serverCfgStr(c, "bind", "server", "surveyUrl",  out->surveyUrl));
    serverStrCpy(out->controlUrl, sizeof out->controlUrl,
                 serverCfgStr(c, "bind", "server", "controlUrl", out->surveyUrl));
    serverStrCpy(out->pubUrl, sizeof out->pubUrl,
                 serverCfgStr(c, "bind", "server", "pubUrl",     out->pubUrl));
    serverStrCpy(out->ctlSocket, sizeof out->ctlSocket,
                 serverCfgStr(c, "bind", "server", "ctlSocket",  out->ctlSocket));

    /* useTls is configurable but defaults to "any bind asks for tls" */
    out->useTls = solariConfigGetBool(c, "bind", "useTls",
                      serverUrlIsTls(out->ingestUrl) ||
                      serverUrlIsTls(out->controlUrl) ||
                      serverUrlIsTls(out->pubUrl));

    /* TLS material (kept out of the web tier; loaded by solariCtl) */
    serverStrCpy(out->caFile,   sizeof out->caFile,
                 serverCfgStr(c, "tls", "bind", "caFile",   out->caFile));
    serverStrCpy(out->certFile, sizeof out->certFile,
                 serverCfgStr(c, "tls", "bind", "certFile", out->certFile));
    serverStrCpy(out->keyFile,  sizeof out->keyFile,
                 serverCfgStr(c, "tls", "bind", "keyFile",  out->keyFile));

    /* internal CA (signs node certs; relocatable — see serverConfig). The CA
     * cert defaults to the verify root (caFile); the CA key has no default (must
     * be set explicitly to enable signing). caMode "local" signs here. */
    serverStrCpy(out->caMode, sizeof out->caMode,
                 solariConfigGetStr(c, "ca", "mode", out->caMode[0] ? out->caMode : "local"));
    serverStrCpy(out->caCertFile, sizeof out->caCertFile,
                 solariConfigGetStr(c, "ca", "certFile", out->caFile));
    serverStrCpy(out->caKeyFile, sizeof out->caKeyFile,
                 solariConfigGetStr(c, "ca", "keyFile", out->caKeyFile));
    serverStrCpy(out->caUrl, sizeof out->caUrl,
                 solariConfigGetStr(c, "ca", "url", out->caUrl));

    /* failover / lease */
    out->leaseRenewSec = (uint32_t)solariConfigGetInt(c, "lease", "leaseRenewSec",
                                                      (long)out->leaseRenewSec);
    out->leaseTtlSec   = (uint32_t)solariConfigGetInt(c, "lease", "leaseTtlSec",
                                                      (long)out->leaseTtlSec);

    /* database: password comes from $SOLARI_DB_PASS, never the file (§13) */
    serverStrCpy(out->dbHost, sizeof out->dbHost,
                 solariConfigGetStr(c, "db", "host", out->dbHost));
    out->dbPort = (uint16_t)solariConfigGetInt(c, "db", "port", (long)out->dbPort);
    serverStrCpy(out->dbName, sizeof out->dbName,
                 solariConfigGetStr(c, "db", "name", out->dbName));
    serverStrCpy(out->dbUser, sizeof out->dbUser,
                 solariConfigGetStr(c, "db", "user", out->dbUser));
    serverStrCpy(out->dbSocket, sizeof out->dbSocket,
                 solariConfigGetStr(c, "db", "socket", out->dbSocket));
    out->dbPoolSize = (uint8_t)solariConfigGetInt(c, "db", "poolSize",
                                                  (long)out->dbPoolSize);
    envPass = getenv("SOLARI_DB_PASS");
    if (envPass && envPass[0] != '\0')
        serverStrCpy(out->dbPass, sizeof out->dbPass, envPass);

    /* discovery / provisioning policy */
    out->autoDiscover = solariConfigGetBool(c, "discovery", "autoDiscover",
                                            out->autoDiscover);
    out->autoEnroll   = solariConfigGetBool(c, "discovery", "autoEnroll",
                                            out->autoEnroll);

    solariConfigFree(c);
    return SOLARI_OK;
}

/* ------------------------------------------------------------ node identity */

uint64_t serverNodeId(const serverConfig *cfg)
{
    char fqdn[SOLARI_FQDN_MAX];
    char buf[SOLARI_FQDN_MAX + 8];
    int  n;

    if (cfg && cfg->nodeId) return cfg->nodeId;     /* enrolled id wins */

    if (cfg && cfg->hostFqdn[0]) {
        serverStrCpy(fqdn, sizeof fqdn, cfg->hostFqdn);
    } else {
        serverHostFqdn(fqdn, sizeof fqdn);
    }
    n = snprintf(buf, sizeof buf, "%s|%d", fqdn, (int)ROLE_SERVER);   /* §5.5 */
    return solariFnv1a64(buf, (size_t)(n > 0 ? n : 0));
}

/* ----------------------------------------------------------- context wiring */

solariStatus serverContextInit(serverContext *ctx, const serverConfig *cfg)
{
    if (!ctx || !cfg) return ERR_INVALID_ARG;

    memset(ctx, 0, sizeof *ctx);
    ctx->cfg    = cfg;
    ctx->nodeId = serverNodeId(cfg);
    ctx->role   = SRV_STANDBY;          /* promoted only by serverLeaseTick   */
    ctx->epoch  = 0;
    /* db/ctl/sockets are opened by main + serverApplyRole, not here. */

    solariLogf(SOLARI_LOG_INFO,
               "serverContext init nodeId=0x%llx role=standby",
               (unsigned long long)ctx->nodeId);
    return SOLARI_OK;
}

void serverContextClose(serverContext *ctx)
{
    if (!ctx) return;

    solariConnClose(ctx->ingest);
    solariConnClose(ctx->control);
    solariConnClose(ctx->pub);
    ctx->ingest = ctx->control = ctx->pub = NULL;

    serverCtlClose(ctx->ctl);
    ctx->ctl = NULL;

    ctx->role = SRV_STANDBY;
    /* db is owned by the caller (main); intentionally not closed here. */
}
