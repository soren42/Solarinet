/*
 * solariMsg.c - typed message builders/parsers over the TLV codec (§6.4).
 */
#include "solari/solariMsg.h"
#include "solari/solariTlv.h"
#include "solari/solariLog.h"
#include "solariEndian.h"

#include <string.h>

/* Copy a length-bounded, non-NUL-terminated source into a fixed dst buffer,
 * NUL-terminating and truncating as needed. */
static void copyBounded(char *dst, size_t dstCap, const uint8_t *src, uint16_t srcLen)
{
    size_t n = srcLen;
    if (n > dstCap - 1) {
        n = dstCap - 1;
    }
    if (n > 0) {
        memcpy(dst, src, n);
    }
    dst[n] = '\0';
}

/* ============================ CLIENT_REPORT ============================ */

solariStatus solariMsgBuildClientReport(const solariClientReport *rep,
                                        uint8_t *payload, size_t cap,
                                        size_t *outLen, uint16_t *tlvCount)
{
    solariTlvWriter w;
    solariStatus st;
    uint8_t tmp[512];
    uint8_t i;

    if (!rep || !payload) {
        return ERR_INVALID_ARG;
    }
    solariTlvWriterInit(&w, payload, cap);

    if ((st = solariTlvAppendStr(&w, TLV_HOST_FQDN, rep->hostFqdn)) != SOLARI_OK) return st;
    if ((st = solariTlvAppendStr(&w, TLV_OS_NAME,   rep->osName))   != SOLARI_OK) return st;
    if ((st = solariTlvAppendStr(&w, TLV_ARCH,      rep->arch))     != SOLARI_OK) return st;

    /* cpu load: one TLV holding coreCount big-endian u32s */
    {
        uint8_t buf[SOLARI_MAX_CORES * 4];
        uint8_t n = rep->coreCount;
        uint8_t k;
        if (n > SOLARI_MAX_CORES) n = SOLARI_MAX_CORES;
        for (k = 0; k < n; k++) {
            solariPutU32(buf + (size_t)k * 4, rep->cpuLoadMilli[k]);
        }
        if ((st = solariTlvAppend(&w, TLV_CPU_LOAD_MILLI, buf, (uint16_t)(n * 4))) != SOLARI_OK) return st;
    }

    if ((st = solariTlvAppendU64(&w, TLV_RAM_USED_KB,   rep->ramUsedKb))   != SOLARI_OK) return st;
    if ((st = solariTlvAppendU64(&w, TLV_RAM_TOTAL_KB,  rep->ramTotalKb))  != SOLARI_OK) return st;
    if ((st = solariTlvAppendU64(&w, TLV_SWAP_USED_KB,  rep->swapUsedKb))  != SOLARI_OK) return st;
    if ((st = solariTlvAppendU64(&w, TLV_SWAP_TOTAL_KB, rep->swapTotalKb)) != SOLARI_OK) return st;

    for (i = 0; i < rep->diskCount && i < SOLARI_MAX_DISKS; i++) {
        const solariDiskEntry *d = &rep->disks[i];
        size_t ml = strlen(d->mount);
        if (16 + ml > sizeof tmp) ml = sizeof tmp - 16;
        solariPutU64(tmp,     d->freeKb);
        solariPutU64(tmp + 8, d->totalKb);
        memcpy(tmp + 16, d->mount, ml);
        if ((st = solariTlvAppend(&w, TLV_DISK_FREE_KB, tmp, (uint16_t)(16 + ml))) != SOLARI_OK) return st;
    }

    for (i = 0; i < rep->ifaceCount && i < SOLARI_MAX_IFACES; i++) {
        const solariIfaceEntry *f = &rep->ifaces[i];
        size_t nl = strlen(f->name);
        if (24 + nl > sizeof tmp) nl = sizeof tmp - 24;
        solariPutU64(tmp,      f->rxKbps);
        solariPutU64(tmp + 8,  f->txKbps);
        solariPutU64(tmp + 16, f->capacityKbps);
        memcpy(tmp + 24, f->name, nl);
        if ((st = solariTlvAppend(&w, TLV_NET_IFACE, tmp, (uint16_t)(24 + nl))) != SOLARI_OK) return st;
    }

    for (i = 0; i < rep->usbCount && i < SOLARI_MAX_USB; i++) {
        const solariUsbEntry *u = &rep->usb[i];
        size_t bl = strlen(u->bus);
        if (16 + bl > sizeof tmp) bl = sizeof tmp - 16;
        solariPutU64(tmp,     u->throughputKbps);
        solariPutU64(tmp + 8, u->capacityKbps);
        memcpy(tmp + 16, u->bus, bl);
        if ((st = solariTlvAppend(&w, TLV_USB_THROUGHPUT, tmp, (uint16_t)(16 + bl))) != SOLARI_OK) return st;
    }

    for (i = 0; i < rep->procCount && i < SOLARI_MAX_PROCS; i++) {
        const solariProcEntry *p = &rep->procs[i];
        size_t nl = strlen(p->name);
        if (21 + nl > sizeof tmp) nl = sizeof tmp - 21;
        solariPutU32(tmp,      (uint32_t)p->pid);
        tmp[4] = p->state;
        solariPutU32(tmp + 5,  p->nFiles);
        solariPutU32(tmp + 9,  p->nSockets);
        solariPutU64(tmp + 13, p->rssKb);
        memcpy(tmp + 21, p->name, nl);
        if ((st = solariTlvAppend(&w, TLV_PROC_WATCH, tmp, (uint16_t)(21 + nl))) != SOLARI_OK) return st;
    }

    for (i = 0; i < rep->logCount && i < SOLARI_MAX_LOGS; i++) {
        const solariLogEntry *l = &rep->logs[i];
        size_t pl = strlen(l->path);
        if (20 + pl > sizeof tmp) pl = sizeof tmp - 20;
        solariPutU64(tmp,      l->sizeDelta);
        solariPutU32(tmp + 8,  l->matchCount);
        solariPutU64(tmp + 12, l->lastOffset);
        memcpy(tmp + 20, l->path, pl);
        if ((st = solariTlvAppend(&w, TLV_LOGFILE_STAT, tmp, (uint16_t)(20 + pl))) != SOLARI_OK) return st;
    }

    if (rep->health.fsReadonlyCount || rep->health.blockDevMissing ||
        rep->health.smartFailCount || rep->health.failedUnitCount ||
        rep->health.dmesgCritCount) {
        uint8_t health[7];
        health[0] = rep->health.fsReadonlyCount;
        health[1] = rep->health.blockDevMissing;
        health[2] = rep->health.smartFailCount;
        solariPutU16(health + 3, rep->health.failedUnitCount);
        solariPutU16(health + 5, rep->health.dmesgCritCount);
        if ((st = solariTlvAppend(&w, TLV_CR_HEALTH_SCALARS,
                                  health, sizeof health)) != SOLARI_OK)
            return st;
    }
    if (rep->health.fsReadonlyList[0]) {
        if ((st = solariTlvAppendStr(&w, TLV_CR_HEALTH_FS_LIST,
                                     rep->health.fsReadonlyList)) != SOLARI_OK)
            return st;
    }
    if (rep->health.smartFailList[0]) {
        if ((st = solariTlvAppendStr(&w, TLV_CR_HEALTH_SMART_LIST,
                                     rep->health.smartFailList)) != SOLARI_OK)
            return st;
    }
    if (rep->health.failedUnitList[0]) {
        if ((st = solariTlvAppendStr(&w, TLV_CR_HEALTH_UNIT_LIST,
                                     rep->health.failedUnitList)) != SOLARI_OK)
            return st;
    }
    if (rep->health.dmesgCritSample[0]) {
        if ((st = solariTlvAppendStr(&w, TLV_CR_HEALTH_DMESG_SAMPLE,
                                     rep->health.dmesgCritSample)) != SOLARI_OK)
            return st;
    }

    if (outLen)   *outLen = w.len;
    if (tlvCount) *tlvCount = w.count;
    return SOLARI_OK;
}

