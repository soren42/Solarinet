/*
 * main.c - solariServer entry point (§9). STUB skeleton.
 *
 * Thin: load config, open serverDb, init the context, then run the loops -
 * lease tick (failover), ingest accept + dispatch, control-plane service, and
 * the operator bridge poll. All real logic lives in libservercore; this file
 * only wires the subsystems together.
 *
 * Phase 5 skeleton: parses args, builds the context, and exits cleanly. The
 * accept/dispatch loop is sketched but not yet driven.
 */
#include "server.h"

#include "solari/solariLog.h"
#include "solari/solariTime.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void usage(const char *argv0)
{
    fprintf(stderr,
        "usage: %s [--config PATH] [--init-db] [-h]\n"
        "  --config PATH   load the server .conf (sec 13)\n"
        "  --init-db       apply db/schema.sql then exit\n",
        argv0);
}

int main(int argc, char **argv)
{
    const char    *cfgPath = NULL;
    int            initDb = 0, i;
    serverConfig   cfg;
    serverContext  ctx;
    serverDb      *db = NULL;
    solariStatus   rc;

    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--config") && i + 1 < argc) cfgPath = argv[++i];
        else if (!strcmp(argv[i], "--init-db")) initDb = 1;
        else if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")) {
            usage(argv[0]); return 0;
        } else {
            fprintf(stderr, "unknown arg: %s\n", argv[i]);
            usage(argv[0]); return 2;
        }
    }

    solariLogInit(SOLARI_LOG_SINK_STDERR, NULL, "solariServer");

    if (cfgPath) {
        rc = serverConfigFromFile(cfgPath, &cfg);
        if (rc != SOLARI_OK) {
            solariLogf(SOLARI_LOG_WARN, "config load not yet implemented (%s): %s",
                       cfgPath, solariStrError(rc));
            serverConfigDefaults(&cfg);
        }
    } else {
        serverConfigDefaults(&cfg);
        solariLogf(SOLARI_LOG_WARN, "no --config; using defaults");
    }

    rc = serverContextInit(&ctx, &cfg);
    if (rc != SOLARI_OK) {
        solariLogf(SOLARI_LOG_FATAL, "context init failed: %s", solariStrError(rc));
        solariLogShutdown();
        return 1;
    }

    rc = serverDbOpen(&cfg, &db);
    if (rc != SOLARI_OK) {
        solariLogf(SOLARI_LOG_WARN, "db open not yet implemented: %s",
                   solariStrError(rc));
    }
    ctx.db = db;

    if (initDb) {
        rc = serverDbApplyScript(db, "db/schema.sql");
        solariLogf(SOLARI_LOG_INFO, "init-db: %s", solariStrError(rc));
        serverDbClose(db);
        serverContextClose(&ctx);
        solariLogShutdown();
        return rc == SOLARI_OK ? 0 : 1;
    }

    solariLogf(SOLARI_LOG_INFO,
               "solariServer nodeId=0x%llx (skeleton; loops not yet driven)",
               (unsigned long long)ctx.nodeId);

    /* TODO(Phase5): run loops -
     *   serverCtlOpen(&cfg, &ctx, &ctx.ctl);
     *   for (;;) {
     *     serverLeaseTick(&ctx, &ctx.role, &ctx.epoch);
     *     serverApplyRole(&ctx, ctx.role);
     *     ... solariConnRecv -> solariFrameParse -> serverIngestValidate
     *         -> serverIngestDispatch ...
     *     serverCtlPoll(ctx.ctl);
     *   }
     */

    serverDbClose(db);
    serverContextClose(&ctx);
    solariLogShutdown();
    return 0;
}
