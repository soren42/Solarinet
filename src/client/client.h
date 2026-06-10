/*
 * client.h - solariClient internal types (sec 7).
 *
 * The client core is platform-agnostic: it speaks only to the PAL (platOS.h)
 * and libsolari. Configuration is parsed from the .conf (sec 13) into a
 * clientConfig; per-cycle mutable state (log tail offsets) lives separately in
 * clientState so the config stays read-only during collection.
 */
#ifndef SOLARI_CLIENT_H
#define SOLARI_CLIENT_H

#include "solari/solariCommon.h"
#include "solari/solariMsg.h"

/* A watched log file: path + optional POSIX ERE (empty = count every line). */
typedef struct {
    char path[SOLARI_LOGPATH_MAX];
    char regex[128];
} clientLogWatch;

/* Static client configuration, derived from the .conf (sec 13). */
typedef struct {
    char     hostFqdn[SOLARI_FQDN_MAX];   /* override; empty = autodetect */
    uint64_t configEpoch;
    uint32_t sampleIntervalSec;           /* default 15 */
    uint32_t watchdogIntervalSec;         /* default 5  */
    char     procs[SOLARI_MAX_PROCS][SOLARI_PROCNAME_MAX];
    uint8_t  procCount;
    clientLogWatch logs[SOLARI_MAX_LOGS];
    uint8_t  logCount;
} clientConfig;

/* Mutable per-cycle runtime state: where each watched log was last read. */
typedef struct {
    uint64_t logOffset[SOLARI_MAX_LOGS];
} clientState;

/* Fill a clientConfig from a parsed .conf file. Absent keys take sensible
 * defaults; identity.hostFqdn empty means "autodetect at collection time". */
solariStatus clientConfigFromFile(const char *path, clientConfig *out);

/* Fill a clientConfig with autodetect defaults and no watch targets (used when
 * no --config is supplied). Never fails. */
void clientConfigDefaults(clientConfig *out);

/* Gather one report via the PAL. catalogueOnly=true fills only slow-changing
 * identity (for HELLO); false adds fast samples, watched procs, and log deltas.
 * Updates st->logOffset[]. */
solariStatus clientCollectReport(const clientConfig *cfg, clientState *st,
                                 bool catalogueOnly, solariClientReport *out);

#endif /* SOLARI_CLIENT_H */
