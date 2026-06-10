/*
 * main.c - solariMonitor entry point (sec 8).
 *
 * This increment wires probe + HRW ownership into a runnable monitor: load the
 * .conf, take the targets this node owns (HRW; a standalone monitor owns all),
 * probe each per round, and print the reachability/RTT/loss results. The peer
 * mesh + gossip, push-or-spool MONITOR_REPORT, survey responder, and
 * CTRL_ADOPT_TARGET land in the next increment.
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

static int runRound(const monitorConfig *cfg, uint64_t self)
{
    uint64_t fleet[1];
    uint8_t i;
    int owned = 0;

    fleet[0] = self;                 /* standalone fleet: this node owns all */
    for (i = 0; i < cfg->targetCount; i++) {
        const monitorTarget *t = &cfg->targets[i];
        probeSpec spec;
        solariProbeResult res;
        if (!monitorOwnsTarget(self, fleet, 1, t->targetId, cfg->replFactor)) continue;
        owned++;

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
    }
    return owned;
}

int main(int argc, char **argv)
{
    const char *cfgPath = NULL;
    long intervalOverride = -1;
    int loop = 0, i;
    monitorConfig cfg;
    uint64_t self;

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
    solariLogf(SOLARI_LOG_INFO, "monitor nodeId=0x%llx, %u targets, replFactor %u, %u/round",
               (unsigned long long)self, (unsigned)cfg.targetCount,
               (unsigned)cfg.replFactor, (unsigned)cfg.probesPerRound);

    do {
        printf("== probe round @ %llu ==\n", (unsigned long long)solariNowUnixMs());
        runRound(&cfg, self);
        fflush(stdout);
        if (loop) solariSleepMs(cfg.roundIntervalSec * 1000u);
    } while (loop);

    solariLogShutdown();
    return 0;
}
