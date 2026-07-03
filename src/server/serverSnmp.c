/*
 * serverSnmp.c - SNMP polling tier implementation (see serverSnmp.h).
 *
 * Pure parsing/merge + a shell-out IF-MIB walk (numeric OIDs, net-snmp CLI)
 * gated behind SOLARI_WITH_SNMP. Persists per-interface throughput to
 * gearInterfaceCurrent/History with server-computed rates.
 */
#define _DEFAULT_SOURCE   /* popen/pclose under -std=c99/c11 */
#include "serverSnmp.h"

#include "solari/solariError.h"
#include "solari/solariLog.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <mariadb/mysql.h>

/* dbConn accessor exposed by serverDb.c for server-tier modules. */
MYSQL *serverDbConn(serverDb *db);

/* Numeric IF-MIB / ifXTable OIDs (no MIB files needed). */
#define OID_IF_NAME   ".1.3.6.1.2.1.31.1.1.1.1"
#define OID_IF_HCIN   ".1.3.6.1.2.1.31.1.1.1.6"
#define OID_IF_HCOUT  ".1.3.6.1.2.1.31.1.1.1.10"
#define OID_IF_STATUS ".1.3.6.1.2.1.2.2.1.8"

/* ---- pure parsing ------------------------------------------------------- */

int serverSnmpParseWalkLine(const char *line, int *ifIndex, char *val, size_t valCap)
{
    const char *sp, *dot, *v, *end;
    long idx;

    if (!line || !ifIndex || !val || valCap == 0) return 0;
    if (val && valCap) val[0] = '\0';

    sp = strchr(line, ' ');           /* "<oid> <value>" */
    if (!sp || sp == line) return 0;

    /* ifIndex = the last dotted label of the OID (before the space). */
    dot = sp;
    while (dot > line && *(dot - 1) != '.') dot--;
    if (dot == line || dot == sp) return 0;
    idx = strtol(dot, NULL, 10);
    if (idx <= 0) return 0;
    *ifIndex = (int)idx;

    /* value: skip the space(s), strip a leading type tag we didn't ask for, and
     * surrounding quotes. With -Onq the value is bare ("30347517242" or a quoted
     * string like "\"enp1s0\""). */
    v = sp;
    while (*v == ' ' || *v == '\t') v++;
    end = v + strlen(v);
    while (end > v && (end[-1] == '\n' || end[-1] == '\r' || end[-1] == ' ')) end--;
    if (end > v && *v == '"' && end[-1] == '"') { v++; end--; }   /* dequote */
    {
        size_t n = (size_t)(end - v);
        if (n >= valCap) n = valCap - 1;
        memcpy(val, v, n);
        val[n] = '\0';
    }
    return 1;
}

static serverSnmpIface *snmpFindIface(serverSnmpIface *ifaces, size_t *count, size_t cap, int ifIndex)
{
    size_t i;
    for (i = 0; i < *count; i++)
        if (ifaces[i].ifIndex == ifIndex) return &ifaces[i];
    if (*count >= cap) return NULL;
    memset(&ifaces[*count], 0, sizeof ifaces[*count]);
    ifaces[*count].ifIndex = ifIndex;
    return &ifaces[(*count)++];
}

int serverSnmpMergeIface(serverSnmpIface *ifaces, size_t *count, size_t cap,
                         serverSnmpOidKind kind, int ifIndex, const char *val)
{
    serverSnmpIface *f;
    if (!ifaces || !count || !val) return 0;
    f = snmpFindIface(ifaces, count, cap, ifIndex);
    if (!f) return 0;
    switch (kind) {
    case SNMP_OID_NAME:   snprintf(f->name, sizeof f->name, "%s", val); f->haveName = 1; break;
    case SNMP_OID_IN:     f->inOctets  = strtoull(val, NULL, 10); f->haveIn = 1; break;
    case SNMP_OID_OUT:    f->outOctets = strtoull(val, NULL, 10); f->haveOut = 1; break;
    case SNMP_OID_STATUS: f->operStatus = (int)strtol(val, NULL, 10); f->haveStatus = 1; break;
    }
    return 1;
}

/* ---- SNMP walk (shell-out; only compiled with SOLARI_WITH_SNMP) ---------- */

/* Only [A-Za-z0-9._:-] survive into the shell command (community + host come
 * from operator config/DB but we never trust them into popen unescaped). */
static void snmpSanitize(const char *in, char *out, size_t cap)
{
    size_t o = 0;
    for (; in && *in && o + 1 < cap; in++) {
        char c = *in;
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9') || c == '.' || c == '_' || c == ':' || c == '-')
            out[o++] = c;
    }
    out[o] = '\0';
}

