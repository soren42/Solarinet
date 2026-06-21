/*
 * monitorControl.c - SCP_MSG_CONTROL handling on the monitor (sec 8, Phase 3
 * Handoff sec 4.2/7.4).
 *
 * The server steers a live monitor without a redeploy by sending an
 * SCP_MSG_CONTROL frame carrying TLV_CTRL_VERB. This increment lands
 * CTRL_ADOPT_TARGET: "one-click monitor this" promotes a discovered entity to a
 * live probe target. The verb's TLV_CTRL_PAYLOAD is the same "proto:host[:port]
 * [ : label]" spec the .conf uses, so adoption converges through the one
 * scheduler add path (monitorAddTarget) - idempotent, no redeploy. The monitor
 * answers with an SCP_MSG_CONTROL_RESULT payload echoing the verb plus an error
 * magnitude (0 = success).
 */
#include "monitor.h"

#include "solari/solariLog.h"
#include "solari/solariTlv.h"

#include <string.h>

/* Build the CONTROL_RESULT payload: echo the verb, report the magnitude (0=ok). */
static solariStatus buildResult(uint8_t verb, solariStatus outcome,
                                uint8_t *buf, size_t cap,
                                size_t *outLen, uint16_t *tlvCount)
{
    solariTlvWriter w;
    uint16_t mag = (uint16_t)(outcome >= 0 ? 0 : -outcome);
    solariStatus rc;

    solariTlvWriterInit(&w, buf, cap);
    rc = solariTlvAppendU8(&w, TLV_CTRL_VERB, verb);
    if (rc == SOLARI_OK) rc = solariTlvAppendU16(&w, TLV_ERROR_CODE, mag);
    if (rc != SOLARI_OK) return rc;
    if (outLen)    *outLen    = w.len;
    if (tlvCount)  *tlvCount  = w.count;
    return SOLARI_OK;
}

solariStatus monitorHandleControl(monitorConfig *cfg,
                                  const uint8_t *payload, size_t payloadLen,
                                  uint8_t *resultBuf, size_t resultCap,
                                  size_t *resultLen, uint16_t *resultTlvCount)
{
    solariControl c;
    solariStatus  rc, outcome;
    char spec[MONITOR_TARGETID_MAX];
    uint16_t n;

    if (!cfg || !payload || !resultBuf) return ERR_INVALID_ARG;
    if (resultLen)      *resultLen      = 0;
    if (resultTlvCount) *resultTlvCount = 0;

    rc = solariMsgParseControl(payload, payloadLen, &c);
    if (rc != SOLARI_OK) return rc;

    if (c.verb != CTRL_ADOPT_TARGET) {              /* other verbs land later */
        solariLogf(SOLARI_LOG_WARN, "control verb %u not handled", (unsigned)c.verb);
        outcome = ERR_UNKNOWN_MSG;
        return buildResult(c.verb, outcome, resultBuf, resultCap,
                           resultLen, resultTlvCount) == SOLARI_OK ? outcome : ERR_BUFFER_FULL;
    }

    /* The adopt payload is the target spec; copy it NUL-terminated (TLV strings
     * carry no terminator) and bound it to a targetId's worth of bytes. */
    if (!c.payload || c.payloadLen == 0) outcome = ERR_INVALID_ARG;
    else {
        n = c.payloadLen < sizeof spec - 1 ? c.payloadLen : (uint16_t)(sizeof spec - 1);
        memcpy(spec, c.payload, n);
        spec[n] = '\0';
        outcome = monitorAddTarget(cfg, spec);
        if (outcome == SOLARI_OK)
            solariLogf(SOLARI_LOG_INFO, "adopted target '%s' (%u live)",
                       spec, (unsigned)cfg->targetCount);
        else
            solariLogf(SOLARI_LOG_ERROR, "adopt target '%s' failed: %s",
                       spec, solariStrError(outcome));
    }

    rc = buildResult(CTRL_ADOPT_TARGET, outcome, resultBuf, resultCap,
                     resultLen, resultTlvCount);
    return rc == SOLARI_OK ? outcome : rc;
}