solariStatus solariMsgParseClientReport(const uint8_t *payload, size_t len,
                                        solariClientReport *rep)
{
    solariTlvReader r;
    uint16_t type, vlen;
    const uint8_t *val;
    solariStatus st;

    if (!payload || !rep) {
        return ERR_INVALID_ARG;
    }
    memset(rep, 0, sizeof *rep);
    solariTlvReaderInit(&r, payload, len);

    while ((st = solariTlvNext(&r, &type, &val, &vlen)) == SOLARI_OK) {
        switch (type) {
        case TLV_HOST_FQDN: copyBounded(rep->hostFqdn, sizeof rep->hostFqdn, val, vlen); break;
        case TLV_OS_NAME:   copyBounded(rep->osName,   sizeof rep->osName,   val, vlen); break;
        case TLV_ARCH:      copyBounded(rep->arch,     sizeof rep->arch,     val, vlen); break;
        case TLV_CPU_LOAD_MILLI: {
            uint16_t n = (uint16_t)(vlen / 4);
            uint16_t k;
            if (n > SOLARI_MAX_CORES) n = SOLARI_MAX_CORES;
            for (k = 0; k < n; k++) rep->cpuLoadMilli[k] = solariGetU32(val + (size_t)k * 4);
            rep->coreCount = (uint8_t)n;
            break;
        }
        case TLV_RAM_USED_KB:   solariTlvReadU64(val, vlen, &rep->ramUsedKb);   break;
        case TLV_RAM_TOTAL_KB:  solariTlvReadU64(val, vlen, &rep->ramTotalKb);  break;
        case TLV_SWAP_USED_KB:  solariTlvReadU64(val, vlen, &rep->swapUsedKb);  break;
        case TLV_SWAP_TOTAL_KB: solariTlvReadU64(val, vlen, &rep->swapTotalKb); break;
        case TLV_DISK_FREE_KB:
            if (vlen >= 16 && rep->diskCount < SOLARI_MAX_DISKS) {
                solariDiskEntry *d = &rep->disks[rep->diskCount++];
                d->freeKb  = solariGetU64(val);
                d->totalKb = solariGetU64(val + 8);
                copyBounded(d->mount, sizeof d->mount, val + 16, (uint16_t)(vlen - 16));
            }
            break;
        case TLV_NET_IFACE:
            if (vlen >= 24 && rep->ifaceCount < SOLARI_MAX_IFACES) {
                solariIfaceEntry *f = &rep->ifaces[rep->ifaceCount++];
                f->rxKbps       = solariGetU64(val);
                f->txKbps       = solariGetU64(val + 8);
                f->capacityKbps = solariGetU64(val + 16);
                copyBounded(f->name, sizeof f->name, val + 24, (uint16_t)(vlen - 24));
            }
            break;
        case TLV_USB_THROUGHPUT:
            if (vlen >= 16 && rep->usbCount < SOLARI_MAX_USB) {
                solariUsbEntry *u = &rep->usb[rep->usbCount++];
                u->throughputKbps = solariGetU64(val);
                u->capacityKbps   = solariGetU64(val + 8);
                copyBounded(u->bus, sizeof u->bus, val + 16, (uint16_t)(vlen - 16));
            }
            break;
        case TLV_PROC_WATCH:
            if (vlen >= 21 && rep->procCount < SOLARI_MAX_PROCS) {
                solariProcEntry *p = &rep->procs[rep->procCount++];
                p->pid      = (int32_t)solariGetU32(val);
                p->state    = val[4];
                p->nFiles   = solariGetU32(val + 5);
                p->nSockets = solariGetU32(val + 9);
                p->rssKb    = solariGetU64(val + 13);
                copyBounded(p->name, sizeof p->name, val + 21, (uint16_t)(vlen - 21));
            }
            break;
        case TLV_LOGFILE_STAT:
            if (vlen >= 20 && rep->logCount < SOLARI_MAX_LOGS) {
                solariLogEntry *l = &rep->logs[rep->logCount++];
                l->sizeDelta  = solariGetU64(val);
                l->matchCount = solariGetU32(val + 8);
                l->lastOffset = solariGetU64(val + 12);
                copyBounded(l->path, sizeof l->path, val + 20, (uint16_t)(vlen - 20));
            }
            break;
        case TLV_CR_HEALTH_SCALARS:
            if (vlen == 7) {
                rep->health.fsReadonlyCount = val[0];
                rep->health.blockDevMissing = val[1];
                rep->health.smartFailCount  = val[2];
                rep->health.failedUnitCount = solariGetU16(val + 3);
                rep->health.dmesgCritCount  = solariGetU16(val + 5);
            }
            break;
        case TLV_CR_HEALTH_FS_LIST:
            copyBounded(rep->health.fsReadonlyList,
                        sizeof rep->health.fsReadonlyList, val, vlen);
            break;
        case TLV_CR_HEALTH_SMART_LIST:
            copyBounded(rep->health.smartFailList,
                        sizeof rep->health.smartFailList, val, vlen);
            break;
        case TLV_CR_HEALTH_UNIT_LIST:
            copyBounded(rep->health.failedUnitList,
                        sizeof rep->health.failedUnitList, val, vlen);
            break;
        case TLV_CR_HEALTH_DMESG_SAMPLE:
            copyBounded(rep->health.dmesgCritSample,
                        sizeof rep->health.dmesgCritSample, val, vlen);
            break;
        default:
            /* unknown TLV: skip silently (forward-compat) */
            break;
        }
    }
    return (st == ERR_TLV_END) ? SOLARI_OK : st;
}