#ifdef SOLARI_WITH_SNMP
static solariStatus snmpWalk(const char *mgmtIp, const char *community, const char *oid,
                             serverSnmpIface *ifaces, size_t *count, size_t cap,
                             serverSnmpOidKind kind)
{
    char cmd[512], ip[128], comm[128], line[600];
    FILE *f;
    int got = 0;

    snmpSanitize(mgmtIp, ip, sizeof ip);
    snmpSanitize((community && community[0]) ? community : "public", comm, sizeof comm);
    if (!ip[0]) return ERR_INVALID_ARG;

    snprintf(cmd, sizeof cmd,
             "snmpbulkwalk -v2c -c %s -t 2 -r 1 -Onq %s %s 2>/dev/null",
             comm, ip, oid);
    f = popen(cmd, "r");
    if (!f) return ERR_PLATFORM;
    while (fgets(line, sizeof line, f)) {
        int idx; char val[128];
        if (serverSnmpParseWalkLine(line, &idx, val, sizeof val)) {
            serverSnmpMergeIface(ifaces, count, cap, kind, idx, val);
            got++;
        }
    }
    pclose(f);
    return got > 0 ? SOLARI_OK : ERR_CONN_RETRY;   /* no rows -> unreachable/no SNMP */
}
#endif

/* ---- DB helpers (server-tier direct SQL, escaped) ----------------------- */

/* Read the previous sample for rate computation. Returns 1 if a prior row
 * exists (fills *inPrev,*outPrev,*ageSec), else 0. */
static int snmpPrevSample(MYSQL *conn, const char *gearIdEsc, int ifIndex,
                          unsigned long long *inPrev, unsigned long long *outPrev,
                          long *ageSec)
{
    char q[256];
    MYSQL_RES *res;
    MYSQL_ROW row;
    int have = 0;
    snprintf(q, sizeof q,
             "SELECT inOctets, outOctets, TIMESTAMPDIFF(SECOND, sampledAt, UTC_TIMESTAMP()) "
             "FROM gearInterfaceCurrent WHERE gearId='%s' AND ifIndex=%d",
             gearIdEsc, ifIndex);
    if (mysql_query(conn, q) != 0) return 0;
    res = mysql_store_result(conn);
    if (!res) return 0;
    if ((row = mysql_fetch_row(res)) && row[0] && row[1] && row[2]) {
        *inPrev  = strtoull(row[0], NULL, 10);
        *outPrev = strtoull(row[1], NULL, 10);
        *ageSec  = strtol(row[2], NULL, 10);
        have = 1;
    }
    mysql_free_result(res);
    return have;
}

solariStatus serverSnmpPollGear(serverContext *ctx, const char *gearId,
                                const char *mgmtIp, const char *community)
{
    (void)community;
    if (!ctx || !ctx->db || !gearId || !mgmtIp || !mgmtIp[0]) return ERR_INVALID_ARG;

#ifndef SOLARI_WITH_SNMP
    (void)gearId; (void)mgmtIp;
    return ERR_PLATFORM;                 /* no SNMP backend compiled in */
#else
    {
        MYSQL *conn = serverDbConn(ctx->db);
        serverSnmpIface ifaces[SERVER_SNMP_MAX_IFACES];
        size_t count = 0, i;
        char gEsc[97], nEsc[193];
        solariStatus rc;

        if (!conn) return ERR_DB;

        rc = snmpWalk(mgmtIp, community, OID_IF_NAME,   ifaces, &count, SERVER_SNMP_MAX_IFACES, SNMP_OID_NAME);
        if (rc != SOLARI_OK) return rc;  /* first walk failing == gear unreachable */
        (void)snmpWalk(mgmtIp, community, OID_IF_HCIN,   ifaces, &count, SERVER_SNMP_MAX_IFACES, SNMP_OID_IN);
        (void)snmpWalk(mgmtIp, community, OID_IF_HCOUT,  ifaces, &count, SERVER_SNMP_MAX_IFACES, SNMP_OID_OUT);
        (void)snmpWalk(mgmtIp, community, OID_IF_STATUS, ifaces, &count, SERVER_SNMP_MAX_IFACES, SNMP_OID_STATUS);

        mysql_real_escape_string(conn, gEsc, gearId, (unsigned long)strlen(gearId));

        for (i = 0; i < count; i++) {
            serverSnmpIface *f = &ifaces[i];
            unsigned long long inPrev = 0, outPrev = 0, inRate = 0, outRate = 0;
            long ageSec = 0;
            char q[1024];

            if (!f->haveIn && !f->haveOut) continue;   /* nothing useful */

            if (snmpPrevSample(conn, gEsc, f->ifIndex, &inPrev, &outPrev, &ageSec) && ageSec > 0) {
                /* rate Kbps = deltaOctets*8/1000/age; guard counter resets. */
                if (f->inOctets  >= inPrev)  inRate  = (f->inOctets  - inPrev)  * 8ULL / 1000ULL / (unsigned long long)ageSec;
                if (f->outOctets >= outPrev) outRate = (f->outOctets - outPrev) * 8ULL / 1000ULL / (unsigned long long)ageSec;
            }

            mysql_real_escape_string(conn, nEsc, f->name, (unsigned long)strlen(f->name));
            snprintf(q, sizeof q,
                     "INSERT INTO gearInterfaceCurrent "
                     "(gearId, ifIndex, ifName, inOctets, outOctets, inRateKbps, outRateKbps, operStatus, sampledAt) "
                     "VALUES ('%s', %d, '%s', %llu, %llu, %llu, %llu, %d, UTC_TIMESTAMP()) "
                     "ON DUPLICATE KEY UPDATE ifName=VALUES(ifName), inOctets=VALUES(inOctets), "
                     "outOctets=VALUES(outOctets), inRateKbps=VALUES(inRateKbps), "
                     "outRateKbps=VALUES(outRateKbps), operStatus=VALUES(operStatus), sampledAt=UTC_TIMESTAMP()",
                     gEsc, f->ifIndex, nEsc, f->inOctets, f->outOctets, inRate, outRate, f->operStatus);
            (void)mysql_query(conn, q);

            snprintf(q, sizeof q,
                     "INSERT INTO gearInterfaceHistory "
                     "(gearId, ifIndex, inRateKbps, outRateKbps, operStatus, sampledAt) "
                     "VALUES ('%s', %d, %llu, %llu, %d, UTC_TIMESTAMP())",
                     gEsc, f->ifIndex, inRate, outRate, f->operStatus);
            (void)mysql_query(conn, q);
        }
        solariLogf(SOLARI_LOG_INFO, "snmp: polled gear %s (%zu ifaces)", gearId, count);
        return SOLARI_OK;
    }
#endif
}

