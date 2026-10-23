/*
 * Minimal real solariCtl process for discovery HTTP integration tests.
 * It opens the disposable monitoring DB and production serverCtl socket code,
 * but intentionally has no transport/PUB connection to live monitors.
 */
#define _DEFAULT_SOURCE
#include "server.h"
#include "solari/solariLog.h"

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static volatile sig_atomic_t g_stop;

static void stop_on_signal(int signo)
{
    (void)signo;
    g_stop = 1;
}

static int copy_env(char *dst, size_t cap, const char *key, const char *fallback)
{
    const char *value = getenv(key);
    int n = snprintf(dst, cap, "%s", value && *value ? value : fallback);
    return n >= 0 && (size_t)n < cap;
}

int main(int argc, char **argv)
{
    serverConfig cfg;
    serverContext ctx;
    serverDb *db = NULL;
    serverCtl *ctl = NULL;
    const char *portText;
    const char *drainQueue;
    char *end = NULL;
    unsigned long port;
    solariStatus st;

    if (argc != 2 || strlen(argv[1]) >= sizeof cfg.ctlSocket) {
        fprintf(stderr, "usage: %s <unix-socket-path>\n", argv[0]);
        return 2;
    }
    serverConfigDefaults(&cfg);
    if (!copy_env(cfg.dbHost, sizeof cfg.dbHost, "SOLARI_DB_HOST", "127.0.0.1")
        || !copy_env(cfg.dbName, sizeof cfg.dbName, "SOLARI_DB_NAME", "")
        || !copy_env(cfg.dbUser, sizeof cfg.dbUser, "SOLARI_DB_USER", "")
        || !copy_env(cfg.dbPass, sizeof cfg.dbPass, "SOLARI_DB_PASS", "")) {
        fprintf(stderr, "database configuration value is too long\n");
        return 2;
    }
    if ((strcmp(cfg.dbHost, "127.0.0.1") != 0 && strcmp(cfg.dbHost, "localhost") != 0)
        || strncmp(cfg.dbName, "solarinet_scan_worker_",
                   sizeof "solarinet_scan_worker_" - 1) != 0) {
        fprintf(stderr, "refusing a non-local or non-scratch monitoring database\n");
        return 2;
    }
    portText = getenv("SOLARI_DB_PORT");
    if (portText && *portText) {
        port = strtoul(portText, &end, 10);
        if (!end || *end || port == 0 || port > 65535) {
            fprintf(stderr, "invalid SOLARI_DB_PORT\n");
            return 2;
        }
        cfg.dbPort = (uint16_t)port;
    }
    cfg.dbPoolSize = 1;
    snprintf(cfg.ctlSocket, sizeof cfg.ctlSocket, "%s", argv[1]);
    cfg.ctlEnforcePeer = false; /* isolated fixture: PHP connects as ordinary peer */

    st = serverContextInit(&ctx, &cfg);
    if (st != SOLARI_OK) return 3;
    ctx.nodeId = UINT64_C(0x7e57c0de00a1);
    ctx.role = SRV_ACTIVE;
    ctx.epoch = 1;
    st = serverDbOpen(&cfg, &db);
    if (st != SOLARI_OK) {
        serverContextClose(&ctx);
        return 4;
    }
    ctx.db = db;
    st = serverCtlOpen(&cfg, &ctx, &ctl);
    if (st != SOLARI_OK) {
        serverDbClose(db);
        serverContextClose(&ctx);
        return 5;
    }

    signal(SIGTERM, stop_on_signal);
    signal(SIGINT, stop_on_signal);
    puts("READY");
    fflush(stdout);
    while (!g_stop) {
        (void)serverCtlPoll(ctl);
        drainQueue = getenv("SOLARI_TEST_DRAIN_CTL_QUEUE");
        if (drainQueue && strcmp(drainQueue, "1") == 0)
            (void)serverCtlQueuePoll(ctl);
        usleep(5000);
    }
    serverCtlClose(ctl);
    serverDbClose(db);
    serverContextClose(&ctx);
    return 0;
}
