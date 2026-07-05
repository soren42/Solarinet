/*
 * main.c - solariMonitor entry point (sec 8).
 *
 * This increment wires probe + HRW ownership into a runnable monitor: load the
 * .conf, take the targets this node owns (HRW; a standalone monitor owns all),
 * probe each per round, and print the reachability/RTT/loss results. The peer
 * mesh + gossip, push-or-spool MONITOR_REPORT, and the control receive path
 * are wired: in reporting mode the inter-round sleep doubles as the
 * control-plane poll (monitorControlPoll) - a SUB connection to the server's
 * fleet PUB endpoint receives CTRL_ADOPT_TARGET / CTRL_SET_CONFIG /
 * CTRL_PROVISION directives, applies + persists them, and answers
 * CONTROL_RESULT over the push channel so convergence clears.
 */
#include "monitor.h"

#include "solari/solariLog.h"
#include "solari/solariTime.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void usage(const char *argv0)
{
    fprintf(stderr,
        "usage: %s [--config PATH] [--interval SEC] [--loop] [--once] [-h]\n"
        "  --config PATH   load a monitor .conf (sec 13); else no targets\n"
        "  --interval SEC  override the round interval\n"
        "  --loop          probe every round interval (Ctrl-C to stop)\n"
        "  --once          one round, then exit (default)\n",
        argv0);
}

static const char *protoStr(uint8_t p)
{
    switch (p) { case PROBE_TCP: return "tcp"; case PROBE_UDP: return "udp";
                 case PROBE_ICMP: return "icmp"; default: return "?"; }
}

static const char *outcomeStr(uint8_t o)
{
    switch (o) {
        case PROBE_OK:          return "ok";
        case PROBE_TIMEOUT:     return "timeout";
        case PROBE_REFUSED:     return "refused";
        case PROBE_UNREACHABLE: return "unreachable";
        case PROBE_DNS_FAIL:    return "dns-fail";
        case PROBE_TLS_FAIL:    return "tls-fail";
        case PROBE_PROTO_ERR:   return "proto-err";
        default:                return "?";
    }
}

/* Probe every owned target over the current live `fleet`, print results, and
 * accumulate them into *out. */
static void runRound(const monitorConfig *cfg, uint64_t self,
                     const uint64_t *fleet, size_t fleetLen, solariMonitorReport *out)
{
    uint8_t i;

    memset(out, 0, sizeof *out);
    if (cfg->hostFqdn[0]) strncpy(out->hostFqdn, cfg->hostFqdn, sizeof out->hostFqdn - 1);
    else                  monitorHostFqdn(out->hostFqdn, sizeof out->hostFqdn);

    for (i = 0; i < cfg->targetCount; i++) {
        const monitorTarget *t = &cfg->targets[i];
        probeSpec spec;
        solariProbeResult res;
        if (!monitorOwnsTarget(self, fleet, fleetLen, t->targetId, cfg->replFactor)) continue;

        memset(&spec, 0, sizeof spec);
        strncpy(spec.targetHost, t->host, sizeof spec.targetHost - 1);
        spec.dstPort   = t->port;
        spec.proto     = t->proto;
        spec.count     = cfg->probesPerRound;
        spec.timeoutMs = cfg->probeTimeoutMs;

        if (probeRun(&spec, &res) != SOLARI_OK) continue;
        printf("  %-26s %-5s %-11s rtt=%.2fms jit=%.2fms loss=%.1f%%  %s\n",
               t->targetId, protoStr(res.proto), outcomeStr(res.outcome),
               res.rttMicros / 1000.0, res.jitterMicros / 1000.0,
               res.lossPermille / 10.0, t->label);
        if (out->probeCount < SOLARI_MAX_PROBES) out->probes[out->probeCount++] = res;
    }
}