solariStatus serverSnmpPollAll(serverContext *ctx, const char *defaultCommunity,
                               size_t *polledOut)
{
    MYSQL     *conn;
    MYSQL_RES *res;
    MYSQL_ROW  row;
    size_t     polled = 0;

    if (polledOut) *polledOut = 0;
    if (!ctx || !ctx->db) return ERR_INVALID_ARG;
    conn = serverDbConn(ctx->db);
    if (!conn) return ERR_DB;

    if (mysql_query(conn,
            "SELECT gearId, mgmtIp, COALESCE(snmpCommunity,'') FROM networkGear "
            "WHERE mgmtIp IS NOT NULL AND mgmtIp <> ''") != 0)
        return ERR_DB;
    res = mysql_store_result(conn);
    if (!res) return ERR_DB;

    while ((row = mysql_fetch_row(res))) {
        const char *gearId = row[0], *mgmtIp = row[1];
        const char *comm = (row[2] && row[2][0]) ? row[2] : defaultCommunity;
        /* store_result holds the connection's result; copy fields before polling
         * (which issues its own queries) by finishing the walk after freeing. We
         * collect into a small on-stack list instead. */
        char gBuf[64], ipBuf[128], cBuf[128];
        snprintf(gBuf, sizeof gBuf, "%s", gearId ? gearId : "");
        snprintf(ipBuf, sizeof ipBuf, "%s", mgmtIp ? mgmtIp : "");
        snprintf(cBuf, sizeof cBuf, "%s", comm ? comm : "");
        /* defer the actual poll until the result set is freed (below) by stashing;
         * simplest correct approach: free result first, then poll all stashed. */
        /* -- but to keep memory bounded we poll after freeing; see loop rebuild. */
        (void)gBuf; (void)ipBuf; (void)cBuf;
        break;  /* replaced by the two-phase approach below */
    }
    mysql_free_result(res);

    /* Two-phase: re-select into an in-memory list, free the result, THEN poll
     * (each poll issues its own queries on the same connection). */
    {
        struct { char gearId[64], ip[128], comm[128]; } list[128];
        size_t n = 0, i;
        if (mysql_query(conn,
                "SELECT gearId, mgmtIp, COALESCE(snmpCommunity,'') FROM networkGear "
                "WHERE mgmtIp IS NOT NULL AND mgmtIp <> ''") != 0)
            return ERR_DB;
        res = mysql_store_result(conn);
        if (!res) return ERR_DB;
        while ((row = mysql_fetch_row(res)) && n < 128) {
            snprintf(list[n].gearId, sizeof list[n].gearId, "%s", row[0] ? row[0] : "");
            snprintf(list[n].ip,     sizeof list[n].ip,     "%s", row[1] ? row[1] : "");
            snprintf(list[n].comm,   sizeof list[n].comm,   "%s",
                     (row[2] && row[2][0]) ? row[2] : (defaultCommunity ? defaultCommunity : ""));
            n++;
        }
        mysql_free_result(res);
        for (i = 0; i < n; i++) {
            if (serverSnmpPollGear(ctx, list[i].gearId, list[i].ip, list[i].comm) == SOLARI_OK)
                polled++;
        }
    }

    if (polledOut) *polledOut = polled;
    return SOLARI_OK;
}