/* ============================ MONITOR_REPORT ============================ */

solariStatus solariMsgBuildMonitorReport(const solariMonitorReport *rep,
                                         uint8_t *payload, size_t cap,
                                         size_t *outLen, uint16_t *tlvCount)
{
    solariTlvWriter w;
    solariStatus st;
    uint8_t buf[SOLARI_PROBE_RESULT_WIRE_LEN];
    uint8_t lbuf[SOLARI_LINK_STAT_WIRE_LEN];
    uint16_t i;

    if (!rep || !payload) {
        return ERR_INVALID_ARG;
    }
    solariTlvWriterInit(&w, payload, cap);

    if ((st = solariTlvAppendStr(&w, TLV_HOST_FQDN, rep->hostFqdn)) != SOLARI_OK) return st;

    for (i = 0; i < rep->probeCount && i < SOLARI_MAX_PROBES; i++) {
        const solariProbeResult *p = &rep->probes[i];
        buf[0] = p->proto;
        buf[1] = p->outcome;
        solariPutU16(buf + 2, p->dstPort);
        memcpy(buf + 4, p->dstAddr, 16);
        solariPutU32(buf + 20, p->rttMicros);
        solariPutU32(buf + 24, p->jitterMicros);
        solariPutU16(buf + 28, p->lossPermille);
        solariPutU32(buf + 30, p->throughputKbps);
        solariPutU64(buf + 34, p->probeTimeUnixMs);
        if ((st = solariTlvAppend(&w, TLV_PROBE_RESULT, buf, SOLARI_PROBE_RESULT_WIRE_LEN)) != SOLARI_OK) return st;
    }

    for (i = 0; i < rep->linkCount && i < SOLARI_MAX_IFACES; i++) {
        const solariLinkStat *l = &rep->links[i];
        solariPutU64(lbuf, l->peerNodeId);
        solariPutU32(lbuf + 8, l->transitMicros);
        solariPutU32(lbuf + 12, l->throughputKbps);
        solariPutU32(lbuf + 16, l->capacityKbps);
        solariPutU16(lbuf + 20, l->overheadPermille);
        if ((st = solariTlvAppend(&w, TLV_LINK_STAT, lbuf, SOLARI_LINK_STAT_WIRE_LEN)) != SOLARI_OK) return st;
    }

    if (outLen)   *outLen = w.len;
    if (tlvCount) *tlvCount = w.count;
    return SOLARI_OK;
}

