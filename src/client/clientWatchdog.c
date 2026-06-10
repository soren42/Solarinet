/*
 * clientWatchdog.c - the watchdog sibling (sec 7.3, 15.2).
 *
 * A separate process (the same binary re-invoked with --watchdog-of PID) that
 * supervises the client: every watchdogIntervalSec it checks the client is
 * alive and, when reporting is built in, pushes a header-only SCP_MSG_WATCHDOG
 * heartbeat so the server sees independent liveness. If the client dies, the
 * watchdog re-execs it (the new client spawns its own watchdog) and exits.
 *
 * Supervision and re-exec use only the PAL (platProcessAlive / platSpawnSelf),
 * so this file builds on every target; only the heartbeat send needs the nng
 * transport and is therefore compiled behind CLIENT_WITH_REPORTING.
 */
#include "client.h"
#include "platOS.h"

#include "solari/solariFrame.h"
#include "solari/solariLog.h"
#include "solari/solariTime.h"

#include <string.h>

#ifdef CLIENT_WITH_REPORTING
#include "solari/solariNet.h"
#endif

bool clientWatchdogSupervisedGone(int64_t supervisedPid)
{
    bool alive = false;
    if (platProcessAlive(supervisedPid, &alive) != SOLARI_OK)
        return true;                    /* unsupervisable -> treat as gone */
    return !alive;
}

solariStatus clientWatchdogBuildHeartbeat(uint64_t nodeId, uint32_t seq,
                                          uint8_t *out, size_t cap, size_t *outLen)
{
    solariFrameHeader h;
    if (!out || !outLen) return ERR_INVALID_ARG;
    memset(&h, 0, sizeof h);
    h.magic[0] = SCP_MAGIC_0;
    h.magic[1] = SCP_MAGIC_1;
    h.protoVersion   = SCP_PROTO_VERSION;
    h.msgType        = SCP_MSG_WATCHDOG;
    h.sourceNodeId   = nodeId;
    h.sendTimeUnixMs = solariNowUnixMs();
    h.seqNo          = seq;
    return solariFrameBuild(&h, NULL, 0, out, cap, outLen);   /* payload-less */
}

#ifdef CLIENT_WITH_REPORTING
/* Open a best-effort PUSH connection for heartbeats (NULL if no server/offline). */
static solariConn *watchdogConnect(const clientConfig *cfg)
{
    solariConnOpts o;
    solariConn *c = NULL;
    const char *url = cfg->primaryUrl[0] ? cfg->primaryUrl :
                      (cfg->failoverUrl[0] ? cfg->failoverUrl : NULL);
    if (!url) return NULL;
    memset(&o, 0, sizeof o);
    o.url = url;
    o.pattern = SOLARI_PATTERN_PUSH;
    o.useTls  = cfg->useTls;
    o.caFile   = cfg->caFile[0]   ? cfg->caFile   : NULL;
    o.certFile = cfg->certFile[0] ? cfg->certFile : NULL;
    o.keyFile  = cfg->keyFile[0]  ? cfg->keyFile  : NULL;
    o.dialTimeoutMs = 3000;
    o.sendTimeoutMs = 1000;
    if (solariConnOpen(&o, &c) != SOLARI_OK) return NULL;
    return c;
}
#endif

int clientWatchdogMain(const clientConfig *cfg, int64_t supervisedPid,
                       const char *argv0, char *const clientArgv[])
{
    uint32_t interval = cfg->watchdogIntervalSec ? cfg->watchdogIntervalSec : 5;
    uint32_t seq = 0;
#ifdef CLIENT_WITH_REPORTING
    solariConn *conn = watchdogConnect(cfg);
    uint64_t nodeId = clientNodeId(cfg);
#endif

    solariLogf(SOLARI_LOG_INFO, "watchdog up: supervising pid %lld every %us",
               (long long)supervisedPid, (unsigned)interval);

    for (;;) {
        solariSleepMs(interval * 1000u);

        if (clientWatchdogSupervisedGone(supervisedPid)) {
            solariLogf(SOLARI_LOG_ERROR,
                       "client pid %lld gone; re-exec %s",
                       (long long)supervisedPid, argv0 ? argv0 : "(null)");
#ifdef CLIENT_WITH_REPORTING
            if (conn) solariConnClose(conn);
#endif
            if (platSpawnSelf(argv0, 0, clientArgv) != SOLARI_OK) {
                solariLogf(SOLARI_LOG_FATAL, "watchdog re-exec failed");
                return 1;
            }
            return 0;   /* the fresh client spawns its own watchdog */
        }

#ifdef CLIENT_WITH_REPORTING
        {
            uint8_t frame[64];
            size_t flen = 0;
            if (!conn) conn = watchdogConnect(cfg);   /* retry if offline */
            if (conn &&
                clientWatchdogBuildHeartbeat(nodeId, ++seq, frame, sizeof frame, &flen) == SOLARI_OK) {
                if (solariConnSend(conn, frame, flen) == ERR_CONN_FATAL) {
                    solariConnClose(conn);
                    conn = NULL;
                }
            }
        }
#endif
    }
}
