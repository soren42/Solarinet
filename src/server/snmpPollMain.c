/*
 * snmpPollMain.c - solariSnmpPoll: standalone SNMP polling driver.
 *
 * Loads the server config (for DB creds), opens the database, and polls every
 * networkGear's IF-MIB interface counters into gearInterfaceCurrent/History.
 * Kept OUT of the failover-critical server main loop; a systemd timer runs it
 * on a cadence (--once, the default) or it can self-loop (--loop).
 *
 *   solariSnmpPoll --config run/server.conf [--community public]
 *                  [--interval 60] [--once|--loop]
 */
#define _DEFAULT_SOURCE   /* nanosleep */
#include "server.h"
#include "serverSnmp.h"

#include "solari/solariError.h"
#include "solari/solariLog.h"

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static volatile sig_atomic_t g_stop = 0;
static void onSig(int s) { (void)s; g_stop = 1; }

static void usage(const char *a0)
{
    fprintf(stderr,
        "usage: %s --config PATH [--community C] [--interval SEC] [--once|--loop]\n"
        "  polls networkGear IF-MIB counters into gearInterface{Current,History}\n",
        a0);
}

int main(int argc, char **argv)
{
    const char *cfgPath = NULL, *community = "public";
    int loop = 0, interval = 60, i;
    serverConfig cfg;
    serverContext ctx;
    serverDb *db = NULL;
    solariStatus rc;

    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--config") && i + 1 < argc) cfgPath = argv[++i];
        else if (!strcmp(argv[i], "--community") && i + 1 < argc) community = argv[++i];
        else if (!strcmp(argv[i], "--interval") && i + 1 < argc) interval = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--loop")) loop = 1;
        else if (!strcmp(argv[i], "--once")) loop = 0;
        else { usage(argv[0]); return 2; }
    }
    if (!cfgPath) { usage(argv[0]); return 2; }
    if (interval < 5) interval = 5;

    solariLogInit(SOLARI_LOG_SINK_STDERR, NULL, "solariSnmpPoll");
    signal(SIGINT, onSig);
    signal(SIGTERM, onSig);

    if (serverConfigFromFile(cfgPath, &cfg) != SOLARI_OK) {
        solariLogf(SOLARI_LOG_FATAL, "cannot load config %s", cfgPath);
        return 1;
    }
    if (serverContextInit(&ctx, &cfg) != SOLARI_OK) {
        solariLogf(SOLARI_LOG_FATAL, "context init failed");
        return 1;
    }
    if (serverDbOpen(&cfg, &db) != SOLARI_OK) {
        solariLogf(SOLARI_LOG_FATAL, "db open failed");
        return 1;
    }
    ctx.db = db;

    do {
        size_t polled = 0;
        rc = serverSnmpPollAll(&ctx, community, &polled);
        if (rc == SOLARI_OK)
            solariLogf(SOLARI_LOG_INFO, "snmp poll: %zu gear polled", polled);
        else
            solariLogf(SOLARI_LOG_WARN, "snmp poll rc=%d (%s)", (int)rc, solariStrError(rc));
        if (!loop || g_stop) break;
        {
            int slept = 0;
            while (slept < interval && !g_stop) { struct timespec t = {1, 0}; nanosleep(&t, NULL); slept++; }
        }
    } while (!g_stop);

    serverDbClose(db);
    serverContextClose(&ctx);
    return 0;
}