solariStatus solariMsgParseMonitorReport(const uint8_t *payload, size_t len,
                                         solariMonitorReport *rep)
{
    solariTlvReader r;
    uint16_t type, vlen;
    const uint8_t *val;
    solariStatus st;

    if (!payload || !rep) {
        return ERR_INVALID_ARG;
    }
    memset(rep, 0, sizeof *rep);
    solariTlvReaderInit(&r, payload, len);

    while ((st = solariTlvNext(&r, &type, &val, &vlen)) == SOLARI_OK) {
        switch (type) {
        case TLV_HOST_FQDN:
            copyBounded(rep->hostFqdn, sizeof rep->hostFqdn, val, vlen);
            break;
        case TLV_PROBE_RESULT:
            if (vlen == SOLARI_PROBE_RESULT_WIRE_LEN && rep->probeCount < SOLARI_MAX_PROBES) {
                solariProbeResult *p = &rep->probes[rep->probeCount++];
                p->proto           = val[0];
                p->outcome         = val[1];
                p->dstPort         = solariGetU16(val + 2);
                memcpy(p->dstAddr, val + 4, 16);
                p->rttMicros       = solariGetU32(val + 20);
                p->jitterMicros    = solariGetU32(val + 24);
                p->lossPermille    = solariGetU16(val + 28);
                p->throughputKbps  = solariGetU32(val + 30);
                p->probeTimeUnixMs = solariGetU64(val + 34);
            }
            break;
        case TLV_LINK_STAT:
            if (vlen == SOLARI_LINK_STAT_WIRE_LEN && rep->linkCount < SOLARI_MAX_IFACES) {
                solariLinkStat *l = &rep->links[rep->linkCount++];
                l->peerNodeId       = solariGetU64(val);
                l->transitMicros    = solariGetU32(val + 8);
                l->throughputKbps   = solariGetU32(val + 12);
                l->capacityKbps     = solariGetU32(val + 16);
                l->overheadPermille = solariGetU16(val + 20);
            }
            break;
        default:
            break;
        }
    }
    return (st == ERR_TLV_END) ? SOLARI_OK : st;
}