int main(int argc, char **argv)
{
    const char *cfgPath = NULL;
    long intervalOverride = -1;
    int loop = 0, i;
    monitorConfig cfg;
    uint64_t self;
    monitorPeers reg;

    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--config") && i + 1 < argc) cfgPath = argv[++i];
        else if (!strcmp(argv[i], "--interval") && i + 1 < argc) intervalOverride = strtol(argv[++i], NULL, 10);
        else if (!strcmp(argv[i], "--loop")) loop = 1;
        else if (!strcmp(argv[i], "--once")) loop = 0;
        else if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")) { usage(argv[0]); return 0; }
        else { fprintf(stderr, "unknown arg: %s\n", argv[i]); usage(argv[0]); return 2; }
    }

    solariLogInit(SOLARI_LOG_SINK_STDERR, NULL, "solariMonitor");

    if (cfgPath) {
        solariStatus rc = monitorConfigFromFile(cfgPath, &cfg);
        if (rc != SOLARI_OK) {
            solariLogf(SOLARI_LOG_FATAL, "config load failed (%s): %s",
                       cfgPath, solariStrError(rc));
            solariLogShutdown();
            return 1;
        }
    } else {
        monitorConfigDefaults(&cfg);
        solariLogf(SOLARI_LOG_WARN, "no --config; no targets to probe");
    }
    if (intervalOverride > 0) cfg.roundIntervalSec = (uint32_t)intervalOverride;

    self = monitorNodeId(&cfg);
    monitorPeersInit(&reg, self);
    solariLogf(SOLARI_LOG_INFO,
               "monitor nodeId=0x%llx, %u targets, replFactor %u, %u/round, %u peer(s)",
               (unsigned long long)self, (unsigned)cfg.targetCount,
               (unsigned)cfg.replFactor, (unsigned)cfg.probesPerRound,
               (unsigned)cfg.peerCount);

#ifdef MONITOR_WITH_REPORTING
    {
        monitorContext      ctx;
        monitorGossip      *gossip = NULL;
        monitorControlState cst;
        monitorControlIo    cio;
        int reporting = (cfg.primaryUrl[0] != '\0');

        memset(&cio, 0, sizeof cio);
        memset(&cst, 0, sizeof cst);
        monitorGossipOpen(&cfg, self, &gossip);      /* NULL if no gossipUrl */
        if (reporting) {
            if (monitorContextInit(&ctx, &cfg) != SOLARI_OK) {
                solariLogf(SOLARI_LOG_FATAL, "reporter init failed");
                monitorGossipClose(gossip);
                solariLogShutdown();
                return 1;
            }
            monitorConnect(&ctx);
            solariLogf(SOLARI_LOG_INFO, "reporting to %s", cfg.primaryUrl);
            /* Restore previously pushed config, then subscribe to the
             * directive channel. Best-effort: the monitor probes regardless. */
            (void)monitorControlStateLoad(&cst, &cfg);
            (void)monitorControlOpen(&cfg, &cio);
        }
        do {
            uint64_t now = solariNowUnixMs();
            uint64_t fleet[MONITOR_MAX_FLEET];
            size_t   fleetLen;
            solariMonitorReport mrep;

            monitorGossipTick(gossip, &reg, now);     /* drain + announce */
            monitorPeersPrune(&reg, now, cfg.peerTtlSec);
            fleetLen = monitorPeersFleet(&reg, now, cfg.peerTtlSec, fleet, MONITOR_MAX_FLEET);

            printf("== probe round @ %llu (fleet=%zu) ==\n",
                   (unsigned long long)now, fleetLen);
            runRound(&cfg, self, fleet, fleetLen, &mrep);
            if (reporting) {
                if (!solariReporterConnected(ctx.rep)) monitorConnect(&ctx);
                monitorReportSend(&ctx, &mrep);
            }
            fflush(stdout);
            if (loop) {
                if (reporting)
                    monitorControlPoll(&cio, &ctx, &cfg, &cst,
                                       cfg.roundIntervalSec * 1000u);
                else
                    solariSleepMs(cfg.roundIntervalSec * 1000u);
            }
        } while (loop);

        monitorControlClose(&cio);
        if (reporting) monitorContextClose(&ctx);
        monitorGossipClose(gossip);
    }
#else
    do {
        uint64_t now = solariNowUnixMs();
        uint64_t fleet[MONITOR_MAX_FLEET];
        size_t   fleetLen;
        solariMonitorReport mrep;

        monitorPeersPrune(&reg, now, cfg.peerTtlSec);    /* no gossip: stays {self} */
        fleetLen = monitorPeersFleet(&reg, now, cfg.peerTtlSec, fleet, MONITOR_MAX_FLEET);

        printf("== probe round @ %llu (fleet=%zu) ==\n", (unsigned long long)now, fleetLen);
        runRound(&cfg, self, fleet, fleetLen, &mrep);
        fflush(stdout);
        if (loop) solariSleepMs(cfg.roundIntervalSec * 1000u);
    } while (loop);
#endif

    solariLogShutdown();
    return 0;
}