/* ============================ HELLO ============================ */

solariStatus solariMsgBuildHello(const solariHello *h,
                                 uint8_t *payload, size_t cap,
                                 size_t *outLen, uint16_t *tlvCount)
{
    solariTlvWriter w;
    solariStatus st;
    if (!h || !payload) return ERR_INVALID_ARG;
    solariTlvWriterInit(&w, payload, cap);
    if ((st = solariTlvAppendU8 (&w, TLV_ROLE,          (uint8_t)h->role))   != SOLARI_OK) return st;
    if ((st = solariTlvAppendStr(&w, TLV_AGENT_VERSION, h->agentVersion))    != SOLARI_OK) return st;
    if ((st = solariTlvAppendStr(&w, TLV_HOST_FQDN,     h->hostFqdn))        != SOLARI_OK) return st;
    if ((st = solariTlvAppendU64(&w, TLV_CONFIG_EPOCH,  h->configEpoch))     != SOLARI_OK) return st;
    if (outLen)   *outLen = w.len;
    if (tlvCount) *tlvCount = w.count;
    return SOLARI_OK;
}

solariStatus solariMsgParseHello(const uint8_t *payload, size_t len, solariHello *h)
{
    solariTlvReader r;
    uint16_t type, vlen;
    const uint8_t *val;
    solariStatus st;
    if (!payload || !h) return ERR_INVALID_ARG;
    memset(h, 0, sizeof *h);
    solariTlvReaderInit(&r, payload, len);
    while ((st = solariTlvNext(&r, &type, &val, &vlen)) == SOLARI_OK) {
        switch (type) {
        case TLV_ROLE: { uint8_t v; if (solariTlvReadU8(val, vlen, &v) == SOLARI_OK) h->role = (solariRole)v; break; }
        case TLV_AGENT_VERSION: copyBounded(h->agentVersion, sizeof h->agentVersion, val, vlen); break;
        case TLV_HOST_FQDN:     copyBounded(h->hostFqdn, sizeof h->hostFqdn, val, vlen); break;
        case TLV_CONFIG_EPOCH:  solariTlvReadU64(val, vlen, &h->configEpoch); break;
        default: break;
        }
    }
    return (st == ERR_TLV_END) ? SOLARI_OK : st;
}

/* ============================ CONTROL ============================ */

solariStatus solariMsgBuildControl(const solariControl *c,
                                   uint8_t *payload, size_t cap,
                                   size_t *outLen, uint16_t *tlvCount)
{
    solariTlvWriter w;
    solariStatus st;
    if (!c || !payload) return ERR_INVALID_ARG;
    solariTlvWriterInit(&w, payload, cap);
    if ((st = solariTlvAppendU8 (&w, TLV_CTRL_VERB,         c->verb))        != SOLARI_OK) return st;
    if ((st = solariTlvAppendU64(&w, TLV_CTRL_TARGET_EPOCH, c->targetEpoch)) != SOLARI_OK) return st;
    if (c->targetNode != 0) {   /* absent = broadcast; old parsers skip it */
        if ((st = solariTlvAppendU64(&w, TLV_CTRL_TARGET_NODE, c->targetNode)) != SOLARI_OK) return st;
    }
    if (c->payloadLen > 0) {
        if ((st = solariTlvAppend(&w, TLV_CTRL_PAYLOAD, c->payload, c->payloadLen)) != SOLARI_OK) return st;
    }
    if (outLen)   *outLen = w.len;
    if (tlvCount) *tlvCount = w.count;
    return SOLARI_OK;
}

solariStatus solariMsgParseControl(const uint8_t *payload, size_t len, solariControl *c)
{
    solariTlvReader r;
    uint16_t type, vlen;
    const uint8_t *val;
    solariStatus st;
    if (!payload || !c) return ERR_INVALID_ARG;
    memset(c, 0, sizeof *c);
    solariTlvReaderInit(&r, payload, len);
    while ((st = solariTlvNext(&r, &type, &val, &vlen)) == SOLARI_OK) {
        solariStatus fst;
        switch (type) {
        /* The scalar control fields are fail-CLOSED: a present-but-malformed
         * verb/epoch/addressee (wrong length) makes the whole directive
         * untrustworthy. Rejecting it is critical for TLV_CTRL_TARGET_NODE in
         * particular — silently defaulting a bad addressee to 0 would turn a
         * corrupt single-node directive into a fleet-wide broadcast. */
        case TLV_CTRL_VERB:
            if ((fst = solariTlvReadU8(val, vlen, &c->verb)) != SOLARI_OK) return fst;
            break;
        case TLV_CTRL_TARGET_EPOCH:
            if ((fst = solariTlvReadU64(val, vlen, &c->targetEpoch)) != SOLARI_OK) return fst;
            break;
        case TLV_CTRL_TARGET_NODE:
            if ((fst = solariTlvReadU64(val, vlen, &c->targetNode)) != SOLARI_OK) return fst;
            break;
        case TLV_CTRL_PAYLOAD:      c->payload = val; c->payloadLen = vlen; break;
        default: break;   /* unknown TLVs remain tolerated (forward-compat) */
        }
    }
    return (st == ERR_TLV_END) ? SOLARI_OK : st;
}

/* ============================ ERROR ============================ */

solariStatus solariMsgBuildError(const solariErrorMsg *e,
                                 uint8_t *payload, size_t cap,
                                 size_t *outLen, uint16_t *tlvCount)
{
    solariTlvWriter w;
    solariStatus st;
    if (!e || !payload) return ERR_INVALID_ARG;
    solariTlvWriterInit(&w, payload, cap);
    if ((st = solariTlvAppendU16(&w, TLV_ERROR_CODE,   e->code))   != SOLARI_OK) return st;
    if ((st = solariTlvAppendStr(&w, TLV_ERROR_DETAIL, e->detail)) != SOLARI_OK) return st;
    if (outLen)   *outLen = w.len;
    if (tlvCount) *tlvCount = w.count;
    return SOLARI_OK;
}

solariStatus solariMsgParseError(const uint8_t *payload, size_t len, solariErrorMsg *e)
{
    solariTlvReader r;
    uint16_t type, vlen;
    const uint8_t *val;
    solariStatus st;
    if (!payload || !e) return ERR_INVALID_ARG;
    memset(e, 0, sizeof *e);
    solariTlvReaderInit(&r, payload, len);
    while ((st = solariTlvNext(&r, &type, &val, &vlen)) == SOLARI_OK) {
        switch (type) {
        case TLV_ERROR_CODE:   solariTlvReadU16(val, vlen, &e->code); break;
        case TLV_ERROR_DETAIL: copyBounded(e->detail, sizeof e->detail, val, vlen); break;
        default: break;
        }
    }
    return (st == ERR_TLV_END) ? SOLARI_OK : st;
}
