/*
 * serverDb.c - MariaDB persistence layer (§9.1, §10; Handoff §5).
 *
 * Owns the MariaDB Connector/C connection lifecycle, prepared-statement
 * helpers, schema apply, and every data-access function the other subsystems
 * call: node upsert/touch/state, client/monitor report writes (upsert current +
 * append history in one txn), alertEvent/audit append + rule load, nodeConfig
 * target/applied epochs, the serverLease conditional-UPDATE election, discovered
 * upsert/status/known/get, lldpEdge + networkGear writes, enrollment CRUD with
 * atomic approval/certificate persistence and CSR hold/clear, and the
 * buildArtifact x nodeConfig convergence join. All time
 * is stored UTC.
 *
 * Design notes:
 *  - The connection "pool" is an array of MYSQL* handles sized by dbPoolSize.
 *    The server core is single-threaded per subsystem (§9, no globals), so a
 *    checkout simply hands back the first live slot; the array exists so a
 *    future threaded poller can fan out without changing this file's API.
 *  - Every query uses a prepared statement (mysql_stmt_*) with bound params so
 *    user/agent-supplied strings (fqdn, CN, services JSON, CSR PEM) are never
 *    spliced into SQL text. The few schema-DDL paths (serverDbApplyScript) use
 *    mysql_query since DDL cannot be parameterised.
 *  - Pure helpers (string/enum mapping, targetId synthesis, IP formatting) are
 *    factored out as static functions so their logic is testable without a live
 *    MariaDB: they construct values, never touch a connection.
 */
#include "server.h"

#include "solari/solariLog.h"
#include "solari/solariTime.h"
#include "solari/solariCrypto.h"

#include <mariadb/mysql.h>
#include <ctype.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ===================================================================== */
/* Opaque handle                                                          */
/* ===================================================================== */

/* A MariaDB connection pool plus the owning config. conns[0] is the primary
 * slot used by the single-threaded core; the rest are pre-opened spares. */
struct serverDb {
    const serverConfig *cfg;
    MYSQL              *conns[16];    /* pool slots; poolSize entries are live  */
    uint8_t            poolSize;
};

static MYSQL *dbOpenConfiguredConn(const serverConfig *cfg, unsigned long flags)
{
    MYSQL *conn;
    my_bool reconnect = 1;
    unsigned timeout = 5;
    const char *sock;
    if (!cfg) return NULL;
    conn = mysql_init(NULL);
    if (!conn) return NULL;
    sock = cfg->dbSocket[0] ? cfg->dbSocket : NULL;
    if (mysql_options(conn, MYSQL_OPT_RECONNECT, &reconnect) != 0 ||
        mysql_options(conn, MYSQL_OPT_CONNECT_TIMEOUT, &timeout) != 0 ||
        mysql_options(conn, MYSQL_INIT_COMMAND, "SET time_zone='+00:00'") != 0 ||
        !mysql_real_connect(conn, cfg->dbHost, cfg->dbUser, cfg->dbPass,
                            cfg->dbName, cfg->dbPort, sock, flags)) {
        mysql_close(conn);
        return NULL;
    }
    return conn;
}

/* Clamp the configured pool size into [1, 16]. */
static uint8_t dbClampPoolSize(uint8_t requested)
{
    if (requested == 0) return 1;
    if (requested > 16) return 16;
    return requested;
}

/* Hand back the primary slot. If transaction cleanup invalidated it, retry a
 * fresh configured connection on every later DB operation. */
static MYSQL *dbConn(serverDb *db)
{
    if (!db || db->poolSize == 0) return NULL;
    if (!db->conns[0]) {
        db->conns[0] = dbOpenConfiguredConn(db->cfg, CLIENT_MULTI_STATEMENTS);
        if (!db->conns[0]) {
            solariLogf(SOLARI_LOG_WARN,
                       "serverDb: primary connection unavailable; reconnect will retry");
            return NULL;
        }
        solariLogf(SOLARI_LOG_INFO, "serverDb: primary connection recovered");
    }
    return db->conns[0];
}

/* Open a short-lived connection for a multi-statement security transaction.
 * Automatic reconnect is disabled so a lost session cannot silently resume in
 * autocommit mode between statements. */
static MYSQL *dbOpenTxnConn(serverDb *db)
{
    MYSQL *conn;
    my_bool reconnect = 0;
    unsigned timeout = 5;
    const char *sock;
    if (!db || !db->cfg) return NULL;
    conn = mysql_init(NULL);
    if (!conn) return NULL;
    sock = db->cfg->dbSocket[0] ? db->cfg->dbSocket : NULL;
    if (mysql_options(conn, MYSQL_OPT_RECONNECT, &reconnect) != 0 ||
        mysql_options(conn, MYSQL_OPT_CONNECT_TIMEOUT, &timeout) != 0 ||
        mysql_options(conn, MYSQL_INIT_COMMAND, "SET time_zone='+00:00'") != 0 ||
        !mysql_real_connect(conn, db->cfg->dbHost, db->cfg->dbUser,
                            db->cfg->dbPass, db->cfg->dbName,
                            db->cfg->dbPort, sock, 0)) {
        mysql_close(conn);
        return NULL;
    }
    return conn;
}

/* Guard a pooled connection for a multi-statement transaction. Reconnect is
 * allowed between operations, but never while a transaction is in flight. */
static solariStatus dbTxnBegin(MYSQL *conn)
{
    my_bool reconnect = 0;
    if (!conn || mysql_ping(conn) != 0) return ERR_DB;
    if (mysql_options(conn, MYSQL_OPT_RECONNECT, &reconnect) != 0) return ERR_DB;
    if (mysql_autocommit(conn, 0) == 0) return SOLARI_OK;
    reconnect = 1;
    (void)mysql_options(conn, MYSQL_OPT_RECONNECT, &reconnect);
    return ERR_DB;
}

static solariStatus dbTxnFinish(serverDb *db, MYSQL *conn, bool commit,
                                solariStatus outcome)
{
    my_bool reconnect = 1;
    int txnFailed = commit ? mysql_commit(conn) : mysql_rollback(conn);
    int cleanupFailed = 0;
    unsigned int txnError = txnFailed ? mysql_errno(conn) : 0;
    char txnErrorText[256] = "";
    if (txnFailed)
        snprintf(txnErrorText, sizeof txnErrorText, "%s", mysql_error(conn));
    if (txnFailed && commit) (void)mysql_rollback(conn);
    if (txnFailed && !commit) {
        solariLogf(SOLARI_LOG_ERROR,
                   "serverDb: rollback failed (%u): %s; replacing pooled connection",
                   txnError, txnErrorText);
    } else if (txnFailed) {
        solariLogf(SOLARI_LOG_ERROR,
                   "serverDb: commit failed (%u): %s",
                   txnError, txnErrorText);
    }
    if (!txnFailed && mysql_autocommit(conn, 1) != 0) cleanupFailed = 1;
    if (!txnFailed && mysql_options(conn, MYSQL_OPT_RECONNECT, &reconnect) != 0)
        cleanupFailed = 1;
    if (txnFailed || cleanupFailed) {
        if (cleanupFailed)
            solariLogf(SOLARI_LOG_ERROR,
                       "serverDb: transaction session reset failed (%u): %s; replacing pooled connection",
                       mysql_errno(conn), mysql_error(conn));
        if (db && db->poolSize > 0 && db->conns[0] == conn) {
            mysql_close(conn); /* closing an open session rolls back its transaction */
            db->conns[0] = dbOpenConfiguredConn(db->cfg, CLIENT_MULTI_STATEMENTS);
            if (!db->conns[0])
                solariLogf(SOLARI_LOG_ERROR,
                           "serverDb: failed to replace pooled transaction connection");
        }
    }
    /* A confirmed commit stays successful even if only session cleanup failed;
     * replaying it could duplicate the caller's operation. */
    if (txnFailed || (cleanupFailed && !commit)) return ERR_DB;
    return outcome;
}

/* Public accessor for server-tier modules that issue their own SQL (serverSnmp).
 * Same single-connection contract as dbConn. */
MYSQL *serverDbConn(serverDb *db) { return dbConn(db); }

/* ===================================================================== */
/* Pure value helpers (no connection touched - unit-testable)             */
/* ===================================================================== */

/* Map a solariRole to its node.role / enrollment.role ENUM string. */
static const char *dbRoleStr(solariRole role)
{
    switch (role) {
        case ROLE_CLIENT:  return "client";
        case ROLE_MONITOR: return "monitor";
        case ROLE_SERVER:  return "server";
        default:           return "client";
    }
}

static bool dbHexStringValid(const char *s, size_t exactLen, size_t maxLen)
{
    size_t i, n;
    if (!s) return false;
    n = strlen(s);
    if ((exactLen && n != exactLen) || n == 0 || n > maxLen) return false;
    for (i = 0; i < n; ++i)
        if (!isxdigit((unsigned char)s[i])) return false;
    return true;
}

/* Map a wire probe-outcome byte (probeOutcome 0..6, §5.7) to the probeCurrent /
 * probeHistory outcome ENUM string. Unknown values fall back to "proto_err". */
static const char *dbOutcomeStr(uint8_t outcome)
{
    switch (outcome) {
        case 0: return "ok";
        case 1: return "timeout";
        case 2: return "refused";
        case 3: return "unreachable";
        case 4: return "dns_fail";
        case 5: return "tls_fail";
        case 6: return "proto_err";
        default: return "proto_err";
    }
}

/* Map a wire proto byte (1=ICMP,2=TCP,3=UDP) to its probeTarget proto string. */
static const char *dbProtoStr(uint8_t proto)
{
    switch (proto) {
        case 1: return "icmp";
        case 2: return "tcp";
        case 3: return "udp";
        default: return "tcp";
    }
}

/* Map a serverEnrollStatus to the enrollment.status ENUM string. */
static const char *dbEnrollStatusStr(serverEnrollStatus status)
{
    switch (status) {
        case ENROLL_TOKEN:    return "token";
        case ENROLL_PENDING:  return "pending";
        case ENROLL_APPROVED: return "approved";
        case ENROLL_REJECTED: return "rejected";
        case ENROLL_EXPIRED:  return "expired";
        default:              return "token";
    }
}

/* Parse an enrollment.status ENUM string back into the enum. */
static serverEnrollStatus dbEnrollStatusParse(const char *s)
{
    if (!s)                          return ENROLL_TOKEN;
    if (strcmp(s, "pending")  == 0)  return ENROLL_PENDING;
    if (strcmp(s, "approved") == 0)  return ENROLL_APPROVED;
    if (strcmp(s, "rejected") == 0)  return ENROLL_REJECTED;
    if (strcmp(s, "expired")  == 0)  return ENROLL_EXPIRED;
    return ENROLL_TOKEN;
}

/* Parse a node/enrollment role ENUM string back into a solariRole. */
static solariRole dbRoleParse(const char *s)
{
    if (s && strcmp(s, "monitor") == 0) return ROLE_MONITOR;
    if (s && strcmp(s, "server")  == 0) return ROLE_SERVER;
    return ROLE_CLIENT;
}

/* Format a 16-byte SCP probe address (IPv4-mapped or IPv6) into a textual host
 * for targetId synthesis. IPv4-mapped (::ffff:a.b.c.d) prints dotted-quad; a
 * pure-zero address prints "0.0.0.0". Returns bytes written (excluding NUL). */
static int dbFormatProbeAddr(const uint8_t addr[16], char *out, size_t cap)
{
    static const uint8_t v4prefix[12] = {
        0,0,0,0, 0,0,0,0, 0,0,0xff,0xff
    };
    int i;
    int allZero = 1;

    if (!out || cap == 0) return 0;
    for (i = 0; i < 16; i++) { if (addr[i]) { allZero = 0; break; } }
    if (allZero)
        return snprintf(out, cap, "0.0.0.0");

    if (memcmp(addr, v4prefix, sizeof v4prefix) == 0)
        return snprintf(out, cap, "%u.%u.%u.%u",
                        addr[12], addr[13], addr[14], addr[15]);

    return snprintf(out, cap,
        "%02x%02x:%02x%02x:%02x%02x:%02x%02x:"
        "%02x%02x:%02x%02x:%02x%02x:%02x%02x",
        addr[0],addr[1],addr[2],addr[3], addr[4],addr[5],addr[6],addr[7],
        addr[8],addr[9],addr[10],addr[11], addr[12],addr[13],addr[14],addr[15]);
}

/* Synthesise the stable probeTarget id for a probe result the same way the
 * monitor config does: "<proto>:<host>:<port>" (ICMP omits the port, which it
 * always reports as 0). Writes into out; returns bytes written (excluding NUL).
 * Pure: depends only on the result fields, so it is unit-testable. */
static int dbProbeTargetId(const solariProbeResult *p, char *out, size_t cap)
{
    char host[SERVER_IP_MAX];

    if (!p || !out || cap == 0) { if (out && cap) out[0] = '\0'; return 0; }
    dbFormatProbeAddr(p->dstAddr, host, sizeof host);
    if (p->proto == 1 /* ICMP */)
        return snprintf(out, cap, "icmp:%s", host);
    return snprintf(out, cap, "%s:%s:%u", dbProtoStr(p->proto), host, p->dstPort);
}

/* Retained for the deferred F4 SCP effective-tier wire work.  Lifecycle
 * cascades use alertEvent.assetId/nodeId/targetId directly instead. */
static bool __attribute__((unused)) dbNumericAlertIdToProbeId(const char *in, char *out, size_t cap)
{
    const char *first, *last, *proto; unsigned long p; char *end = NULL;
    if (!in || !out || cap == 0 || !(first = strchr(in, ':'))) return false;
    p = strtoul(in, &end, 10); if (end != first || p < 1 || p > 3) return false;
    proto = dbProtoStr((uint8_t)p); last = strrchr(first + 1, ':');
    if (p == 1) { if (!first[1]) return false; return snprintf(out,cap,"%s:%s",proto,first+1) < (int)cap; }
    if (!last || !last[1] || last == first + 1) return false;
    return snprintf(out,cap,"%s:%.*s:%s",proto,(int)(last-first-1),first+1,last+1) < (int)cap;
}

static bool __attribute__((unused)) dbProbeIdToNumericAlertId(const char *in, char *out, size_t cap)
{
    const char *first, *last; int p;
    if (!in || !out || cap == 0 || !(first=strchr(in,':'))) return false;
    if ((size_t)(first-in)==4 && strncmp(in,"icmp",4)==0) p=1;
    else if ((size_t)(first-in)==3 && strncmp(in,"tcp",3)==0) p=2;
    else if ((size_t)(first-in)==3 && strncmp(in,"udp",3)==0) p=3;
    else return false;
    last=strrchr(first+1,':');
    if (p==1) { if (!first[1]) return false; return snprintf(out,cap,"1:%s",first+1)<(int)cap; }
    if (!last || !last[1] || last==first+1) return false;
    return snprintf(out,cap,"%d:%.*s:%s",p,(int)(last-first-1),first+1,last+1)<(int)cap;
}

/* ===================================================================== */
/* Prepared-statement execution helpers                                   */
/* ===================================================================== */

/* Initialise one MYSQL_BIND slot for an unsigned 64-bit input parameter. */
static void dbBindU64(MYSQL_BIND *b, unsigned long long *val)
{
    memset(b, 0, sizeof *b);
    b->buffer_type = MYSQL_TYPE_LONGLONG;
    b->buffer      = val;
    b->is_unsigned = 1;
}

/* Initialise one MYSQL_BIND slot for a 32-bit signed input parameter. */
static void dbBindI32(MYSQL_BIND *b, int *val)
{
    memset(b, 0, sizeof *b);
    b->buffer_type = MYSQL_TYPE_LONG;
    b->buffer      = val;
}

/* Initialise one MYSQL_BIND slot for a double input parameter. */
static void dbBindDouble(MYSQL_BIND *b, double *val)
{
    memset(b, 0, sizeof *b);
    b->buffer_type = MYSQL_TYPE_DOUBLE;
    b->buffer      = val;
}

/* Initialise one MYSQL_BIND slot for a NUL-terminated string input parameter.
 * len must point at storage holding strlen(str); the pointer must outlive the
 * execute call. A NULL str binds SQL NULL. */
static void dbBindStr(MYSQL_BIND *b, const char *str, unsigned long *len)
{
    memset(b, 0, sizeof *b);
    if (!str) {
        b->buffer_type = MYSQL_TYPE_NULL;
        return;
    }
    *len = (unsigned long)strlen(str);
    b->buffer_type   = MYSQL_TYPE_STRING;
    b->buffer        = (void *)(size_t)str;   /* MariaDB does not write inputs */
    b->buffer_length = *len;
    b->length        = len;
}

/* Initialise an output MYSQL_BIND for an unsigned 64-bit result column. */
static void dbBindOutU64(MYSQL_BIND *b, unsigned long long *val, my_bool *isNull)
{
    memset(b, 0, sizeof *b);
    b->buffer_type = MYSQL_TYPE_LONGLONG;
    b->buffer      = val;
    b->is_unsigned = 1;
    b->is_null     = isNull;
}

static void dbBindOutI32(MYSQL_BIND *b, int *val, my_bool *isNull)
{
    memset(b, 0, sizeof *b);
    b->buffer_type = MYSQL_TYPE_LONG;
    b->buffer = val;
    b->is_null = isNull;
}

/* Initialise an output MYSQL_BIND for a fixed string result column. cap is the
 * destination capacity; len receives the fetched byte length. */
static void dbBindOutStr(MYSQL_BIND *b, char *buf, unsigned long cap,
                         unsigned long *len, my_bool *isNull)
{
    memset(b, 0, sizeof *b);
    b->buffer_type   = MYSQL_TYPE_STRING;
    b->buffer        = buf;
    b->buffer_length = cap;
    b->length        = len;
    b->is_null       = isNull;
}

/* Prepare a statement on conn, log + return ERR_DB on failure. *out gets the
 * statement on success; the caller always mysql_stmt_close()es it. */
static solariStatus dbStmtPrepare(MYSQL *conn, const char *sql, MYSQL_STMT **out)
{
    MYSQL_STMT *st;

    *out = NULL;
    st = mysql_stmt_init(conn);
    if (!st) {
        solariLogf(SOLARI_LOG_ERROR, "serverDb: stmt_init failed: %s",
                   mysql_error(conn));
        return ERR_DB;
    }
    if (mysql_stmt_prepare(st, sql, (unsigned long)strlen(sql)) != 0) {
        solariLogf(SOLARI_LOG_ERROR, "serverDb: prepare failed: %s",
                   mysql_stmt_error(st));
        mysql_stmt_close(st);
        return ERR_DB;
    }
    *out = st;
    return SOLARI_OK;
}

/* Bind params, execute, and close a write statement. Returns ERR_DB on any
 * failure. binds may be NULL when nParams == 0. */
static solariStatus dbStmtRunWrite(MYSQL_STMT *st, MYSQL_BIND *binds,
                                   unsigned int nParams)
{
    if (nParams > 0 && mysql_stmt_bind_param(st, binds) != 0) {
        solariLogf(SOLARI_LOG_ERROR, "serverDb: bind_param failed: %s",
                   mysql_stmt_error(st));
        return ERR_DB;
    }
    if (mysql_stmt_execute(st) != 0) {
        solariLogf(SOLARI_LOG_ERROR, "serverDb: execute failed: %s",
                   mysql_stmt_error(st));
        return ERR_DB;
    }
    return SOLARI_OK;
}

/* ===================================================================== */
/* Connection lifecycle                                                   */
/* ===================================================================== */

solariStatus serverDbOpen(const serverConfig *cfg, serverDb **out)
{
    serverDb *db;
    uint8_t   i;

    if (!cfg || !out) return ERR_INVALID_ARG;
    *out = NULL;

    db = (serverDb *)calloc(1, sizeof *db);
    if (!db) return ERR_PLATFORM;
    db->cfg      = cfg;
    db->poolSize = dbClampPoolSize(cfg->dbPoolSize);

    for (i = 0; i < db->poolSize; i++) {
        MYSQL *conn = dbOpenConfiguredConn(cfg, CLIENT_MULTI_STATEMENTS);
        if (!conn) {
            solariLogf(SOLARI_LOG_ERROR,
                       "serverDb: could not connect/configure slot %u at %s:%u/%s",
                       i,
                       cfg->dbHost[0] ? cfg->dbHost : "(socket)",
                       (unsigned)cfg->dbPort, cfg->dbName);
            serverDbClose(db);
            return ERR_DB;
        }
        db->conns[i] = conn;
    }

    solariLogf(SOLARI_LOG_INFO, "serverDb: opened %u connection(s) to %s/%s",
               db->poolSize, cfg->dbHost[0] ? cfg->dbHost : "(socket)",
               cfg->dbName);
    *out = db;
    return SOLARI_OK;
}

void serverDbClose(serverDb *db)
{
    uint8_t i;

    if (!db) return;
    for (i = 0; i < 16; i++) {
        if (db->conns[i]) {
            mysql_close(db->conns[i]);
            db->conns[i] = NULL;
        }
    }
    free(db);
}

solariStatus serverDbPing(serverDb *db)
{
    MYSQL *conn = dbConn(db);
    if (!conn) return ERR_DB;
    if (mysql_ping(conn) != 0) {
        solariLogf(SOLARI_LOG_WARN, "serverDb: ping failed: %s",
                   mysql_error(conn));
        return ERR_DB;
    }
    return SOLARI_OK;
}

solariStatus serverDbApplyScript(serverDb *db, const char *sqlPath)
{
    MYSQL   *conn = dbConn(db);
    FILE    *f;
    char    *buf;
    long     size;
    size_t   nread;
    int      rc;

    if (!conn || !sqlPath) return ERR_INVALID_ARG;

    f = fopen(sqlPath, "rb");
    if (!f) {
        solariLogf(SOLARI_LOG_ERROR, "serverDb: cannot open script %s", sqlPath);
        return ERR_DB;
    }
    if (fseek(f, 0, SEEK_END) != 0 || (size = ftell(f)) < 0 ||
        fseek(f, 0, SEEK_SET) != 0) {
        fclose(f);
        return ERR_DB;
    }
    buf = (char *)malloc((size_t)size + 1);
    if (!buf) { fclose(f); return ERR_PLATFORM; }
    nread = fread(buf, 1, (size_t)size, f);
    fclose(f);
    buf[nread] = '\0';

    /* CLIENT_MULTI_STATEMENTS lets a whole script run in one call; drain any
     * extra result sets so the connection is left usable. */
    rc = mysql_query(conn, buf);
    free(buf);
    if (rc != 0) {
        solariLogf(SOLARI_LOG_ERROR, "serverDb: script %s failed: %s",
                   sqlPath, mysql_error(conn));
        return ERR_DB;
    }
    do {
        MYSQL_RES *res = mysql_store_result(conn);
        if (res) mysql_free_result(res);
    } while (mysql_next_result(conn) == 0);

    solariLogf(SOLARI_LOG_INFO, "serverDb: applied script %s", sqlPath);
    return SOLARI_OK;
}

/* ===================================================================== */
/* node identity / lifecycle (§10 node)                                   */
/* ===================================================================== */

solariStatus serverDbUpsertNode(serverDb *db, uint64_t nodeId, solariRole role,
                                const char *hostFqdn, const char *certCn,
                                const char *osName, const char *arch,
                                uint64_t configEpoch)
{
    static const char *SQL =
        "INSERT INTO node "
        "  (nodeId, role, hostFqdn, certCn, osName, arch, enrolledAt, "
        "   lastSeenAt, configEpoch) "
        "VALUES (?, ?, ?, ?, ?, ?, UTC_TIMESTAMP(), UTC_TIMESTAMP(), ?) "
        "ON DUPLICATE KEY UPDATE "
        "  role=VALUES(role), hostFqdn=VALUES(hostFqdn), certCn=VALUES(certCn), "
        "  osName=VALUES(osName), arch=VALUES(arch), "
        "  state=IF(state='retired','unknown',state), "
        "  lastSeenAt=UTC_TIMESTAMP(), configEpoch=VALUES(configEpoch)";
    MYSQL      *conn = dbConn(db);
    MYSQL_STMT *st;
    MYSQL_BIND  b[7];
    unsigned long long vNode = nodeId, vEpoch = configEpoch;
    unsigned long len[5];
    const char *roleStr = dbRoleStr(role);
    solariStatus rc;

    if (!conn || !hostFqdn || !certCn) return ERR_INVALID_ARG;
    rc = dbStmtPrepare(conn, SQL, &st);
    if (rc != SOLARI_OK) return rc;

    dbBindU64(&b[0], &vNode);
    dbBindStr(&b[1], roleStr, &len[0]);
    dbBindStr(&b[2], hostFqdn, &len[1]);
    dbBindStr(&b[3], certCn, &len[2]);
    dbBindStr(&b[4], (osName && osName[0]) ? osName : NULL, &len[3]);
    dbBindStr(&b[5], (arch && arch[0]) ? arch : NULL, &len[4]);
    dbBindU64(&b[6], &vEpoch);

    rc = dbStmtRunWrite(st, b, 7);
    mysql_stmt_close(st);
    return rc;
}

solariStatus serverDbTouchNode(serverDb *db, uint64_t nodeId, uint64_t whenUnixMs)
{
    /* whenUnixMs is the wall-clock ms when the heartbeat was observed; we store
     * it as a UTC DATETIME derived from the epoch ms (FROM_UNIXTIME wants
     * seconds, so divide). */
    static const char *SQL =
        "UPDATE node SET lastSeenAt = FROM_UNIXTIME(? / 1000) WHERE nodeId = ?";
    MYSQL      *conn = dbConn(db);
    MYSQL_STMT *st;
    MYSQL_BIND  b[2];
    unsigned long long vWhen = whenUnixMs ? whenUnixMs : solariNowUnixMs();
    unsigned long long vNode = nodeId;
    solariStatus rc;

    if (!conn) return ERR_INVALID_ARG;
    rc = dbStmtPrepare(conn, SQL, &st);
    if (rc != SOLARI_OK) return rc;
    dbBindU64(&b[0], &vWhen);
    dbBindU64(&b[1], &vNode);
    rc = dbStmtRunWrite(st, b, 2);
    mysql_stmt_close(st);
    return rc;
}

solariStatus serverDbSetNodeState(serverDb *db, uint64_t nodeId, const char *state)
{
    static const char *SQL = "UPDATE node SET state = ? WHERE nodeId = ?";
    MYSQL      *conn = dbConn(db);
    MYSQL_STMT *st;
    MYSQL_BIND  b[2];
    unsigned long long vNode = nodeId;
    unsigned long sLen;
    solariStatus rc;

    if (!conn || !state) return ERR_INVALID_ARG;
    rc = dbStmtPrepare(conn, SQL, &st);
    if (rc != SOLARI_OK) return rc;
    dbBindStr(&b[0], state, &sLen);
    dbBindU64(&b[1], &vNode);
    rc = dbStmtRunWrite(st, b, 2);
    mysql_stmt_close(st);
    return rc;
}

solariStatus serverDbDecommissionIssue(serverDb *db, uint64_t nodeId,
                                       uint32_t wipeScope, const char *requestedBy,
                                       const char tokenHash[65],
                                       uint64_t issuedAtUnixMs,
                                       uint64_t expiresAtUnixMs,
                                       uint64_t *generationOut)
{
    static const char *SQL =
        "INSERT INTO decommissionChallenge "
        "(nodeId,wipeScope,requestedBy,tokenHash,state,issuedAt,expiresAt,generation) "
        "VALUES (?,?,?,?, 'pending',FROM_UNIXTIME(? / 1000),FROM_UNIXTIME(? / 1000),LAST_INSERT_ID(1)) "
        "ON DUPLICATE KEY UPDATE wipeScope=VALUES(wipeScope), "
        "requestedBy=VALUES(requestedBy), tokenHash=VALUES(tokenHash), "
        "state='pending', issuedAt=VALUES(issuedAt), expiresAt=VALUES(expiresAt), "
        "consumedAt=NULL, deliveryAttempts=0, lastDeliveryAt=NULL, nextDeliveryAt=NULL, "
        "generation=LAST_INSERT_ID(generation+1)";
    MYSQL *conn = dbConn(db);
    MYSQL_STMT *st;
    MYSQL_BIND b[6];
    unsigned long long vNode=nodeId, vScope=wipeScope;
    unsigned long long vIssued=issuedAtUnixMs, vExpires=expiresAtUnixMs;
    unsigned long lOperator=0, lHash=0;
    unsigned long long generation;
    solariStatus rc;

    if (generationOut) *generationOut = 0;
    if (!conn || nodeId == 0 || wipeScope == 0 || !requestedBy || !requestedBy[0] ||
        !dbHexStringValid(tokenHash, 64, 64) || expiresAtUnixMs <= issuedAtUnixMs)
        return ERR_INVALID_ARG;
    rc = dbStmtPrepare(conn, SQL, &st);
    if (rc != SOLARI_OK) return rc;
    dbBindU64(&b[0], &vNode);
    dbBindU64(&b[1], &vScope);
    dbBindStr(&b[2], requestedBy, &lOperator);
    dbBindStr(&b[3], tokenHash, &lHash);
    dbBindU64(&b[4], &vIssued);
    dbBindU64(&b[5], &vExpires);
    rc = dbStmtRunWrite(st, b, 6);
    if (rc == SOLARI_OK) {
        generation = mysql_stmt_insert_id(st);
        if (generationOut) *generationOut = generation;
    }
    mysql_stmt_close(st);
    return rc;
}

solariStatus serverDbDecommissionConfirm(serverDb *db, uint64_t nodeId,
                                         uint32_t wipeScope, const char *requestedBy,
                                         const char tokenHash[65],
                                         uint64_t nowUnixMs,
                                         uint64_t *generationOut)
{
    static const char *CLAIM_SQL =
        "UPDATE decommissionChallenge SET state='consumed', consumedAt=UTC_TIMESTAMP(6), "
        "nextDeliveryAt=UTC_TIMESTAMP(6), deliveryAttempts=0, lastDeliveryAt=NULL, "
        "generation=LAST_INSERT_ID(generation) "
        "WHERE nodeId=? AND wipeScope=? AND BINARY requestedBy=BINARY ? "
        "AND tokenHash=? AND state='pending' "
        "AND expiresAt > FROM_UNIXTIME(? / 1000)";
    MYSQL *conn = dbConn(db);
    MYSQL_STMT *st = NULL;
    MYSQL_BIND b[5];
    unsigned long long vNode=nodeId, vScope=wipeScope, vNow=nowUnixMs;
    unsigned long lOperator=0, lHash=0;
    my_ulonglong changed;
    unsigned long long generation = 0;
    solariStatus rc = ERR_DB;

    if (generationOut) *generationOut = 0;
    if (!conn || nodeId == 0 || wipeScope == 0 || !requestedBy || !requestedBy[0] ||
        !dbHexStringValid(tokenHash, 64, 64) || nowUnixMs == 0)
        return ERR_INVALID_ARG;
    if (dbTxnBegin(conn) != SOLARI_OK) return ERR_DB;
    rc = dbStmtPrepare(conn, CLAIM_SQL, &st);
    if (rc != SOLARI_OK) goto finish;
    dbBindU64(&b[0], &vNode);
    dbBindU64(&b[1], &vScope);
    dbBindStr(&b[2], requestedBy, &lOperator);
    dbBindStr(&b[3], tokenHash, &lHash);
    dbBindU64(&b[4], &vNow);
    rc = dbStmtRunWrite(st, b, 5);
    changed = mysql_stmt_affected_rows(st);
    generation = mysql_stmt_insert_id(st);
    mysql_stmt_close(st);
    st = NULL;
    if (rc != SOLARI_OK) goto finish;
    if (changed == 0) { rc = ERR_AUTH_ROLE; goto finish; }
    rc = SOLARI_OK;

finish:
    if (st) mysql_stmt_close(st);
    rc = dbTxnFinish(db, conn, rc == SOLARI_OK, rc);
    if (rc == SOLARI_OK && generationOut) *generationOut = generation;
    return rc;
}

solariStatus serverDbDecommissionAck(serverDb *db, uint64_t nodeId,
                                     uint64_t generation)
{
    static const char *ACK_SQL =
        "UPDATE decommissionChallenge SET state='acknowledged' "
        "WHERE nodeId=? AND generation=? AND state='consumed'";
    static const char *RETIRE_SQL = "UPDATE node SET state='retired' WHERE nodeId=?";
    MYSQL *conn = dbConn(db);
    MYSQL_STMT *st = NULL;
    MYSQL_BIND b[2], nodeBind[1];
    unsigned long long vNode=nodeId, vGeneration=generation;
    my_ulonglong changed;
    solariStatus rc = ERR_DB;

    if (!conn || nodeId == 0 || generation == 0) return ERR_INVALID_ARG;
    if (dbTxnBegin(conn) != SOLARI_OK) return ERR_DB;
    rc = dbStmtPrepare(conn, ACK_SQL, &st);
    if (rc != SOLARI_OK) goto finish;
    dbBindU64(&b[0], &vNode);
    dbBindU64(&b[1], &vGeneration);
    rc = dbStmtRunWrite(st, b, 2);
    changed = mysql_stmt_affected_rows(st);
    mysql_stmt_close(st);
    st = NULL;
    if (rc != SOLARI_OK) goto finish;
    if (changed == 0) { rc = ERR_AUTH_ROLE; goto finish; }

    rc = dbStmtPrepare(conn, RETIRE_SQL, &st);
    if (rc != SOLARI_OK) goto finish;
    dbBindU64(&nodeBind[0], &vNode);
    rc = dbStmtRunWrite(st, nodeBind, 1);
    mysql_stmt_close(st);
    st = NULL;
    if (rc != SOLARI_OK) goto finish;
    rc = SOLARI_OK;

finish:
    if (st) mysql_stmt_close(st);
    return dbTxnFinish(db, conn, rc == SOLARI_OK, rc);
}

solariStatus serverDbDecommissionListDue(serverDb *db, uint64_t nowUnixMs,
                                         serverDecommissionDelivery *out,
                                         size_t cap, size_t *count)
{
    static const char *SQL =
        "SELECT nodeId,wipeScope,generation,deliveryAttempts "
        "FROM decommissionChallenge WHERE state='consumed' "
        "AND (nextDeliveryAt IS NULL OR nextDeliveryAt <= FROM_UNIXTIME(? / 1000)) "
        "ORDER BY nextDeliveryAt,nodeId LIMIT ?";
    MYSQL *conn = dbConn(db);
    MYSQL_STMT *st = NULL;
    MYSQL_BIND in[2], result[4];
    unsigned long long vNow = nowUnixMs ? nowUnixMs : solariNowUnixMs();
    unsigned long long vCap = (unsigned long long)cap;
    unsigned long long nodeId = 0, scope = 0, generation = 0, attempts = 0;
    my_bool nNode = 0, nScope = 0, nGeneration = 0, nAttempts = 0;
    size_t n = 0;
    int fetch;
    solariStatus rc;

    if (count) *count = 0;
    if (!conn || !count || (cap > 0 && !out) || cap > UINT_MAX) return ERR_INVALID_ARG;
    if (cap == 0) return SOLARI_OK;
    rc = dbStmtPrepare(conn, SQL, &st);
    if (rc != SOLARI_OK) return rc;
    dbBindU64(&in[0], &vNow);
    dbBindU64(&in[1], &vCap);
    if (mysql_stmt_bind_param(st, in) != 0 || mysql_stmt_execute(st) != 0 ||
        mysql_stmt_store_result(st) != 0) {
        mysql_stmt_close(st);
        return ERR_DB;
    }
    memset(result, 0, sizeof result);
    dbBindOutU64(&result[0], &nodeId, &nNode);
    dbBindOutU64(&result[1], &scope, &nScope);
    dbBindOutU64(&result[2], &generation, &nGeneration);
    dbBindOutU64(&result[3], &attempts, &nAttempts);
    if (mysql_stmt_bind_result(st, result) != 0) {
        mysql_stmt_free_result(st);
        mysql_stmt_close(st);
        return ERR_DB;
    }
    while ((fetch = mysql_stmt_fetch(st)) == 0) {
        if (nNode || nScope || nGeneration || nAttempts || n == cap) {
            mysql_stmt_free_result(st);
            mysql_stmt_close(st);
            return ERR_DB;
        }
        out[n].nodeId = (uint64_t)nodeId;
        out[n].wipeScope = (uint32_t)scope;
        out[n].generation = (uint64_t)generation;
        out[n].deliveryAttempts = (uint32_t)attempts;
        n++;
    }
    mysql_stmt_free_result(st);
    mysql_stmt_close(st);
    if (fetch != MYSQL_NO_DATA) return ERR_DB;
    *count = n;
    return SOLARI_OK;
}

solariStatus serverDbDecommissionNoteAttempt(serverDb *db, uint64_t nodeId,
                                             uint64_t generation,
                                             uint64_t nowUnixMs,
                                             uint64_t nextRetryUnixMs)
{
    static const char *SQL =
        "UPDATE decommissionChallenge SET deliveryAttempts=deliveryAttempts+1, "
        "lastDeliveryAt=FROM_UNIXTIME(? / 1000), "
        "nextDeliveryAt=FROM_UNIXTIME(? / 1000) "
        "WHERE nodeId=? AND generation=? AND state='consumed' "
        "AND (nextDeliveryAt IS NULL OR nextDeliveryAt <= FROM_UNIXTIME(? / 1000))";
    MYSQL *conn = dbConn(db);
    MYSQL_STMT *st = NULL;
    MYSQL_BIND b[5];
    unsigned long long vNow = nowUnixMs;
    unsigned long long vNext = nextRetryUnixMs;
    unsigned long long vNode = nodeId;
    unsigned long long vGeneration = generation;
    solariStatus rc;

    if (!conn || nodeId == 0 || generation == 0 || nowUnixMs == 0 ||
        nextRetryUnixMs <= nowUnixMs) return ERR_INVALID_ARG;
    rc = dbStmtPrepare(conn, SQL, &st);
    if (rc != SOLARI_OK) return rc;
    dbBindU64(&b[0], &vNow);
    dbBindU64(&b[1], &vNext);
    dbBindU64(&b[2], &vNode);
    dbBindU64(&b[3], &vGeneration);
    dbBindU64(&b[4], &vNow);
    rc = dbStmtRunWrite(st, b, 5);
    if (rc == SOLARI_OK && mysql_stmt_affected_rows(st) != 1) rc = ERR_AUTH_ROLE;
    mysql_stmt_close(st);
    return rc;
}

solariStatus serverDbRetireNode(serverDb *db, uint64_t nodeId)
{
    static const char *CANCEL_SQL =
        "UPDATE decommissionChallenge SET state='cancelled' "
        "WHERE nodeId=? AND state IN ('pending','consumed')";
    static const char *RETIRE_SQL =
        "UPDATE node SET state='retired' WHERE nodeId=?";
    MYSQL *conn = dbConn(db);
    MYSQL_STMT *st = NULL;
    MYSQL_BIND b[1];
    MYSQL_BIND out[1];
    unsigned long long vNode=nodeId;
    unsigned long long foundNode=0;
    int fetch;
    my_bool nFound=0;
    solariStatus rc = ERR_DB;

    if (!conn || nodeId == 0) return ERR_INVALID_ARG;
    if (dbTxnBegin(conn) != SOLARI_OK) return ERR_DB;
    rc = dbStmtPrepare(conn, CANCEL_SQL, &st);
    if (rc != SOLARI_OK) goto finish;
    dbBindU64(&b[0], &vNode);
    rc = dbStmtRunWrite(st, b, 1);
    if (rc == SOLARI_OK && mysql_stmt_affected_rows(st) == 0) {
        static const char *EXISTS_SQL = "SELECT nodeId FROM node WHERE nodeId=?";
        mysql_stmt_close(st);
        st = NULL;
        rc = dbStmtPrepare(conn, EXISTS_SQL, &st);
        if (rc != SOLARI_OK) goto finish;
        dbBindU64(&b[0], &vNode);
        if (mysql_stmt_bind_param(st, b) != 0 || mysql_stmt_execute(st) != 0 ||
            mysql_stmt_store_result(st) != 0) { rc = ERR_DB; goto finish; }
        memset(out, 0, sizeof out);
        dbBindOutU64(&out[0], &foundNode, &nFound);
        if (mysql_stmt_bind_result(st, out) != 0) { rc = ERR_DB; goto finish; }
        fetch = mysql_stmt_fetch(st);
        if (fetch == MYSQL_NO_DATA) rc = ERR_AUTH_ROLE;
        else if (fetch != 0 || nFound || foundNode != nodeId) rc = ERR_DB;
        else rc = SOLARI_OK; /* already retired is an idempotent success */
    }
    mysql_stmt_close(st);
    st = NULL;
    if (rc != SOLARI_OK) goto finish;

    rc = dbStmtPrepare(conn, RETIRE_SQL, &st);
    if (rc != SOLARI_OK) goto finish;
    dbBindU64(&b[0], &vNode);
    rc = dbStmtRunWrite(st, b, 1);
    mysql_stmt_close(st);
    st = NULL;
    if (rc != SOLARI_OK) goto finish;
    rc = SOLARI_OK;

finish:
    if (st) mysql_stmt_close(st);
    return dbTxnFinish(db, conn, rc == SOLARI_OK, rc);
}

solariStatus serverDbNodeCertMatches(serverDb *db, uint64_t nodeId,
                                     const char *certCn, bool *matches)
{
    static const char *SQL =
        "SELECT 1 FROM node WHERE nodeId = ? AND BINARY certCn = BINARY ? "
        "AND state <> 'retired' LIMIT 1";
    MYSQL *conn = dbConn(db);
    MYSQL_STMT *st;
    MYSQL_BIND b[2];
    unsigned long long vNode = nodeId;
    unsigned long cnLen;
    solariStatus rc;

    if (matches) *matches = false;
    if (!conn || !nodeId || !certCn || !certCn[0] || !matches)
        return ERR_INVALID_ARG;
    rc = dbStmtPrepare(conn, SQL, &st);
    if (rc != SOLARI_OK) return rc;
    dbBindU64(&b[0], &vNode);
    dbBindStr(&b[1], certCn, &cnLen);
    if (mysql_stmt_bind_param(st, b) != 0 || mysql_stmt_execute(st) != 0 ||
        mysql_stmt_store_result(st) != 0) {
        mysql_stmt_close(st);
        return ERR_DB;
    }
    *matches = mysql_stmt_num_rows(st) != 0;
    mysql_stmt_free_result(st);
    mysql_stmt_close(st);
    return SOLARI_OK;
}

solariStatus serverDbNodeIdentity(serverDb *db, uint64_t nodeId,
                                  const char *certCn, solariRole *role,
                                  char *hostFqdn, size_t hostFqdnCap)
{
    static const char *SQL =
        "SELECT role,hostFqdn FROM node WHERE nodeId=? "
        "AND BINARY certCn=BINARY ? AND state <> 'retired' LIMIT 1";
    MYSQL *conn = dbConn(db);
    MYSQL_STMT *st = NULL;
    MYSQL_BIND in[2], out[2];
    unsigned long long vNode = nodeId;
    unsigned long cnLen;
    char roleText[16] = {0};
    unsigned long roleLen = 0, hostLen = 0;
    my_bool isNull[2] = {0, 0};
    solariStatus rc;

    if (role) *role = (solariRole)0;
    if (hostFqdn && hostFqdnCap) hostFqdn[0] = '\0';
    if (!conn || !nodeId || !certCn || !certCn[0] || !role ||
        !hostFqdn || hostFqdnCap == 0)
        return ERR_INVALID_ARG;
    rc = dbStmtPrepare(conn, SQL, &st);
    if (rc != SOLARI_OK) return rc;
    dbBindU64(&in[0], &vNode);
    dbBindStr(&in[1], certCn, &cnLen);
    memset(out, 0, sizeof out);
    out[0].buffer_type = MYSQL_TYPE_STRING;
    out[0].buffer = roleText;
    out[0].buffer_length = sizeof roleText - 1;
    out[0].length = &roleLen;
    out[0].is_null = &isNull[0];
    out[1].buffer_type = MYSQL_TYPE_STRING;
    out[1].buffer = hostFqdn;
    out[1].buffer_length = (unsigned long)(hostFqdnCap - 1);
    out[1].length = &hostLen;
    out[1].is_null = &isNull[1];
    if (mysql_stmt_bind_param(st, in) != 0 || mysql_stmt_execute(st) != 0 ||
        mysql_stmt_bind_result(st, out) != 0 || mysql_stmt_store_result(st) != 0) {
        mysql_stmt_close(st);
        return ERR_DB;
    }
    if (mysql_stmt_fetch(st) != 0 || isNull[0] || isNull[1] ||
        roleLen >= sizeof roleText || hostLen >= hostFqdnCap) {
        mysql_stmt_free_result(st);
        mysql_stmt_close(st);
        return ERR_AUTH_ROLE;
    }
    roleText[roleLen] = '\0';
    hostFqdn[hostLen] = '\0';
    *role = dbRoleParse(roleText);
    mysql_stmt_free_result(st);
    mysql_stmt_close(st);
    return SOLARI_OK;
}

static bool dbCertificateRevocationReasonValid(const char *reason)
{
    return reason &&
        (strcmp(reason, "unspecified") == 0 ||
         strcmp(reason, "keyCompromise") == 0 ||
         strcmp(reason, "affiliationChanged") == 0 ||
         strcmp(reason, "superseded") == 0 ||
         strcmp(reason, "cessationOfOperation") == 0);
}

solariStatus serverDbGetLeafCertificateIssuer(serverDb *db, uint64_t nodeId,
                                               const char *serialHex,
                                               const char *leafCertFp,
                                               char issuerFp[65],
                                               uint64_t *resolvedNodeId)
{
    static const char *SQL =
        "SELECT i.issuerFp,COALESCE(i.nodeId,0) FROM certificateIssuance i "
        "JOIN certificateInventoryState s ON s.singletonId=1 AND s.complete=1 "
        "WHERE BINARY i.serialHex=BINARY ? AND BINARY i.leafCertFp=BINARY ? "
        "AND (?=0 OR i.nodeId=?)";
    MYSQL *conn = dbConn(db);
    MYSQL_STMT *st = NULL;
    MYSQL_BIND in[4], out[2];
    unsigned long lSerial = 0, lLeaf = 0, lIssuer = 0;
    unsigned long long vNode = nodeId, vResolvedNode = 0;
    my_bool isNull = 0;
    int fetch;
    solariStatus rc;

    if (issuerFp) issuerFp[0] = '\0';
    if (resolvedNodeId) *resolvedNodeId = 0;
    if (!conn || !dbHexStringValid(serialHex, 0, 40) ||
        !dbHexStringValid(leafCertFp, 64, 64) || !issuerFp || !resolvedNodeId)
        return ERR_INVALID_ARG;
    rc = dbStmtPrepare(conn, SQL, &st);
    if (rc != SOLARI_OK) return rc;
    dbBindStr(&in[0], serialHex, &lSerial);
    dbBindStr(&in[1], leafCertFp, &lLeaf);
    dbBindU64(&in[2], &vNode);
    dbBindU64(&in[3], &vNode);
    if (mysql_stmt_bind_param(st, in) != 0 || mysql_stmt_execute(st) != 0 ||
        mysql_stmt_store_result(st) != 0) {
        mysql_stmt_close(st);
        return ERR_DB;
    }
    dbBindOutStr(&out[0], issuerFp, 65, &lIssuer, &isNull);
    dbBindOutU64(&out[1], &vResolvedNode, NULL);
    if (mysql_stmt_bind_result(st, out) != 0) {
        mysql_stmt_close(st);
        return ERR_DB;
    }
    fetch = mysql_stmt_fetch(st);
    if (fetch == MYSQL_NO_DATA || isNull || fetch != 0 || lIssuer != 64) {
        issuerFp[0] = '\0';
        rc = fetch == MYSQL_NO_DATA ? ERR_AUTH_ROLE :
             (fetch == 0 && (isNull || lIssuer != 64) ? ERR_AUTH_ROLE : ERR_DB);
    } else {
        issuerFp[64] = '\0';
        *resolvedNodeId = (uint64_t)vResolvedNode;
        rc = SOLARI_OK;
    }
    mysql_stmt_free_result(st);
    mysql_stmt_close(st);
    return rc;
}

solariStatus serverDbRecordIssuedCertificate(serverDb *db,
                                              const char *issuerFp,
                                              const char *serialHex,
                                              const char *leafCertFp,
                                              const char *issuedBy,
                                              uint64_t issuedAtUnixMs)
{
    static const char *SQL =
        "INSERT INTO certificateIssuance "
        "(issuerFp,serialHex,leafCertFp,nodeId,enrollmentId,origin,issuedBy,issuedAt) "
        "VALUES (?,?,?,NULL,NULL,'operator',?,FROM_UNIXTIME(? / 1000))";
    MYSQL *conn = dbConn(db);
    MYSQL_STMT *st = NULL;
    MYSQL_BIND b[5];
    unsigned long lIssuer=0, lSerial=0, lLeaf=0, lBy=0;
    unsigned long long vWhen = issuedAtUnixMs;
    solariStatus rc;

    if (!conn || !dbHexStringValid(issuerFp, 64, 64) ||
        !dbHexStringValid(serialHex, 0, 40) ||
        !dbHexStringValid(leafCertFp, 64, 64) || !issuedBy ||
        !issuedBy[0] || strlen(issuedBy) > 128 || !issuedAtUnixMs)
        return ERR_INVALID_ARG;
    rc = dbStmtPrepare(conn, SQL, &st);
    if (rc != SOLARI_OK) return rc;
    dbBindStr(&b[0], issuerFp, &lIssuer);
    dbBindStr(&b[1], serialHex, &lSerial);
    dbBindStr(&b[2], leafCertFp, &lLeaf);
    dbBindStr(&b[3], issuedBy, &lBy);
    dbBindU64(&b[4], &vWhen);
    rc = dbStmtRunWrite(st, b, 5);
    if (rc == SOLARI_OK && mysql_stmt_affected_rows(st) != 1)
        rc = ERR_AUTH_ROLE;
    mysql_stmt_close(st);
    return rc;
}

solariStatus serverDbCertificateRevocationIssue(
    serverDb *db, uint64_t nodeId, const char *issuerFp, const char *serialHex,
    const char *leafCertFp, const char *reason, const char *requestedBy,
    const char tokenHash[65], uint64_t issuedAtUnixMs, uint64_t expiresAtUnixMs)
{
    static const char *DELETE_EXPIRED =
        "DELETE FROM certificateRevocationChallenge WHERE issuerFp=? AND serialHex=? "
        "AND state IN ('pending','cancelled') AND expiresAt<=FROM_UNIXTIME(? / 1000)";
    static const char *INSERT_SQL =
        "INSERT INTO certificateRevocationChallenge "
        "(issuerFp,serialHex,leafCertFp,nodeId,enrollmentId,reason,requestedBy,tokenHash,state,issuedAt,expiresAt) "
        "SELECT i.issuerFp,i.serialHex,i.leafCertFp,i.nodeId,i.enrollmentId,?,?,?,"
        "'pending',FROM_UNIXTIME(? / 1000),FROM_UNIXTIME(? / 1000) "
        "FROM certificateIssuance i JOIN certificateInventoryState s "
        "ON s.singletonId=1 AND s.complete=1 "
        "WHERE i.issuerFp=? AND BINARY i.serialHex=BINARY ? "
        "AND BINARY i.leafCertFp=BINARY ? "
        "AND ((?=0 AND i.nodeId IS NULL) OR i.nodeId=?) "
        "AND NOT EXISTS (SELECT 1 FROM certificateRevocation r "
        " WHERE r.issuerFp=i.issuerFp AND r.serialHex=i.serialHex)";
    MYSQL *conn = NULL;
    MYSQL_STMT *st = NULL;
    MYSQL_BIND d[3], b[10];
    unsigned long long vNode = nodeId, vIssued = issuedAtUnixMs,
                       vExpires = expiresAtUnixMs;
    unsigned long lIssuer=0, lSerial=0, lFp=0, lReason=0, lOperator=0, lHash=0;
    bool txn = false;
    solariStatus rc;

    if (!db || !db->cfg || !dbHexStringValid(issuerFp, 64, 64) ||
        !dbHexStringValid(serialHex, 0, 40) ||
        !dbHexStringValid(leafCertFp, 64, 64) ||
        !dbCertificateRevocationReasonValid(reason) || !requestedBy ||
        !requestedBy[0] || strlen(requestedBy) > 128 ||
        !dbHexStringValid(tokenHash, 64, 64) || issuedAtUnixMs == 0 ||
        expiresAtUnixMs <= issuedAtUnixMs)
        return ERR_INVALID_ARG;
    conn = dbOpenTxnConn(db);
    if (!conn) return ERR_DB;
    if (mysql_autocommit(conn, 0) != 0) { mysql_close(conn); return ERR_DB; }
    txn = true;
    rc = dbStmtPrepare(conn, DELETE_EXPIRED, &st);
    if (rc != SOLARI_OK) goto finish;
    dbBindStr(&d[0], issuerFp, &lIssuer);
    dbBindStr(&d[1], serialHex, &lSerial);
    dbBindU64(&d[2], &vIssued);
    rc = dbStmtRunWrite(st, d, 3);
    mysql_stmt_close(st);
    st = NULL;
    if (rc != SOLARI_OK) goto finish;

    rc = dbStmtPrepare(conn, INSERT_SQL, &st);
    if (rc != SOLARI_OK) goto finish;
    dbBindStr(&b[0], reason, &lReason);
    dbBindStr(&b[1], requestedBy, &lOperator);
    dbBindStr(&b[2], tokenHash, &lHash);
    dbBindU64(&b[3], &vIssued);
    dbBindU64(&b[4], &vExpires);
    dbBindStr(&b[5], issuerFp, &lIssuer);
    dbBindStr(&b[6], serialHex, &lSerial);
    dbBindStr(&b[7], leafCertFp, &lFp);
    dbBindU64(&b[8], &vNode);
    dbBindU64(&b[9], &vNode);
    rc = dbStmtRunWrite(st, b, 10);
    if (rc != SOLARI_OK && mysql_stmt_errno(st) == 1062) rc = ERR_AUTH_ROLE;
    if (rc != SOLARI_OK) goto finish;
    if (mysql_stmt_affected_rows(st) != 1) { rc = ERR_AUTH_ROLE; goto finish; }
    mysql_stmt_close(st);
    st = NULL;
    if (mysql_commit(conn) != 0) { rc = ERR_DB; goto finish; }
    txn = false;
    rc = SOLARI_OK;

finish:
    if (st) mysql_stmt_close(st);
    if (txn) mysql_rollback(conn);
    mysql_close(conn);
    return rc;
}

solariStatus serverDbCertificateRevocationConfirm(
    serverDb *db, uint64_t nodeId, const char *issuerFp, const char *serialHex,
    const char *leafCertFp, const char *reason, const char *requestedBy,
    const char tokenHash[65], uint64_t nowUnixMs)
{
    static const char *VERIFY_SQL =
        "SELECT COALESCE(i.nodeId,0) FROM certificateIssuance i "
        "JOIN certificateInventoryState s ON s.singletonId=1 AND s.complete=1 "
        "WHERE i.issuerFp=? AND BINARY i.serialHex=BINARY ? "
        "AND BINARY i.leafCertFp=BINARY ? "
        "AND ((?=0 AND i.nodeId IS NULL) OR i.nodeId=?) FOR UPDATE";
    static const char *CONSUME_SQL =
        "UPDATE certificateRevocationChallenge SET state='consumed', consumedAt=UTC_TIMESTAMP(6) "
        "WHERE issuerFp=? AND serialHex=? AND leafCertFp=? AND reason=? "
        "AND BINARY requestedBy=BINARY ? AND tokenHash=? AND state='pending' "
        "AND nodeId <=> NULLIF(?,0) AND expiresAt>FROM_UNIXTIME(? / 1000)";
    static const char *ALREADY_SQL =
        "SELECT 1 FROM certificateRevocation r "
        "JOIN certificateRevocationChallenge c USING (issuerFp,serialHex) "
        "WHERE r.issuerFp=? AND r.serialHex=? AND r.leafCertFp=? "
        "AND r.nodeId <=> NULLIF(?,0) "
        "AND r.reason=? AND BINARY r.revokedBy=BINARY ? "
        "AND c.state='consumed' AND c.tokenHash=? LIMIT 1";
    static const char *INSERT_SQL =
        "INSERT INTO certificateRevocation "
        "(issuerFp,serialHex,leafCertFp,nodeId,enrollmentId,reason,revokedBy,revokedAt) "
        "SELECT i.issuerFp,i.serialHex,i.leafCertFp,i.nodeId,i.enrollmentId,?,?,"
        "FROM_UNIXTIME(? / 1000) FROM certificateIssuance i "
        "JOIN certificateInventoryState s ON s.singletonId=1 AND s.complete=1 "
        "WHERE i.issuerFp=? AND BINARY i.serialHex=BINARY ? "
        "AND BINARY i.leafCertFp=BINARY ? "
        "AND ((?=0 AND i.nodeId IS NULL) OR i.nodeId=?)";
    static const char *RETIRE_SQL =
        "UPDATE node SET state='retired' WHERE nodeId=? "
        "AND BINARY leafCertSerial=BINARY ? AND BINARY leafCertFp=BINARY ?";
    MYSQL *conn = NULL;
    MYSQL_STMT *st = NULL;
    MYSQL_BIND verify[5], consume[8], already[7], insert[8], retire[3], result;
    unsigned long long vNode=nodeId, vNow=nowUnixMs, foundNode=0;
    unsigned long lIssuer=0, lSerial=0, lFp=0, lReason=0, lOperator=0, lHash=0;
    my_bool isNull = 0;
    int fetch;
    bool txn = false;
    solariStatus rc;

    if (!db || !db->cfg || !dbHexStringValid(issuerFp, 64, 64) ||
        !dbHexStringValid(serialHex, 0, 40) ||
        !dbHexStringValid(leafCertFp, 64, 64) ||
        !dbCertificateRevocationReasonValid(reason) || !requestedBy ||
        !requestedBy[0] || strlen(requestedBy) > 128 ||
        !dbHexStringValid(tokenHash, 64, 64) || nowUnixMs == 0)
        return ERR_INVALID_ARG;
    conn = dbOpenTxnConn(db);
    if (!conn) return ERR_DB;
    if (mysql_autocommit(conn, 0) != 0) { mysql_close(conn); return ERR_DB; }
    txn = true;

    rc = dbStmtPrepare(conn, VERIFY_SQL, &st);
    if (rc != SOLARI_OK) goto finish;
    dbBindStr(&verify[0], issuerFp, &lIssuer);
    dbBindStr(&verify[1], serialHex, &lSerial);
    dbBindStr(&verify[2], leafCertFp, &lFp);
    dbBindU64(&verify[3], &vNode);
    dbBindU64(&verify[4], &vNode);
    if (mysql_stmt_bind_param(st, verify) != 0 || mysql_stmt_execute(st) != 0 ||
        mysql_stmt_store_result(st) != 0) { rc = ERR_DB; goto finish; }
    memset(&result, 0, sizeof result);
    dbBindOutU64(&result, &foundNode, &isNull);
    if (mysql_stmt_bind_result(st, &result) != 0) { rc = ERR_DB; goto finish; }
    fetch = mysql_stmt_fetch(st);
    if (fetch == MYSQL_NO_DATA) { rc = ERR_AUTH_ROLE; goto finish; }
    if (fetch != 0) { rc = ERR_DB; goto finish; }
    if (isNull || foundNode != nodeId) { rc = ERR_AUTH_ROLE; goto finish; }
    mysql_stmt_free_result(st);
    mysql_stmt_close(st);
    st = NULL;

    rc = dbStmtPrepare(conn, CONSUME_SQL, &st);
    if (rc != SOLARI_OK) goto finish;
    dbBindStr(&consume[0], issuerFp, &lIssuer);
    dbBindStr(&consume[1], serialHex, &lSerial);
    dbBindStr(&consume[2], leafCertFp, &lFp);
    dbBindStr(&consume[3], reason, &lReason);
    dbBindStr(&consume[4], requestedBy, &lOperator);
    dbBindStr(&consume[5], tokenHash, &lHash);
    dbBindU64(&consume[6], &vNode);
    dbBindU64(&consume[7], &vNow);
    rc = dbStmtRunWrite(st, consume, 8);
    if (rc != SOLARI_OK) goto finish;
    if (mysql_stmt_affected_rows(st) != 1) {
        mysql_stmt_close(st);
        st = NULL;
        rc = dbStmtPrepare(conn, ALREADY_SQL, &st);
        if (rc != SOLARI_OK) goto finish;
        dbBindStr(&already[0], issuerFp, &lIssuer);
        dbBindStr(&already[1], serialHex, &lSerial);
        dbBindStr(&already[2], leafCertFp, &lFp);
        dbBindU64(&already[3], &vNode);
        dbBindStr(&already[4], reason, &lReason);
        dbBindStr(&already[5], requestedBy, &lOperator);
        dbBindStr(&already[6], tokenHash, &lHash);
        if (mysql_stmt_bind_param(st, already) != 0 || mysql_stmt_execute(st) != 0 ||
            mysql_stmt_store_result(st) != 0) { rc = ERR_DB; goto finish; }
        if (mysql_stmt_num_rows(st) != 1) { rc = ERR_AUTH_ROLE; goto finish; }
        mysql_stmt_free_result(st);
        mysql_stmt_close(st);
        st = NULL;
        if (mysql_commit(conn) != 0) { rc = ERR_DB; goto finish; }
        txn = false;
        rc = SOLARI_OK; /* same consumed token and certificate: safe retry */
        goto finish;
    }
    mysql_stmt_close(st);
    st = NULL;

    rc = dbStmtPrepare(conn, INSERT_SQL, &st);
    if (rc != SOLARI_OK) goto finish;
    dbBindStr(&insert[0], reason, &lReason);
    dbBindStr(&insert[1], requestedBy, &lOperator);
    dbBindU64(&insert[2], &vNow);
    dbBindStr(&insert[3], issuerFp, &lIssuer);
    dbBindStr(&insert[4], serialHex, &lSerial);
    dbBindStr(&insert[5], leafCertFp, &lFp);
    dbBindU64(&insert[6], &vNode);
    dbBindU64(&insert[7], &vNode);
    rc = dbStmtRunWrite(st, insert, 8);
    if (rc == SOLARI_OK && mysql_stmt_affected_rows(st) != 1)
        rc = ERR_AUTH_ROLE;
    if (rc != SOLARI_OK) goto finish;
    mysql_stmt_close(st);
    st = NULL;

    if (nodeId != 0) {
        rc = dbStmtPrepare(conn, RETIRE_SQL, &st);
        if (rc != SOLARI_OK) goto finish;
        dbBindU64(&retire[0], &vNode);
        dbBindStr(&retire[1], serialHex, &lSerial);
        dbBindStr(&retire[2], leafCertFp, &lFp);
        rc = dbStmtRunWrite(st, retire, 3);
        if (rc != SOLARI_OK) goto finish;
        mysql_stmt_close(st);
        st = NULL;
    }

    if (mysql_commit(conn) != 0) { rc = ERR_DB; goto finish; }
    txn = false;
    rc = SOLARI_OK;

finish:
    if (st) mysql_stmt_close(st);
    if (txn) mysql_rollback(conn);
    mysql_close(conn);
    return rc;
}

solariStatus serverDbCertificateCrlAcknowledge(serverDb *db, uint64_t nodeId,
                                              const char *issuerFp,
                                              uint64_t crlNumber,
                                              const char *crlFp)
{
    static const char *SQL =
        "INSERT INTO certificateCrlAcknowledgement "
        "(nodeId,issuerFp,crlNumber,crlFp,acknowledgedAt) "
        "SELECT ?,issuerFp,publishedNumber,crlFp,UTC_TIMESTAMP(6) "
        "FROM certificateCrlState WHERE issuerFp=? AND publishedNumber=? "
        "AND BINARY crlFp=BINARY ? AND nextUpdate>UTC_TIMESTAMP(6) "
        "ON DUPLICATE KEY UPDATE crlNumber=VALUES(crlNumber), "
        "crlFp=VALUES(crlFp), acknowledgedAt=GREATEST(UTC_TIMESTAMP(6), "
        "acknowledgedAt+INTERVAL 1 MICROSECOND)";
    MYSQL *conn = dbConn(db);
    MYSQL_STMT *st = NULL;
    MYSQL_BIND b[4];
    unsigned long long vNode = nodeId, vNumber = crlNumber;
    unsigned long lIssuer = 0, lFp = 0;
    my_ulonglong changed;
    solariStatus rc;

    if (!conn || !nodeId || !crlNumber ||
        !dbHexStringValid(issuerFp, 64, 64) || !dbHexStringValid(crlFp, 64, 64))
        return ERR_INVALID_ARG;
    rc = dbStmtPrepare(conn, SQL, &st);
    if (rc != SOLARI_OK) return rc;
    dbBindU64(&b[0], &vNode);
    dbBindStr(&b[1], issuerFp, &lIssuer);
    dbBindU64(&b[2], &vNumber);
    dbBindStr(&b[3], crlFp, &lFp);
    rc = dbStmtRunWrite(st, b, 4);
    changed = rc == SOLARI_OK ? mysql_stmt_affected_rows(st) : 0;
    mysql_stmt_close(st);
    if (rc != SOLARI_OK) return rc;
    return changed ? SOLARI_OK : ERR_REJECTED;
}

solariStatus serverDbUpsertProbeTarget(serverDb *db, const char *targetId,
                                       const char *host, int port,
                                       const char *proto, int replFactor,
                                       const char *label, const char *segId,
                                       uint64_t assetId,
                                       const char *probeType,
                                       const char *checkArg)
{
    static const char *SQL =
        "INSERT INTO probeTarget "
        "  (targetId, host, port, proto, replFactor, label, segId, assetId, probeType, checkArg) "
        "SELECT ?, ?, ?, ?, ?, ?, ?, ?, ?, ? "
        "WHERE (? = 0 OR EXISTS (SELECT 1 FROM asset WHERE assetId = ? AND lifecycle = 'active')) "
        "AND NOT EXISTS (SELECT 1 FROM probeTargetTombstone WHERE targetId = ?) "
        "ON DUPLICATE KEY UPDATE host=VALUES(host), port=VALUES(port), "
        "  proto=VALUES(proto), replFactor=VALUES(replFactor), "
        "  label=VALUES(label), segId=VALUES(segId), assetId=VALUES(assetId), "
        "  probeType=VALUES(probeType), checkArg=VALUES(checkArg)";
    MYSQL      *conn = dbConn(db);
    MYSQL_STMT *st;
    MYSQL_BIND  b[13];
    int          vPort = port, vRepl = replFactor;
    unsigned long long vAsset = assetId;
    const char  *vProbeType = (probeType && probeType[0]) ? probeType : "tcp";
    unsigned long lTid, lHost, lProto, lLabel, lSeg, lProbe, lArg;
    solariStatus rc;

    if (!conn || !targetId || !proto) return ERR_INVALID_ARG;
    rc = dbStmtPrepare(conn, SQL, &st);
    if (rc != SOLARI_OK) return rc;
    dbBindStr(&b[0], targetId, &lTid);
    dbBindStr(&b[1], (host && host[0]) ? host : NULL, &lHost);
    dbBindI32(&b[2], &vPort);
    dbBindStr(&b[3], proto, &lProto);
    dbBindI32(&b[4], &vRepl);
    dbBindStr(&b[5], (label && label[0]) ? label : NULL, &lLabel);
    dbBindStr(&b[6], (segId && segId[0]) ? segId : NULL, &lSeg);
    if (assetId) dbBindU64(&b[7], &vAsset); else dbBindStr(&b[7], NULL, &lSeg);
    dbBindStr(&b[8], vProbeType, &lProbe);
    dbBindStr(&b[9], (checkArg && checkArg[0]) ? checkArg : NULL, &lArg);
    dbBindU64(&b[10], &vAsset);
    dbBindU64(&b[11], &vAsset);
    dbBindStr(&b[12], targetId, &lTid);
    rc = dbStmtRunWrite(st, b, 13);
    mysql_stmt_close(st);
    return rc;
}

solariStatus serverDbDeleteProbeTarget(serverDb *db, const char *targetId)
{
    static const char *SQL = "DELETE FROM probeTarget WHERE targetId = ?";
    MYSQL      *conn = dbConn(db);
    MYSQL_STMT *st;
    MYSQL_BIND  b[1];
    unsigned long l0;
    solariStatus rc;

    if (!conn || !targetId) return ERR_INVALID_ARG;
    rc = dbStmtPrepare(conn, SQL, &st);
    if (rc != SOLARI_OK) return rc;
    dbBindStr(&b[0], targetId, &l0);
    rc = dbStmtRunWrite(st, b, 1);
    mysql_stmt_close(st);
    return rc;
}

solariStatus serverDbPurgeProbeState(serverDb *db, const char *targetId)
{
    static const char *SQL_CUR = "DELETE FROM probeCurrent WHERE targetId = ?";
    static const char *SQL_HIS = "DELETE FROM probeHistory WHERE targetId = ?";
    MYSQL      *conn = dbConn(db);
    MYSQL_STMT *st;
    MYSQL_BIND  b[1];
    unsigned long l0;
    solariStatus rc;

    if (!conn || !targetId) return ERR_INVALID_ARG;
    rc = dbStmtPrepare(conn, SQL_CUR, &st);
    if (rc != SOLARI_OK) return rc;
    dbBindStr(&b[0], targetId, &l0);
    rc = dbStmtRunWrite(st, b, 1);
    mysql_stmt_close(st);
    if (rc != SOLARI_OK) return rc;

    rc = dbStmtPrepare(conn, SQL_HIS, &st);
    if (rc != SOLARI_OK) return rc;
    dbBindStr(&b[0], targetId, &l0);
    rc = dbStmtRunWrite(st, b, 1);
    mysql_stmt_close(st);
    return rc;
}

solariStatus serverDbGetAssetIdByIp(serverDb *db, const char *ip, uint64_t *assetId)
{
    static const char *SQL = "SELECT assetId FROM asset WHERE ip = ?";
    MYSQL      *conn = dbConn(db);
    MYSQL_STMT *st;
    MYSQL_BIND  in[1], out[1];
    unsigned long lIp;
    unsigned long long vId = 0;
    my_bool      isNull = 0;
    solariStatus rc;

    if (assetId) *assetId = 0;
    if (!conn || !ip || !assetId) return ERR_INVALID_ARG;
    rc = dbStmtPrepare(conn, SQL, &st);
    if (rc != SOLARI_OK) return rc;
    dbBindStr(&in[0], ip, &lIp);
    if (mysql_stmt_bind_param(st, in) != 0) { mysql_stmt_close(st); return ERR_DB; }
    if (mysql_stmt_execute(st) != 0)        { mysql_stmt_close(st); return ERR_DB; }
    dbBindOutU64(&out[0], &vId, &isNull);
    if (mysql_stmt_bind_result(st, out) != 0) { mysql_stmt_close(st); return ERR_DB; }
    if (mysql_stmt_fetch(st) == 0 && !isNull) *assetId = vId;
    mysql_stmt_close(st);
    return SOLARI_OK;
}

/* serverDbGetAssetHeartbeatInfo - ip + monitorHost flag for one asset.
 * Input: assetId. Output: ip (caller buffer) and monitorHost (0/1).
 * Used by the lifecycle-restore heartbeat resync. */
solariStatus serverDbGetAssetHeartbeatInfo(serverDb *db, uint64_t assetId,
                                           char *ip, size_t ipCap, int *monitorHost)
{
    char       sql[160];
    MYSQL     *conn;
    MYSQL_RES *res;
    MYSQL_ROW  row;

    if (!db || !assetId || !ip || !ipCap || !monitorHost) return ERR_INVALID_ARG;
    conn = dbConn(db);
    if (!conn) return ERR_DB;
    snprintf(sql, sizeof sql,
             "SELECT ip, monitorHost FROM asset WHERE assetId=%llu",
             (unsigned long long)assetId);
    if (mysql_query(conn, sql) != 0) return ERR_DB;
    res = mysql_store_result(conn);
    if (!res) return ERR_DB;
    row = mysql_fetch_row(res);
    if (!row || !row[0]) { mysql_free_result(res); return ERR_DB; }  /* no such asset */
    snprintf(ip, ipCap, "%s", row[0]);
    *monitorHost = row[1] ? atoi(row[1]) : 0;
    mysql_free_result(res);
    return SOLARI_OK;
}

solariStatus serverDbGetAssetConfirmValues(serverDb *db, uint64_t assetId,
                                           char *displayName, size_t displayCap,
                                           char *host, size_t hostCap,
                                           char *ip, size_t ipCap)
{
    MYSQL *conn = dbConn(db);
    MYSQL_STMT *st;
    MYSQL_BIND in[1], out[3];
    unsigned long ld = 0, lh = 0, li = 0;
    unsigned long long id = assetId;
    my_bool nd = 0, nh = 0, ni = 0;
    solariStatus rc;
    static const char *SQL = "SELECT displayName,host,ip FROM asset WHERE assetId=?";

    if (!conn || !assetId || !displayName || !displayCap || !host || !hostCap || !ip || !ipCap)
        return ERR_INVALID_ARG;
    displayName[0] = host[0] = ip[0] = '\0';
    rc = dbStmtPrepare(conn, SQL, &st); if (rc != SOLARI_OK) return rc;
    dbBindU64(&in[0], &id);
    if (mysql_stmt_bind_param(st, in) != 0 || mysql_stmt_execute(st) != 0) {
        mysql_stmt_close(st); return ERR_DB;
    }
    dbBindOutStr(&out[0], displayName, displayCap, &ld, &nd);
    dbBindOutStr(&out[1], host, hostCap, &lh, &nh);
    dbBindOutStr(&out[2], ip, ipCap, &li, &ni);
    if (mysql_stmt_bind_result(st, out) != 0) {
        mysql_stmt_close(st); return ERR_DB;
    }
    {
        int fetch = mysql_stmt_fetch(st);
        if (fetch != 0 && fetch != MYSQL_NO_DATA) {
            mysql_stmt_close(st); return ERR_DB;
        }
        if (fetch == 0) {
            if (!nd) displayName[ld < displayCap ? ld : displayCap - 1] = '\0';
            if (!nh) host[lh < hostCap ? lh : hostCap - 1] = '\0';
            if (!ni) ip[li < ipCap ? li : ipCap - 1] = '\0';
        }
    }
    mysql_stmt_close(st);
    return SOLARI_OK;
}

solariStatus serverDbListAssetTargets(serverDb *db, uint64_t assetId,
                                      char out[][SERVER_TARGETID_MAX],
                                      size_t cap, size_t *count)
{
    static const char *SQL =
        "SELECT targetId FROM probeTarget WHERE assetId = ? ORDER BY targetId";
    MYSQL      *conn = dbConn(db);
    MYSQL_STMT *st;
    MYSQL_BIND  in[1], res[1];
    unsigned long long vId = assetId;
    char targetId[SERVER_TARGETID_MAX];
    unsigned long lTid = 0;
    my_bool nTid = 0;
    size_t n = 0;
    solariStatus rc;

    if (count) *count = 0;
    if (!conn || assetId == 0 || !out || !count) return ERR_INVALID_ARG;
    rc = dbStmtPrepare(conn, SQL, &st);
    if (rc != SOLARI_OK) return rc;
    dbBindU64(&in[0], &vId);
    if (mysql_stmt_bind_param(st, in) != 0) { mysql_stmt_close(st); return ERR_DB; }
    if (mysql_stmt_execute(st) != 0)        { mysql_stmt_close(st); return ERR_DB; }
    dbBindOutStr(&res[0], targetId, sizeof targetId, &lTid, &nTid);
    if (mysql_stmt_bind_result(st, res) != 0) { mysql_stmt_close(st); return ERR_DB; }
    while (mysql_stmt_fetch(st) == 0) {
        if (n >= cap) { mysql_stmt_close(st); return ERR_BUFFER_FULL; }
        targetId[sizeof targetId - 1] = '\0';
        snprintf(out[n], SERVER_TARGETID_MAX, "%s", nTid ? "" : targetId);
        n++;
    }
    mysql_stmt_close(st);
    *count = n;
    return SOLARI_OK;
}

solariStatus serverDbDeleteAsset(serverDb *db, uint64_t assetId)
{
    static const char *SQL = "DELETE FROM asset WHERE assetId = ?";
    MYSQL      *conn = dbConn(db);
    MYSQL_STMT *st;
    MYSQL_BIND  b[1];
    unsigned long long vId = assetId;
    solariStatus rc;

    if (!conn || assetId == 0) return ERR_INVALID_ARG;
    rc = dbStmtPrepare(conn, SQL, &st);
    if (rc != SOLARI_OK) return rc;
    dbBindU64(&b[0], &vId);
    rc = dbStmtRunWrite(st, b, 1);
    if (rc == SOLARI_OK && mysql_stmt_affected_rows(st) != 1)
        rc = ERR_AUTH_ROLE;
    mysql_stmt_close(st);
    return rc;
}

/* Lifecycle reads deliberately hit the database on every caller path: a
 * transition must win over an in-flight report, so these values are never
 * cached in the alert or reconciliation loops. */
static solariStatus dbLifecycleQuery(serverDb *db, const char *sql, bool *active,
                                     int *tier, uint64_t *assetId)
{
    MYSQL *conn = dbConn(db);
    MYSQL_RES *res;
    MYSQL_ROW row;
    if (active) *active = false;
    if (tier) *tier = 2;
    if (assetId) *assetId = 0;
    if (!conn || mysql_query(conn, sql) != 0) return ERR_DB;
    res = mysql_store_result(conn);
    if (!res) return ERR_DB;
    row = mysql_fetch_row(res);
    if (row) {
        if (active) *active = row[0] && strcmp(row[0], "active") == 0;
        if (tier && row[1]) *tier = atoi(row[1]);
        if (assetId && row[2]) *assetId = strtoull(row[2], NULL, 10);
    }
    mysql_free_result(res);
    return SOLARI_OK;
}

solariStatus serverDbAssetIsActive(serverDb *db, uint64_t assetId, bool *active)
{
    char sql[160];
    if (!active || assetId == 0) return ERR_INVALID_ARG;
    snprintf(sql, sizeof sql, "SELECT lifecycle,criticality,assetId FROM asset WHERE assetId=%llu",
             (unsigned long long)assetId);
    return dbLifecycleQuery(db, sql, active, NULL, NULL);
}

solariStatus serverDbNodeAlertTier(serverDb *db, uint64_t nodeId, int *tier, bool *active)
{
    char sql[160];
    if (!tier || !active || nodeId == 0) return ERR_INVALID_ARG;
    snprintf(sql, sizeof sql, "SELECT IF(state='retired','retired','active'),criticality,0 FROM node WHERE nodeId=%llu",
             (unsigned long long)nodeId);
    return dbLifecycleQuery(db, sql, active, tier, NULL);
}

solariStatus serverDbProbeAlertTier(serverDb *db, const char *targetId, uint64_t *assetId,
                                    int *tier, bool *active)
{
    MYSQL *conn = dbConn(db);
    MYSQL_STMT *st;
    MYSQL_BIND in[1], out[3];
    unsigned long lTarget, lLife = 0;
    char life[16]; int vTier = 2; unsigned long long vAsset = 0; my_bool nLife=0,nTier=0,nAsset=0;
    static const char *SQL = "SELECT COALESCE(a.lifecycle,'active'),COALESCE(a.criticality,2),p.assetId FROM probeTarget p LEFT JOIN asset a ON a.assetId=p.assetId WHERE p.targetId=?";
    solariStatus rc;
    if (assetId) *assetId = 0;
    if (tier) *tier = 2;
    if (active) *active = false;
    if (!conn || !targetId || !assetId || !tier || !active) return ERR_INVALID_ARG;
    rc=dbStmtPrepare(conn,SQL,&st); if(rc!=SOLARI_OK) return rc;
    dbBindStr(&in[0],targetId,&lTarget);
    if(mysql_stmt_bind_param(st,in)!=0 || mysql_stmt_execute(st)!=0){mysql_stmt_close(st);return ERR_DB;}
    dbBindOutStr(&out[0],life,sizeof life,&lLife,&nLife); dbBindOutI32(&out[1],&vTier,&nTier); dbBindOutU64(&out[2],&vAsset,&nAsset);
    if(mysql_stmt_bind_result(st,out)!=0){mysql_stmt_close(st);return ERR_DB;}
    if(mysql_stmt_fetch(st)==0){*active=!nLife && strcmp(life,"active")==0; if(!nTier)*tier=vTier; if(!nAsset)*assetId=vAsset;}
    mysql_stmt_close(st); return SOLARI_OK;
}

solariStatus serverDbAlertEventDisposition(serverDb *db, uint64_t eventId,
                                           char *disposition, size_t dispositionCap,
                                           int *effectiveTier)
{
    MYSQL *conn = dbConn(db);
    MYSQL_STMT *st;
    MYSQL_BIND in[1], out[2];
    unsigned long ld = 0;
    unsigned long long id = eventId;
    int tier = 0;
    my_bool nd = 0, nt = 0;
    solariStatus rc;
    static const char *SQL = "SELECT disposition,effectiveTier FROM alertEvent WHERE eventId=? AND eventKind='fired'";

    if (!conn || !eventId || !disposition || !dispositionCap || !effectiveTier)
        return ERR_INVALID_ARG;
    disposition[0] = '\0'; *effectiveTier = 0;
    rc = dbStmtPrepare(conn, SQL, &st); if (rc != SOLARI_OK) return rc;
    dbBindU64(&in[0], &id);
    if (mysql_stmt_bind_param(st, in) != 0 || mysql_stmt_execute(st) != 0) {
        mysql_stmt_close(st); return ERR_DB;
    }
    dbBindOutStr(&out[0], disposition, dispositionCap, &ld, &nd);
    dbBindOutI32(&out[1], &tier, &nt);
    if (mysql_stmt_bind_result(st, out) != 0 || mysql_stmt_fetch(st) != 0 || nd || nt) {
        mysql_stmt_close(st); return ERR_DB;
    }
    *effectiveTier = tier;
    mysql_stmt_close(st);
    return SOLARI_OK;
}

solariStatus serverDbSetAssetLifecycle(serverDb *db, uint64_t assetId, const char *to)
{
    MYSQL *conn=dbConn(db); MYSQL_STMT *st; MYSQL_BIND b[2]; unsigned long l; unsigned long long id=assetId; solariStatus rc;
    static const char *SQL="UPDATE asset SET lifecycle=?,updatedAt=UTC_TIMESTAMP() WHERE assetId=?";
    if (!conn || !to || assetId == 0) return ERR_INVALID_ARG;
    rc = dbStmtPrepare(conn, SQL, &st);
    if (rc != SOLARI_OK) return rc;
    dbBindStr(&b[0],to,&l);dbBindU64(&b[1],&id);rc=dbStmtRunWrite(st,b,2);mysql_stmt_close(st);return rc;
}

static solariStatus dbAssetLockExists(serverDb *db, uint64_t assetId)
{
    static const char *SQL = "SELECT assetId FROM asset WHERE assetId=? FOR UPDATE";
    MYSQL *conn = dbConn(db);
    MYSQL_STMT *st = NULL;
    MYSQL_BIND in[1], out[1];
    unsigned long long vId = assetId, found = 0;
    my_bool isNull = 0;
    solariStatus rc = ERR_DB;
    int fetch;

    if (!conn || assetId == 0) return ERR_INVALID_ARG;
    rc = dbStmtPrepare(conn, SQL, &st);
    if (rc != SOLARI_OK) return rc;
    dbBindU64(&in[0], &vId);
    if (mysql_stmt_bind_param(st, in) != 0 || mysql_stmt_execute(st) != 0 ||
        mysql_stmt_store_result(st) != 0) goto done;
    memset(out, 0, sizeof out);
    dbBindOutU64(&out[0], &found, &isNull);
    if (mysql_stmt_bind_result(st, out) != 0) goto done;
    fetch = mysql_stmt_fetch(st);
    if (fetch == MYSQL_NO_DATA) rc = ERR_AUTH_ROLE;
    else if (fetch == 0 && !isNull && found == assetId) rc = SOLARI_OK;

done:
    mysql_stmt_close(st);
    return rc;
}

solariStatus serverDbLifecycleTransition(serverDb *db, uint64_t assetId, const char *to,
                                         char targets[][SERVER_TARGETID_MAX], size_t count)
{
    MYSQL *conn=dbConn(db); solariStatus rc; size_t i;
    if(!conn||!to||assetId==0||(strcmp(to,"active")&&(!targets&&count)))return ERR_INVALID_ARG;
    if(dbTxnBegin(conn)!=SOLARI_OK)return ERR_DB;
    rc=dbAssetLockExists(db,assetId);
    if(rc!=SOLARI_OK)return dbTxnFinish(db,conn,false,rc);
    if(strcmp(to,"active")!=0){
        rc=serverDbClearOpenAssetAlerts(db,assetId);
        if(rc!=SOLARI_OK)goto fail_nostmt;
        for(i=0;i<count;i++){rc=serverDbDeleteProbeTarget(db,targets[i]);if(rc!=SOLARI_OK)goto fail_nostmt;}
    } else {
        rc=serverDbClearAssetTombstones(db,assetId);
        if(rc!=SOLARI_OK)goto fail_nostmt;
    }
    rc=serverDbSetAssetLifecycle(db,assetId,to);
    if(rc!=SOLARI_OK)goto fail_nostmt;
    return dbTxnFinish(db,conn,true,SOLARI_OK);
fail_nostmt:
    return dbTxnFinish(db,conn,false,rc);
}

solariStatus serverDbPurgeAsset(serverDb *db, uint64_t assetId,
                                 char targets[][SERVER_TARGETID_MAX], size_t count)
{
    MYSQL *conn=dbConn(db); solariStatus rc; size_t i;
    if(!conn||assetId==0||(!targets&&count))return ERR_INVALID_ARG;
    if(dbTxnBegin(conn)!=SOLARI_OK)return ERR_DB;
    rc=serverDbClearOpenAssetAlerts(db,assetId);
    if(rc!=SOLARI_OK)goto fail_nostmt;
    for(i=0;i<count;i++){rc=serverDbPurgeProbeState(db,targets[i]);if(rc!=SOLARI_OK)goto fail_nostmt;rc=serverDbDeleteProbeTarget(db,targets[i]);if(rc!=SOLARI_OK)goto fail_nostmt;}
    rc=serverDbDeleteAsset(db,assetId);
    if(rc!=SOLARI_OK)goto fail_nostmt;
    return dbTxnFinish(db,conn,true,SOLARI_OK);
fail_nostmt:
    return dbTxnFinish(db,conn,false,rc);
}

solariStatus serverDbSetCriticality(serverDb *db, uint64_t assetId, uint64_t nodeId, int tier)
{
    char sql[160]; MYSQL *conn=dbConn(db);
    if(!conn || tier<0 || tier>4 || ((assetId==0)==(nodeId==0))) return ERR_INVALID_ARG;
    snprintf(sql,sizeof sql,"UPDATE %s SET criticality=%d WHERE %s=%llu",assetId?"asset":"node",tier,assetId?"assetId":"nodeId",(unsigned long long)(assetId?assetId:nodeId));
    return mysql_query(conn,sql)==0 ? SOLARI_OK : ERR_DB;
}

solariStatus serverDbTombstoneProbeTarget(serverDb *db, const char *targetId, const char *operator_)
{
    MYSQL *conn=dbConn(db); MYSQL_STMT *st; MYSQL_BIND b[2]; unsigned long lt,lo; solariStatus rc;
    static const char *SQL="INSERT INTO probeTargetTombstone(targetId,removedAt,removedBy,assetId) SELECT targetId,UTC_TIMESTAMP(),?,assetId FROM probeTarget WHERE targetId=? ON DUPLICATE KEY UPDATE removedAt=VALUES(removedAt),removedBy=VALUES(removedBy),assetId=VALUES(assetId)";
    if (!conn || !targetId || !operator_) return ERR_INVALID_ARG;
    rc = dbStmtPrepare(conn, SQL, &st);
    if (rc != SOLARI_OK) return rc;
    dbBindStr(&b[0],operator_,&lo);dbBindStr(&b[1],targetId,&lt);rc=dbStmtRunWrite(st,b,2);mysql_stmt_close(st);return rc;
}

solariStatus serverDbClearAssetTombstones(serverDb *db, uint64_t assetId)
{
    char sql[256]; MYSQL *conn=dbConn(db); if(!conn||assetId==0)return ERR_INVALID_ARG;
    snprintf(sql,sizeof sql,"DELETE FROM probeTargetTombstone WHERE assetId=%llu",(unsigned long long)assetId);
    return mysql_query(conn,sql)==0?SOLARI_OK:ERR_DB;
}

static solariStatus dbClearOpenAlerts(serverDb *db, const char *predicate)
{
    char sql[2048]; MYSQL *conn=dbConn(db); if(!conn)return ERR_INVALID_ARG;
    snprintf(sql,sizeof sql,
        "INSERT INTO alertEvent(ruleId,nodeId,targetId,firedAt,clearedAt,severity,detail,eventKind,openedEventId,assetId,effectiveTier,disposition) "
        "SELECT f.ruleId,f.nodeId,f.targetId,UTC_TIMESTAMP(),UTC_TIMESTAMP(),f.severity,CONCAT('CLEARED lifecycle ',f.detail),'cleared',f.eventId,f.assetId,f.effectiveTier,f.disposition FROM alertEvent f "
        "WHERE (%s) AND f.eventKind='fired' AND f.clearedAt IS NULL AND NOT EXISTS (SELECT 1 FROM alertEvent c WHERE c.openedEventId=f.eventId AND c.eventKind='cleared')",predicate);
    if (mysql_query(conn,sql) != 0) return ERR_DB;
    snprintf(sql,sizeof sql,
        "UPDATE alertEvent f SET f.clearedAt=UTC_TIMESTAMP() "
        "WHERE (%s) AND f.eventKind='fired' AND f.clearedAt IS NULL", predicate);
    return mysql_query(conn,sql)==0 ? SOLARI_OK : ERR_DB;
}
solariStatus serverDbClearOpenAssetAlerts(serverDb *db,uint64_t assetId)
{
    char predicate[1024];
    if (!assetId) return ERR_INVALID_ARG;
    /* Four arms (review R2-M1). Arms 1-3 cover new-engine rows (assetId /
     * nodeId linkage / exact targetId). Arm 4 bridges the two targetId
     * id-spaces for LEGACY rows: alertEvent uses the numeric-proto form
     * ("2:ip:port", icmp port 0) while probeTarget uses the name form
     * ("tcp:ip:port", icmp without port) — exact equality between them
     * matches nothing, which left every pre-018 open alert uncleared. */
    snprintf(predicate, sizeof predicate,
             "f.assetId=%llu"
             " OR f.nodeId=(SELECT nodeId FROM asset WHERE assetId=%llu)"
             " OR f.targetId IN (SELECT targetId FROM probeTarget WHERE assetId=%llu)"
             " OR EXISTS (SELECT 1 FROM probeTarget p WHERE p.assetId=%llu"
             " AND f.targetId = CONCAT("
             "CASE p.proto WHEN 'icmp' THEN '1' WHEN 'tcp' THEN '2' ELSE '3' END,"
             "':', p.host, ':', COALESCE(p.port,0)))",
             (unsigned long long)assetId, (unsigned long long)assetId,
             (unsigned long long)assetId, (unsigned long long)assetId);
    return dbClearOpenAlerts(db, predicate);
}
solariStatus serverDbClearOpenNodeAlerts(serverDb *db,uint64_t nodeId)
{
    char predicate[64];
    if (!nodeId) return ERR_INVALID_ARG;
    snprintf(predicate, sizeof predicate, "f.nodeId=%llu", (unsigned long long)nodeId);
    return dbClearOpenAlerts(db, predicate);
}

solariStatus serverDbUpsertAsset(serverDb *db, const char *ip, const char *host,
                                 const char *displayName, const char *className,
                                 uint64_t poolId, const char *tagsJson,
                                 const char *notes, bool monitorHost,
                                 uint64_t *assetId)
{
    static const char *SQL =
        "INSERT INTO asset (ip, host, displayName, class, poolId, tags, notes, "
        "  monitorHost, createdAt, updatedAt) "
        "VALUES (?, ?, ?, ?, ?, ?, ?, ?, UTC_TIMESTAMP(), UTC_TIMESTAMP()) "
        "ON DUPLICATE KEY UPDATE "
        "  host=COALESCE(VALUES(host),host), displayName=VALUES(displayName), "
        "  class=VALUES(class), poolId=VALUES(poolId), tags=VALUES(tags), "
        "  notes=VALUES(notes), monitorHost=VALUES(monitorHost), "
        "  updatedAt=UTC_TIMESTAMP()";
    MYSQL      *conn = dbConn(db);
    MYSQL_STMT *st;
    MYSQL_BIND  b[8];
    unsigned long long vPool = poolId;
    int          vMon = monitorHost ? 1 : 0;
    unsigned long lIp, lHost, lName, lClass, lTags, lNotes;
    solariStatus rc;

    if (!conn || !ip || !className) return ERR_INVALID_ARG;
    rc = dbStmtPrepare(conn, SQL, &st);
    if (rc != SOLARI_OK) return rc;
    dbBindStr(&b[0], ip, &lIp);
    dbBindStr(&b[1], (host && host[0]) ? host : NULL, &lHost);
    dbBindStr(&b[2], (displayName && displayName[0]) ? displayName : NULL, &lName);
    dbBindStr(&b[3], className, &lClass);
    if (poolId) dbBindU64(&b[4], &vPool); else dbBindStr(&b[4], NULL, &lTags);
    dbBindStr(&b[5], (tagsJson && tagsJson[0]) ? tagsJson : NULL, &lTags);
    dbBindStr(&b[6], (notes && notes[0]) ? notes : NULL, &lNotes);
    dbBindI32(&b[7], &vMon);
    rc = dbStmtRunWrite(st, b, 8);
    mysql_stmt_close(st);
    if (rc != SOLARI_OK) return rc;
    return serverDbGetAssetIdByIp(db, ip, assetId);
}

solariStatus serverDbCreatePool(serverDb *db, const char *name, const char *desc,
                                const char *color, uint64_t *poolId)
{
    static const char *SQL =
        "INSERT INTO pool (name, description, color, createdAt) "
        "VALUES (?, ?, ?, UTC_TIMESTAMP())";
    MYSQL      *conn = dbConn(db);
    MYSQL_STMT *st;
    MYSQL_BIND  b[3];
    unsigned long lN, lD, lC;
    solariStatus rc;

    if (poolId) *poolId = 0;
    if (!conn || !name || !name[0]) return ERR_INVALID_ARG;
    rc = dbStmtPrepare(conn, SQL, &st);
    if (rc != SOLARI_OK) return rc;
    dbBindStr(&b[0], name, &lN);
    dbBindStr(&b[1], (desc && desc[0]) ? desc : NULL, &lD);
    dbBindStr(&b[2], (color && color[0]) ? color : NULL, &lC);
    rc = dbStmtRunWrite(st, b, 3);
    if (rc == SOLARI_OK && poolId)
        *poolId = (unsigned long long)mysql_stmt_insert_id(st);
    mysql_stmt_close(st);
    return rc;
}

solariStatus serverDbUpdatePool(serverDb *db, uint64_t poolId, const char *name,
                                const char *desc, const char *color)
{
    static const char *SQL =
        "UPDATE pool SET name=COALESCE(?,name), description=COALESCE(?,description), "
        "  color=COALESCE(?,color) WHERE poolId=?";
    MYSQL      *conn = dbConn(db);
    MYSQL_STMT *st;
    MYSQL_BIND  b[4];
    unsigned long long vId = poolId;
    unsigned long lN, lD, lC;
    solariStatus rc;

    if (!conn || poolId == 0) return ERR_INVALID_ARG;
    rc = dbStmtPrepare(conn, SQL, &st);
    if (rc != SOLARI_OK) return rc;
    dbBindStr(&b[0], (name  && name[0])  ? name  : NULL, &lN);
    dbBindStr(&b[1], (desc  && desc[0])  ? desc  : NULL, &lD);
    dbBindStr(&b[2], (color && color[0]) ? color : NULL, &lC);
    dbBindU64(&b[3], &vId);
    rc = dbStmtRunWrite(st, b, 4);
    mysql_stmt_close(st);
    return rc;
}

solariStatus serverDbReassignPoolAssets(serverDb *db, uint64_t fromPool,
                                        uint64_t toPool, size_t *n)
{
    static const char *SQL = "UPDATE asset SET poolId = ? WHERE poolId = ?";
    MYSQL      *conn = dbConn(db);
    MYSQL_STMT *st;
    MYSQL_BIND  b[2];
    unsigned long long vTo = toPool, vFrom = fromPool;
    solariStatus rc;

    if (n) *n = 0;
    if (!conn || fromPool == 0) return ERR_INVALID_ARG;
    rc = dbStmtPrepare(conn, SQL, &st);
    if (rc != SOLARI_OK) return rc;
    if (toPool) dbBindU64(&b[0], &vTo);
    else {
        memset(&b[0], 0, sizeof b[0]);
        b[0].buffer_type = MYSQL_TYPE_NULL;
    }
    dbBindU64(&b[1], &vFrom);
    rc = dbStmtRunWrite(st, b, 2);
    if (rc == SOLARI_OK && n) *n = (size_t)mysql_stmt_affected_rows(st);
    mysql_stmt_close(st);
    return rc;
}

solariStatus serverDbDeletePool(serverDb *db, uint64_t poolId)
{
    static const char *SQL = "DELETE FROM pool WHERE poolId = ?";
    MYSQL      *conn = dbConn(db);
    MYSQL_STMT *st;
    MYSQL_BIND  b[1];
    unsigned long long vId = poolId;
    solariStatus rc;

    if (!conn || poolId == 0) return ERR_INVALID_ARG;
    rc = dbStmtPrepare(conn, SQL, &st);
    if (rc != SOLARI_OK) return rc;
    dbBindU64(&b[0], &vId);
    rc = dbStmtRunWrite(st, b, 1);
    mysql_stmt_close(st);
    return rc;
}

solariStatus serverDbSetGlobalConfig(serverDb *db, const char *configJson,
                                     const char *updatedBy, uint64_t *epoch)
{
    static const char *SQL =
        "INSERT INTO globalConfig (id, configBlob, epoch, updatedAt, updatedBy) "
        "VALUES (1, ?, 1, UTC_TIMESTAMP(), ?) "
        "ON DUPLICATE KEY UPDATE configBlob=VALUES(configBlob), "
        "  epoch=epoch+1, updatedAt=UTC_TIMESTAMP(), updatedBy=VALUES(updatedBy)";
    static const char *SEL = "SELECT epoch FROM globalConfig WHERE id=1";
    MYSQL      *conn = dbConn(db);
    MYSQL_STMT *st;
    MYSQL_BIND  b[2], out[1];
    unsigned long lCfg, lBy;
    unsigned long long vEpoch = 0;
    my_bool      isNull = 0;
    solariStatus rc;

    if (epoch) *epoch = 0;
    if (!conn || !configJson) return ERR_INVALID_ARG;
    rc = dbStmtPrepare(conn, SQL, &st);
    if (rc != SOLARI_OK) return rc;
    dbBindStr(&b[0], configJson, &lCfg);
    dbBindStr(&b[1], (updatedBy && updatedBy[0]) ? updatedBy : NULL, &lBy);
    rc = dbStmtRunWrite(st, b, 2);
    mysql_stmt_close(st);
    if (rc != SOLARI_OK) return rc;

    rc = dbStmtPrepare(conn, SEL, &st);
    if (rc != SOLARI_OK) return rc;
    if (mysql_stmt_execute(st) != 0) { mysql_stmt_close(st); return ERR_DB; }
    dbBindOutU64(&out[0], &vEpoch, &isNull);
    if (mysql_stmt_bind_result(st, out) == 0 && mysql_stmt_fetch(st) == 0 && epoch)
        *epoch = vEpoch;
    mysql_stmt_close(st);
    return SOLARI_OK;
}

solariStatus serverDbUpdateAlertRule(serverDb *db, const serverAlertRuleEdit *e)
{
    char        sql[512];
    int         n = 0;
    MYSQL      *conn = dbConn(db);
    MYSQL_STMT *st;
    MYSQL_BIND  b[8];
    int         nb = 0;
    /* stable storage for bound scalars */
    int         vEnabled = 0, vFor = 0;
    double      vThresh = 0.0;
    unsigned long long vId;
    unsigned long lOp, lSev, lMet, lScope;
    solariStatus rc;

    if (!conn || !e || e->ruleId <= 0) return ERR_INVALID_ARG;

    /* Build the SET clause from present fields only. Column names are fixed
     * literals (never from input), so this is injection-safe; values bind. */
    n += snprintf(sql + n, sizeof sql - n, "UPDATE alertRule SET ");
    {
        int first = 1;
        #define ADDSET(col) do { n += snprintf(sql+n, sizeof sql-n, "%s%s=?", first?"":", ", col); first=0; } while(0)
        if (e->hasEnabled)    ADDSET("enabled");
        if (e->hasThreshold)  ADDSET("threshold");
        if (e->hasForSeconds) ADDSET("forSeconds");
        if (e->hasOp)         ADDSET("op");
        if (e->hasSeverity)   ADDSET("severity");
        if (e->hasMetric)     ADDSET("metric");
        if (e->hasScope)      ADDSET("scope");
        #undef ADDSET
        if (first) return ERR_INVALID_ARG;   /* nothing to update */
    }
    n += snprintf(sql + n, sizeof sql - n, " WHERE ruleId=?");

    rc = dbStmtPrepare(conn, sql, &st);
    if (rc != SOLARI_OK) return rc;

    if (e->hasEnabled)    { vEnabled = e->enabled ? 1 : 0; dbBindI32(&b[nb++], &vEnabled); }
    if (e->hasThreshold)  { vThresh = e->threshold;        dbBindDouble(&b[nb++], &vThresh); }
    if (e->hasForSeconds) { vFor = e->forSeconds;          dbBindI32(&b[nb++], &vFor); }
    if (e->hasOp)         dbBindStr(&b[nb++], e->op, &lOp);
    if (e->hasSeverity)   dbBindStr(&b[nb++], e->severity, &lSev);
    if (e->hasMetric)     dbBindStr(&b[nb++], e->metric, &lMet);
    if (e->hasScope)      dbBindStr(&b[nb++], e->scope, &lScope);
    vId = (unsigned long long)e->ruleId;
    dbBindU64(&b[nb++], &vId);

    rc = dbStmtRunWrite(st, b, nb);
    mysql_stmt_close(st);
    return rc;
}

solariStatus serverDbDeleteAlertRule(serverDb *db, uint64_t ruleId)
{
    static const char *SQL = "DELETE FROM alertRule WHERE ruleId = ?";
    MYSQL      *conn = dbConn(db);
    MYSQL_STMT *st;
    MYSQL_BIND  b[1];
    unsigned long long vId = ruleId;
    solariStatus rc;

    if (!conn || ruleId == 0) return ERR_INVALID_ARG;
    rc = dbStmtPrepare(conn, SQL, &st);
    if (rc != SOLARI_OK) return rc;
    dbBindU64(&b[0], &vId);
    rc = dbStmtRunWrite(st, b, 1);
    mysql_stmt_close(st);
    return rc;
}

/* ===================================================================== */
/* report persistence (§9.1, §10)                                         */
/* ===================================================================== */

/* Compute the average per-core CPU load (milli) across the reported cores;
 * returns 0 when no cores were reported. Pure helper. */
static unsigned dbAvgCpuMilli(const solariClientReport *rep)
{
    unsigned long long sum = 0;
    uint8_t i;
    if (!rep || rep->coreCount == 0) return 0;
    for (i = 0; i < rep->coreCount && i < SOLARI_MAX_CORES; i++)
        sum += rep->cpuLoadMilli[i];
    return (unsigned)(sum / rep->coreCount);
}

/* Compute the minimum free-disk percentage across reported mounts (0..100);
 * returns 100 when no disks were reported. Pure helper. */
static int dbMinDiskFreePct(const solariClientReport *rep)
{
    int minPct = 100;
    uint8_t i;
    if (!rep) return 100;
    for (i = 0; i < rep->diskCount && i < SOLARI_MAX_DISKS; i++) {
        const solariDiskEntry *d = &rep->disks[i];
        int pct;
        if (d->totalKb == 0) continue;
        pct = (int)((d->freeKb * 100ULL) / d->totalKb);
        if (pct < minPct) minPct = pct;
    }
    return minPct;
}

/* Aggregate network throughput across reported interfaces (sum of rx+tx Kbps).
 * The agent already reports per-iface rates; this reduces them to one scalar for
 * the hostHistory net trend. Pure helper; 0 when no ifaces reported. */
static unsigned long long dbSumNetKbps(const solariClientReport *rep)
{
    unsigned long long sum = 0;
    uint8_t i;
    if (!rep) return 0;
    for (i = 0; i < rep->ifaceCount && i < SOLARI_MAX_IFACES; i++)
        sum += rep->ifaces[i].rxKbps + rep->ifaces[i].txKbps;
    return sum;
}

/* Append a JSON-escaped string (no surrounding quotes) into dst at *off. */
static void dbJsonEsc(char *dst, size_t cap, size_t *off, const char *s)
{
    size_t o = *off;
    for (; s && *s && o + 7 < cap; s++) {
        unsigned char c = (unsigned char)*s;
        if (c == '"' || c == '\\') { dst[o++] = '\\'; dst[o++] = (char)c; }
        else if (c < 0x20)         { o += (size_t)snprintf(dst + o, cap - o, "\\u%04x", c); }
        else                        dst[o++] = (char)c;
    }
    *off = o;
}

/* per-core load: [m0,m1,...] (milli). */
static void dbJsonCpu(const solariClientReport *r, char *buf, size_t cap)
{
    size_t o = 1; uint8_t i;
    buf[0] = '[';
    for (i = 0; i < r->coreCount && i < SOLARI_MAX_CORES && o < cap - 16; i++)
        o += (size_t)snprintf(buf + o, cap - o, "%s%u", i ? "," : "", (unsigned)r->cpuLoadMilli[i]);
    snprintf(buf + o, cap - o, "]");
}

/* disks: [{"mount":..,"totalKb":..,"usedKb":..},...]. */
static void dbJsonDisks(const solariClientReport *r, char *buf, size_t cap)
{
    size_t o = 1; uint8_t i;
    buf[0] = '[';
    for (i = 0; i < r->diskCount && i < SOLARI_MAX_DISKS && o < cap - 160; i++) {
        const solariDiskEntry *d = &r->disks[i];
        unsigned long long used = d->totalKb > d->freeKb ? d->totalKb - d->freeKb : 0ULL;
        o += (size_t)snprintf(buf + o, cap - o, "%s{\"mount\":\"", i ? "," : "");
        dbJsonEsc(buf, cap, &o, d->mount);
        o += (size_t)snprintf(buf + o, cap - o, "\",\"totalKb\":%llu,\"usedKb\":%llu}",
                              (unsigned long long)d->totalKb, used);
    }
    snprintf(buf + o, cap - o, "]");
}

/* ifaces: [{"name":..,"rxKbps":..,"txKbps":..,"speedMbps":..},...]. */
static void dbJsonIfaces(const solariClientReport *r, char *buf, size_t cap)
{
    size_t o = 1; uint8_t i;
    buf[0] = '[';
    for (i = 0; i < r->ifaceCount && i < SOLARI_MAX_IFACES && o < cap - 160; i++) {
        const solariIfaceEntry *f = &r->ifaces[i];
        o += (size_t)snprintf(buf + o, cap - o, "%s{\"name\":\"", i ? "," : "");
        dbJsonEsc(buf, cap, &o, f->name);
        o += (size_t)snprintf(buf + o, cap - o,
                              "\",\"rxKbps\":%llu,\"txKbps\":%llu,\"speedMbps\":%llu}",
                              (unsigned long long)f->rxKbps, (unsigned long long)f->txKbps,
                              (unsigned long long)(f->capacityKbps / 1000ULL));
    }
    snprintf(buf + o, cap - o, "]");
}

/* Refresh procCurrent for a node within the open txn: clear then re-insert. */
static solariStatus dbWriteProcs(MYSQL *conn, unsigned long long *vNode,
                                 const solariClientReport *rep)
{
    static const char *SQL_INS =
        "INSERT INTO procCurrent "
        "  (nodeId, procName, pid, runState, nFiles, nSockets, rssKb, sampledAt) "
        "VALUES (?, ?, ?, ?, ?, ?, ?, UTC_TIMESTAMP())";
    MYSQL_STMT *st;
    MYSQL_BIND  b[7];
    uint8_t     i;
    solariStatus rc;

    rc = dbStmtPrepare(conn, "DELETE FROM procCurrent WHERE nodeId=?", &st);
    if (rc != SOLARI_OK) return rc;
    dbBindU64(&b[0], vNode);
    rc = dbStmtRunWrite(st, b, 1);
    mysql_stmt_close(st);
    if (rc != SOLARI_OK) return rc;

    for (i = 0; i < rep->procCount && i < SOLARI_MAX_PROCS; i++) {
        const solariProcEntry *p = &rep->procs[i];
        char rstate[2]; int vpid, vnf, vns; unsigned long long vrss;
        unsigned long lname, lstate;
        rstate[0] = p->state ? (char)p->state : '?'; rstate[1] = '\0';
        vpid = p->pid; vnf = (int)p->nFiles; vns = (int)p->nSockets; vrss = p->rssKb;
        rc = dbStmtPrepare(conn, SQL_INS, &st);
        if (rc != SOLARI_OK) return rc;
        dbBindU64(&b[0], vNode);
        dbBindStr(&b[1], p->name, &lname);
        dbBindI32(&b[2], &vpid);
        dbBindStr(&b[3], rstate, &lstate);
        dbBindI32(&b[4], &vnf);
        dbBindI32(&b[5], &vns);
        dbBindU64(&b[6], &vrss);
        rc = dbStmtRunWrite(st, b, 7);
        mysql_stmt_close(st);
        if (rc != SOLARI_OK) return rc;
    }
    return SOLARI_OK;
}

/* Reserve a receipt inside the report transaction. A duplicate never reaches
 * history writes; identity reuse with different content fails closed. */
static solariStatus dbReportBegin(MYSQL *conn)
{
    return dbTxnBegin(conn);
}

static solariStatus dbReportFinish(serverDb *db, MYSQL *conn, bool commit,
                                   solariStatus outcome)
{
    return dbTxnFinish(db, conn, commit, outcome);
}

static solariStatus dbReportReceipt(MYSQL *conn, uint64_t nodeId,
                                     const serverReportReceipt *receipt)
{
    static const char *insertSql =
        "INSERT INTO reportReceipt (nodeId,sessionId,seqNo,msgType,payloadHash) "
        "VALUES (?,UNHEX(?),?,?,UNHEX(?))";
    static const char *matchSql =
        "SELECT payloadHash=UNHEX(?) FROM reportReceipt "
        "WHERE nodeId=? AND sessionId=UNHEX(?) AND seqNo=? AND msgType=?";
    MYSQL_STMT *stmt;
    MYSQL_BIND binds[5], result;
    unsigned long long node = nodeId, seq = receipt->seqNo;
    unsigned long sessionLen, hashLen;
    int type = receipt->msgType, matches = 0;
    my_bool isNull = 0;
    char sessionHex[33], hashHex[65];
    unsigned error;
    solariStatus rc;
    solariHexEncode(receipt->session, sizeof receipt->session, sessionHex, sizeof sessionHex);
    solariHexEncode(receipt->payloadHash, sizeof receipt->payloadHash, hashHex, sizeof hashHex);
    rc = dbStmtPrepare(conn, insertSql, &stmt);
    if (rc != SOLARI_OK) return rc;
    dbBindU64(&binds[0], &node);
    dbBindStr(&binds[1], sessionHex, &sessionLen);
    dbBindU64(&binds[2], &seq);
    dbBindI32(&binds[3], &type);
    dbBindStr(&binds[4], hashHex, &hashLen);
    if (mysql_stmt_bind_param(stmt, binds) != 0) { mysql_stmt_close(stmt); return ERR_DB; }
    if (mysql_stmt_execute(stmt) == 0) { mysql_stmt_close(stmt); return SOLARI_OK; }
    error = mysql_stmt_errno(stmt);
    mysql_stmt_close(stmt);
    if (error != 1062) return ERR_DB;
    rc = dbStmtPrepare(conn, matchSql, &stmt);
    if (rc != SOLARI_OK) return rc;
    dbBindStr(&binds[0], hashHex, &hashLen);
    dbBindU64(&binds[1], &node);
    dbBindStr(&binds[2], sessionHex, &sessionLen);
    dbBindU64(&binds[3], &seq);
    dbBindI32(&binds[4], &type);
    if (mysql_stmt_bind_param(stmt, binds) != 0 || mysql_stmt_execute(stmt) != 0) {
        mysql_stmt_close(stmt); return ERR_DB;
    }
    dbBindOutI32(&result, &matches, &isNull);
    if (mysql_stmt_bind_result(stmt, &result) != 0 || mysql_stmt_fetch(stmt) != 0 || isNull) {
        mysql_stmt_close(stmt); return ERR_DB;
    }
    mysql_stmt_close(stmt);
    return matches ? ERR_CONN_RETRY : ERR_INVALID_ARG;
}

solariStatus serverDbWriteClientReport(serverContext *ctx, uint64_t nodeId,
                                       const solariClientReport *rep,
                                       const serverReportReceipt *receipt)
{
    static const char *SQL_CUR =
        "INSERT INTO hostCurrent "
        "  (nodeId, sampledAt, cpuLoadMilli, ramUsedKb, ramTotalKb, "
        "   swapUsedKb, swapTotalKb, disks, ifaces, usbBuses, "
        "   fsReadonlyCount, blockDevMissing, smartFailCount, failedUnitCount, "
        "   dmesgCritCount, fsReadonlyList, smartFailList, failedUnitList, "
        "   dmesgCritSample) "
        "VALUES (?, UTC_TIMESTAMP(), ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?) "
        "ON DUPLICATE KEY UPDATE "
        "  sampledAt=UTC_TIMESTAMP(), cpuLoadMilli=VALUES(cpuLoadMilli), "
        "  ramUsedKb=VALUES(ramUsedKb), ramTotalKb=VALUES(ramTotalKb), "
        "  swapUsedKb=VALUES(swapUsedKb), swapTotalKb=VALUES(swapTotalKb), "
        "  disks=VALUES(disks), ifaces=VALUES(ifaces), usbBuses=VALUES(usbBuses), "
        "  fsReadonlyCount=VALUES(fsReadonlyCount), "
        "  blockDevMissing=VALUES(blockDevMissing), "
        "  smartFailCount=VALUES(smartFailCount), "
        "  failedUnitCount=VALUES(failedUnitCount), "
        "  dmesgCritCount=VALUES(dmesgCritCount), "
        "  fsReadonlyList=VALUES(fsReadonlyList), "
        "  smartFailList=VALUES(smartFailList), "
        "  failedUnitList=VALUES(failedUnitList), "
        "  dmesgCritSample=VALUES(dmesgCritSample)";
    static const char *SQL_HIST =
        "INSERT INTO hostHistory "
        "  (nodeId, sampledAt, cpuAvgMilli, ramUsedKb, swapUsedKb, diskMinFreePct, netKbps, "
        "   fsReadonlyCount, blockDevMissing, smartFailCount, failedUnitCount, dmesgCritCount) "
        "VALUES (?, UTC_TIMESTAMP(), ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)";
    MYSQL      *conn;
    MYSQL_STMT *st;
    MYSQL_BIND  b[20];
    unsigned long long vNode, vRamU, vRamT, vSwapU, vSwapT, vNet;
    unsigned long long vFsRo, vBlockMissing, vSmartFail, vFailedUnits, vDmesgCrit;
    int cpuAvg, diskMin;
    unsigned long lCpu, lDisks, lIfaces, lUsb;
    unsigned long lFsRoList, lSmartFailList, lFailedUnitList, lDmesgCritSample;
    char cpuJson[512], disksJson[4096], ifacesJson[2048];
    static const char *usbJson = "[]";
    solariStatus rc;

    if (!ctx || !ctx->db || !rep || !receipt ||
        receipt->msgType != SCP_MSG_CLIENT_REPORT) return ERR_INVALID_ARG;
    conn = dbConn(ctx->db);
    if (!conn) return ERR_DB;

    vNode  = nodeId;
    vRamU  = rep->ramUsedKb;  vRamT = rep->ramTotalKb;
    vSwapU = rep->swapUsedKb; vSwapT = rep->swapTotalKb;
    vFsRo = rep->health.fsReadonlyCount;
    vBlockMissing = rep->health.blockDevMissing;
    vSmartFail = rep->health.smartFailCount;
    vFailedUnits = rep->health.failedUnitCount;
    vDmesgCrit = rep->health.dmesgCritCount;
    cpuAvg = (int)dbAvgCpuMilli(rep);
    diskMin = dbMinDiskFreePct(rep);
    vNet = dbSumNetKbps(rep);
    dbJsonCpu(rep, cpuJson, sizeof cpuJson);
    dbJsonDisks(rep, disksJson, sizeof disksJson);
    dbJsonIfaces(rep, ifacesJson, sizeof ifacesJson);

    /* One transaction: upsert current (+ JSON detail) + procs + history (§9.1). */
    if (dbReportBegin(conn) != SOLARI_OK) return ERR_DB;

    rc = receipt->legacy ? SOLARI_OK : dbReportReceipt(conn, nodeId, receipt);
    if (rc != SOLARI_OK) return dbReportFinish(ctx->db, conn, false, rc);

    rc = dbStmtPrepare(conn, SQL_CUR, &st);
    if (rc != SOLARI_OK) return dbReportFinish(ctx->db, conn, false, rc);
    dbBindU64(&b[0], &vNode);
    dbBindStr(&b[1], cpuJson,   &lCpu);
    dbBindU64(&b[2], &vRamU);
    dbBindU64(&b[3], &vRamT);
    dbBindU64(&b[4], &vSwapU);
    dbBindU64(&b[5], &vSwapT);
    dbBindStr(&b[6], disksJson,  &lDisks);
    dbBindStr(&b[7], ifacesJson, &lIfaces);
    dbBindStr(&b[8], usbJson,    &lUsb);
    dbBindU64(&b[9], &vFsRo);
    dbBindU64(&b[10], &vBlockMissing);
    dbBindU64(&b[11], &vSmartFail);
    dbBindU64(&b[12], &vFailedUnits);
    dbBindU64(&b[13], &vDmesgCrit);
    dbBindStr(&b[14], rep->health.fsReadonlyList,  &lFsRoList);
    dbBindStr(&b[15], rep->health.smartFailList,   &lSmartFailList);
    dbBindStr(&b[16], rep->health.failedUnitList,  &lFailedUnitList);
    dbBindStr(&b[17], rep->health.dmesgCritSample, &lDmesgCritSample);
    rc = dbStmtRunWrite(st, b, 18);
    mysql_stmt_close(st);
    if (rc != SOLARI_OK) return dbReportFinish(ctx->db, conn, false, rc);

    rc = dbWriteProcs(conn, &vNode, rep);
    if (rc != SOLARI_OK) return dbReportFinish(ctx->db, conn, false, rc);

    rc = dbStmtPrepare(conn, SQL_HIST, &st);
    if (rc != SOLARI_OK) return dbReportFinish(ctx->db, conn, false, rc);
    dbBindU64(&b[0], &vNode);
    dbBindI32(&b[1], &cpuAvg);
    dbBindU64(&b[2], &vRamU);
    dbBindU64(&b[3], &vSwapU);
    dbBindI32(&b[4], &diskMin);
    dbBindU64(&b[5], &vNet);
    dbBindU64(&b[6], &vFsRo);
    dbBindU64(&b[7], &vBlockMissing);
    dbBindU64(&b[8], &vSmartFail);
    dbBindU64(&b[9], &vFailedUnits);
    dbBindU64(&b[10], &vDmesgCrit);
    rc = dbStmtRunWrite(st, b, 11);
    mysql_stmt_close(st);
    if (rc != SOLARI_OK) return dbReportFinish(ctx->db, conn, false, rc);

    rc = dbReportFinish(ctx->db, conn, true, SOLARI_OK);
    if (rc != SOLARI_OK) {
        solariLogf(SOLARI_LOG_ERROR,
                   "serverDb: client report transaction failed during commit/reset");
        return ERR_DB;
    }
    return SOLARI_OK;
}

solariStatus serverDbWriteMonitorReport(serverContext *ctx, uint64_t nodeId,
                                        const solariMonitorReport *rep,
                                        const serverReportReceipt *receipt)
{
    static const char *SQL_CUR =
        "INSERT INTO probeCurrent "
        "  (targetId, monitorNode, outcome, rttMicros, jitterMicros, "
        "   lossPermille, throughputKbps, sampledAt) "
        "VALUES (?, ?, ?, ?, ?, ?, ?, UTC_TIMESTAMP()) "
        "ON DUPLICATE KEY UPDATE "
        "  outcome=VALUES(outcome), rttMicros=VALUES(rttMicros), "
        "  jitterMicros=VALUES(jitterMicros), lossPermille=VALUES(lossPermille), "
        "  throughputKbps=VALUES(throughputKbps), sampledAt=UTC_TIMESTAMP()";
    static const char *SQL_HIST =
        "INSERT INTO probeHistory "
        "  (targetId, monitorNode, outcome, rttMicros, lossPermille, sampledAt) "
        "VALUES (?, ?, ?, ?, ?, UTC_TIMESTAMP())";
    MYSQL      *conn;
    MYSQL_STMT *stCur, *stHist;
    unsigned long long vNode = nodeId;
    uint16_t    i;
    solariStatus rc = SOLARI_OK;

    if (!ctx || !ctx->db || !rep || !receipt ||
        (receipt->msgType != SCP_MSG_MONITOR_REPORT &&
         receipt->msgType != SCP_MSG_SURVEY_RESP)) return ERR_INVALID_ARG;
    conn = dbConn(ctx->db);
    if (!conn) return ERR_DB;

    if (dbReportBegin(conn) != SOLARI_OK) return ERR_DB;

    rc = receipt->legacy ? SOLARI_OK : dbReportReceipt(conn, nodeId, receipt);
    if (rc != SOLARI_OK) goto fail_nostmt;

    rc = dbStmtPrepare(conn, SQL_CUR, &stCur);
    if (rc != SOLARI_OK) goto fail_nostmt;
    rc = dbStmtPrepare(conn, SQL_HIST, &stHist);
    if (rc != SOLARI_OK) { mysql_stmt_close(stCur); goto fail_nostmt; }

    for (i = 0; i < rep->probeCount && i < SOLARI_MAX_PROBES; i++) {
        const solariProbeResult *p = &rep->probes[i];
        char targetId[SERVER_TARGETID_MAX];
        const char *outcome = dbOutcomeStr(p->outcome);
        MYSQL_BIND  bc[7], bh[5];
        unsigned long tLen, oLen, tLen2, oLen2;
        int vRtt = (int)p->rttMicros, vJit = (int)p->jitterMicros;
        int vLoss = (int)p->lossPermille, vThru = (int)p->throughputKbps;

        dbProbeTargetId(p, targetId, sizeof targetId);

        dbBindStr(&bc[0], targetId, &tLen);
        dbBindU64(&bc[1], &vNode);
        dbBindStr(&bc[2], outcome, &oLen);
        dbBindI32(&bc[3], &vRtt);
        dbBindI32(&bc[4], &vJit);
        dbBindI32(&bc[5], &vLoss);
        dbBindI32(&bc[6], &vThru);
        rc = dbStmtRunWrite(stCur, bc, 7);
        if (rc != SOLARI_OK) break;

        dbBindStr(&bh[0], targetId, &tLen2);
        dbBindU64(&bh[1], &vNode);
        dbBindStr(&bh[2], outcome, &oLen2);
        dbBindI32(&bh[3], &vRtt);
        dbBindI32(&bh[4], &vLoss);
        rc = dbStmtRunWrite(stHist, bh, 5);
        if (rc != SOLARI_OK) break;
    }

    mysql_stmt_close(stCur);
    mysql_stmt_close(stHist);

    if (rc != SOLARI_OK) return dbReportFinish(ctx->db, conn, false, rc);
    rc = dbReportFinish(ctx->db, conn, true, SOLARI_OK);
    if (rc != SOLARI_OK) {
        solariLogf(SOLARI_LOG_ERROR,
                   "serverDb: monitor report transaction failed during commit/reset");
        return ERR_DB;
    }
    return SOLARI_OK;

fail_nostmt:
    return dbReportFinish(ctx->db, conn, false, rc);
}

/* ===================================================================== */
/* alerts / audit (§10 alertEvent, alertRule)                             */
/* ===================================================================== */

solariStatus serverDbWriteAlertEvent(serverDb *db, int ruleId, uint64_t nodeId,
                                     const char *targetId, const char *severity,
                                     const char *detail, uint64_t firedUnixMs,
                                     const char *eventKind, uint64_t openedEventId,
                                     uint64_t assetId, int effectiveTier,
                                     const char *disposition, uint64_t *eventId)
{
    static const char *SQL =
        "INSERT INTO alertEvent "
        "  (ruleId, nodeId, targetId, firedAt, severity, detail, eventKind, openedEventId, assetId, effectiveTier, disposition) "
        "VALUES (?, ?, ?, FROM_UNIXTIME(? / 1000), ?, ?, ?, ?, ?, ?, ?)";
    static const char *SQL_CLEARED =
        "INSERT INTO alertEvent "
        "  (ruleId, nodeId, targetId, firedAt, clearedAt, severity, detail, eventKind, openedEventId, assetId, effectiveTier, disposition) "
        "VALUES (?, ?, ?, FROM_UNIXTIME(? / 1000), UTC_TIMESTAMP(), ?, ?, ?, ?, ?, ?, ?)";
    MYSQL      *conn = dbConn(db);
    MYSQL_STMT *st;
    MYSQL_BIND  b[11];
    int                vRule = ruleId;
    unsigned long long vNode = nodeId;
    unsigned long long vWhen = firedUnixMs ? firedUnixMs : solariNowUnixMs();
    unsigned long long vOpened = openedEventId, vAsset = assetId;
    unsigned long tLen, sLen, dLen, kLen, pLen;
    solariStatus rc;

    if (eventId) *eventId = 0;
    if (!conn || !eventKind || !disposition) return ERR_INVALID_ARG;
    rc = dbStmtPrepare(conn, strcmp(eventKind, "cleared") == 0 ? SQL_CLEARED : SQL, &st);
    if (rc != SOLARI_OK) return rc;

    /* ruleId 0 = synthetic/audit row -> store SQL NULL so it is not a dangling
     * FK to a real rule. */
    if (ruleId == 0) {
        memset(&b[0], 0, sizeof b[0]);
        b[0].buffer_type = MYSQL_TYPE_NULL;
    } else {
        dbBindI32(&b[0], &vRule);
    }
    dbBindU64(&b[1], &vNode);
    dbBindStr(&b[2], (targetId && targetId[0]) ? targetId : NULL, &tLen);
    dbBindU64(&b[3], &vWhen);
    dbBindStr(&b[4], (severity && severity[0]) ? severity : NULL, &sLen);
    dbBindStr(&b[5], (detail && detail[0]) ? detail : NULL, &dLen);
    dbBindStr(&b[6], eventKind, &kLen);
    if (openedEventId) dbBindU64(&b[7], &vOpened); else dbBindStr(&b[7], NULL, &pLen);
    if (assetId) dbBindU64(&b[8], &vAsset); else dbBindStr(&b[8], NULL, &pLen);
    dbBindI32(&b[9], &effectiveTier);
    dbBindStr(&b[10], disposition, &pLen);

    rc = dbStmtRunWrite(st, b, 11);
    if (rc == SOLARI_OK && eventId) *eventId = (uint64_t)mysql_insert_id(conn);
    mysql_stmt_close(st);
    return rc;
}

solariStatus serverDbAckAlertEvent(serverDb *db, uint64_t eventId)
{
    static const char *SQL =
        "UPDATE alertEvent SET clearedAt = COALESCE(clearedAt, UTC_TIMESTAMP()) "
        "WHERE eventId = ?";
    MYSQL      *conn = dbConn(db);
    MYSQL_STMT *st;
    MYSQL_BIND  b[1];
    unsigned long long vId = eventId;
    solariStatus rc;

    if (!conn || eventId == 0) return ERR_INVALID_ARG;
    rc = dbStmtPrepare(conn, SQL, &st);
    if (rc != SOLARI_OK) return rc;
    dbBindU64(&b[0], &vId);
    rc = dbStmtRunWrite(st, b, 1);
    mysql_stmt_close(st);
    return rc;
}

/* ===================================================================== */
/* Privileged control-verb request queue (Task #1 — PHP→host boundary)    */
/* ===================================================================== */
/*
 * The ctlRequest table (db/migrations/020_ctl_request_queue.sql) is the
 * hand-off queue between the unprivileged dashboard peer and the privileged
 * server process. The dashboard is REFUSED every PRIVILEGED verb at the socket
 * (SO_PEERCRED + the verb-class ACL in solariCtl.c); instead it Submits a row
 * here, and the in-process consumer (serverCtlQueuePoll) Claims + executes it
 * with operator authority, then records the result. These four functions are
 * that queue's entire data-access surface; all state lives in the server's own
 * monitoring DB, co-located with its single consumer.
 */

/* serverDbCtlRequestSubmit — enqueue one privileged request from the dashboard.
 *
 * Input:  verb        inner privileged verb name (already validated by caller)
 *         argsWire    inner verb args in ctl line-protocol form (may be "")
 *         requestedBy authenticated operator string from the PHP session
 *         peerUid     SO_PEERCRED uid of the submitter (audit)
 *         idemKey     optional client idempotency key (NULL = unkeyed)
 *         reqIdOut    receives the row id (new, or the existing one on replay)
 * Output: SOLARI_OK with *reqIdOut set. A repeated idemKey collides on the
 *         UNIQUE index (errno 1062); we treat that as success and return the
 *         id of the row already queued, so a resubmit is idempotent rather than
 *         an error. ERR_DB on any other failure.
 */
solariStatus serverDbCtlRequestSubmit(serverDb *db, const char *verb,
        const char *argsWire, const char *requestedBy,
        uint32_t peerUid, const char *idemKey, unsigned long long *reqIdOut)
{
    static const char *SQL =
        "INSERT INTO ctlRequest (verb, argsWire, requestedBy, peerUid, "
        "  idempotencyKey, state, createdAt) "
        "VALUES (?, ?, ?, ?, ?, 'pending', UTC_TIMESTAMP(6))";
    MYSQL      *conn = dbConn(db);
    MYSQL_STMT *st;
    MYSQL_BIND  b[5];
    unsigned long long vPeer = peerUid;
    unsigned long lV, lA, lR, lK;
    solariStatus rc;

    if (reqIdOut) *reqIdOut = 0;
    if (!conn || !verb || !verb[0] || !requestedBy || !requestedBy[0])
        return ERR_INVALID_ARG;
    rc = dbStmtPrepare(conn, SQL, &st);
    if (rc != SOLARI_OK) return rc;
    dbBindStr(&b[0], verb, &lV);
    dbBindStr(&b[1], argsWire ? argsWire : "", &lA);
    dbBindStr(&b[2], requestedBy, &lR);
    dbBindU64(&b[3], &vPeer);
    dbBindStr(&b[4], (idemKey && idemKey[0]) ? idemKey : NULL, &lK);
    if (mysql_stmt_bind_param(st, b) != 0) { mysql_stmt_close(st); return ERR_DB; }
    if (mysql_stmt_execute(st) != 0) {
        unsigned int e = mysql_stmt_errno(st);
        mysql_stmt_close(st);
        /* Duplicate idempotency key: replay only an identical request. A key
         * reused with different arguments must fail closed; otherwise the API
         * could pair the original request id with the caller's new payload. */
        if (e == 1062 && idemKey && idemKey[0]) {
            /* Scope the lookup to (requestedBy, idemKey) to match the composite
             * UNIQUE key (review F10): a global key let one requester pre-seed a
             * predictable key so another's submit silently returned the first
             * one's request id. Keys now only collide within the same requester. */
            static const char *SEL =
                "SELECT id, verb, argsWire FROM ctlRequest "
                "WHERE requestedBy = ? AND idempotencyKey = ?";
            MYSQL_STMT *sel;
            MYSQL_BIND  in[2], out[3];
            unsigned long long vId = 0;
            char existingVerb[32], existingArgs[16384];
            unsigned long lReqSel, lKey, lVerb = 0, lArgs = 0;
            my_bool isNullId = 0, isNullVerb = 0, isNullArgs = 0;
            if (dbStmtPrepare(conn, SEL, &sel) != SOLARI_OK) return ERR_DB;
            dbBindStr(&in[0], requestedBy, &lReqSel);
            dbBindStr(&in[1], idemKey, &lKey);
            if (mysql_stmt_bind_param(sel, in) != 0 ||
                mysql_stmt_execute(sel) != 0) {
                mysql_stmt_close(sel); return ERR_DB;
            }
            dbBindOutU64(&out[0], &vId, &isNullId);
            dbBindOutStr(&out[1], existingVerb, sizeof existingVerb - 1,
                         &lVerb, &isNullVerb);
            dbBindOutStr(&out[2], existingArgs, sizeof existingArgs - 1,
                         &lArgs, &isNullArgs);
            if (mysql_stmt_bind_result(sel, out) != 0) {
                mysql_stmt_close(sel); return ERR_DB;
            }
            int fetchRc = mysql_stmt_fetch(sel);
            if (fetchRc == 0 && !isNullId && !isNullVerb && !isNullArgs) {
                existingVerb[lVerb < sizeof existingVerb ? lVerb : sizeof existingVerb - 1] = '\0';
                existingArgs[lArgs < sizeof existingArgs ? lArgs : sizeof existingArgs - 1] = '\0';
                if (strcmp(existingVerb, verb) != 0 || strcmp(existingArgs, argsWire ? argsWire : "") != 0) {
                    mysql_stmt_close(sel);
                    return ERR_INVALID_ARG;
                }
                if (reqIdOut) *reqIdOut = vId;
            }
            mysql_stmt_close(sel);
            if (fetchRc == MYSQL_DATA_TRUNCATED) return ERR_INVALID_ARG;
            return (reqIdOut && *reqIdOut) ? SOLARI_OK : ERR_DB;
        }
        solariLogf(SOLARI_LOG_ERROR, "serverDb: ctlRequest submit failed (errno %u)", e);
        return ERR_DB;
    }
    if (reqIdOut) *reqIdOut = (unsigned long long)mysql_stmt_insert_id(st);
    mysql_stmt_close(st);
    return SOLARI_OK;
}

/* serverDbCtlRequestClaim — atomically claim the oldest pending request.
 *
 * The claim is a compare-and-set: UPDATE the oldest pending row to 'claimed'
 * only while it is still 'pending'. mysql_stmt_affected_rows() == 1 means this
 * caller won it; 0 means another claimer took it (or the queue is empty). On a
 * win we SELECT the claimed row's verb + args for replay.
 *
 * Input:  claimedBy  consumer label (audit)
 * Output: *reqIdOut, verbOut, argsWireOut for the won row.
 *         SOLARI_OK   a row was claimed.
 *         ERR_TLV_END the queue is empty / nothing was claimed (not an error).
 *         ERR_DB      on failure.
 */
solariStatus serverDbCtlRequestClaim(serverDb *db, const char *claimedBy,
        unsigned long long *reqIdOut, char *verbOut, size_t verbCap,
        char *argsWireOut, size_t argsCap,
        char *requestedByOut, size_t requestedByCap)
{
    /* Target the single oldest pending row by id; the WHERE state='pending'
     * predicate is the CAS — two claimers racing the same row, only one UPDATE
     * changes it. ORDER BY id LIMIT 1 keeps it FIFO and touches one row.
     *
     * id=LAST_INSERT_ID(id) stamps THIS row's id into the connection-local
     * LAST_INSERT_ID() as a side effect of the winning UPDATE, so we can fetch
     * exactly the row we just claimed by its id (cross-lab review F5). The prior
     * "SELECT ... WHERE claimedBy=? ORDER BY claimedAt DESC" correlated the row
     * only by the (nonunique) consumer label + recency: two same-label claimers
     * racing could both read the newer row and double-execute it while the older
     * stayed orphaned in 'claimed'. LAST_INSERT_ID is per-connection, so it is
     * immune to that race regardless of how many consumers share a label. */
    static const char *UPD =
        "UPDATE ctlRequest SET state='claimed', claimedBy=?, attempts=attempts+1, "
        "  claimedAt=UTC_TIMESTAMP(6), id=LAST_INSERT_ID(id) "
        "WHERE state='pending' ORDER BY id LIMIT 1";
    static const char *SEL =
        "SELECT id, verb, argsWire, requestedBy FROM ctlRequest WHERE id=?";
    MYSQL      *conn = dbConn(db);
    MYSQL_STMT *st;
    MYSQL_BIND  b[1];
    unsigned long lBy;
    unsigned long long claimedId;
    solariStatus rc;

    if (reqIdOut) *reqIdOut = 0;
    if (verbOut && verbCap) verbOut[0] = '\0';
    if (argsWireOut && argsCap) argsWireOut[0] = '\0';
    if (requestedByOut && requestedByCap) requestedByOut[0] = '\0';
    if (!conn || !claimedBy || !claimedBy[0] || !verbOut || !verbCap ||
        !argsWireOut || !argsCap)
        return ERR_INVALID_ARG;

    /* --- CAS claim --- */
    rc = dbStmtPrepare(conn, UPD, &st);
    if (rc != SOLARI_OK) return rc;
    dbBindStr(&b[0], claimedBy, &lBy);
    rc = dbStmtRunWrite(st, b, 1);
    if (rc != SOLARI_OK) { mysql_stmt_close(st); return rc; }
    if (mysql_stmt_affected_rows(st) == 0) { mysql_stmt_close(st); return ERR_TLV_END; }
    /* The winning UPDATE set LAST_INSERT_ID(id) to the claimed row's id. */
    claimedId = (unsigned long long)mysql_stmt_insert_id(st);
    mysql_stmt_close(st);

    /* --- fetch exactly the row we just claimed, by its id --- */
    {
        MYSQL_BIND in[1], out[4];
        unsigned long long vId = 0, vIdIn = claimedId;
        unsigned long lVerb = 0, lArgs = 0, lReq = 0;
        my_bool nId = 0, nVerb = 0, nArgs = 0, nReq = 0;
        int fetch;

        rc = dbStmtPrepare(conn, SEL, &st);
        if (rc != SOLARI_OK) return rc;
        dbBindU64(&in[0], &vIdIn);
        if (mysql_stmt_bind_param(st, in) != 0 || mysql_stmt_execute(st) != 0) {
            mysql_stmt_close(st); return ERR_DB;
        }
        dbBindOutU64(&out[0], &vId, &nId);
        dbBindOutStr(&out[1], verbOut, (unsigned long)verbCap, &lVerb, &nVerb);
        dbBindOutStr(&out[2], argsWireOut, (unsigned long)argsCap, &lArgs, &nArgs);
        dbBindOutStr(&out[3], requestedByOut ? requestedByOut : verbOut,
                     (unsigned long)(requestedByOut ? requestedByCap : 0),
                     &lReq, &nReq);
        if (mysql_stmt_bind_result(st, out) != 0) { mysql_stmt_close(st); return ERR_DB; }
        fetch = mysql_stmt_fetch(st);
        if (fetch != 0 && fetch != MYSQL_DATA_TRUNCATED) {
            /* Claimed a row but cannot read it back — leave it 'claimed' (its
             * attempts counter records the miss) and report empty so the caller
             * retries rather than replaying a half-read command. */
            mysql_stmt_close(st);
            return ERR_DB;
        }
        /* A truncated verb/args must NOT be replayed: the forced NUL below would
         * hand the operator-authority consumer a valid-looking PREFIX of an
         * attacker-oversized command (cross-lab review F7). Fail the row closed
         * so it can never execute, then report empty. verbOut/argsWire are sized
         * to the protocol cap, so any truncation here is anomalous input. */
        if ((!nVerb && lVerb >= verbCap) || (!nArgs && lArgs >= argsCap)) {
            mysql_stmt_close(st);
            (void)serverDbCtlRequestComplete(db, claimedId, false,
                                             "stored request exceeds replay buffer");
            if (reqIdOut) *reqIdOut = 0;
            return ERR_BUFFER_FULL;
        }
        if (reqIdOut && !nId) *reqIdOut = vId;
        verbOut[(!nVerb && lVerb < verbCap) ? lVerb : verbCap - 1] = '\0';
        argsWireOut[(!nArgs && lArgs < argsCap) ? lArgs : argsCap - 1] = '\0';
        if (requestedByOut && requestedByCap)
            requestedByOut[(!nReq && lReq < requestedByCap) ? lReq
                                                            : requestedByCap - 1] = '\0';
        mysql_stmt_close(st);
    }
    return SOLARI_OK;
}

/* serverDbCtlRequestComplete — record the terminal result of a claimed request.
 *
 * Input:  reqId       the claimed row
 *         ok          true → state='done' and resultText → result column;
 *                     false → state='failed' and resultText → error column.
 *         resultText  the inner verb reply (or failure reason); may be NULL.
 * Output: SOLARI_OK on write, ERR_DB on failure.
 */
solariStatus serverDbCtlRequestComplete(serverDb *db, unsigned long long reqId,
        bool ok, const char *resultText)
{
    /* One UPDATE for both outcomes: the ok flag routes the text into result vs
     * error and picks the state, and only ever advances a 'claimed' row so a
     * late/duplicate completion cannot resurrect or overwrite a settled one. */
    static const char *SQL =
        "UPDATE ctlRequest SET "
        "  state = IF(?, 'done', 'failed'), "
        "  result = IF(?, ?, result), "
        "  error  = IF(?, error, ?), "
        "  completedAt = UTC_TIMESTAMP(6) "
        "WHERE id = ? AND state = 'claimed'";
    MYSQL      *conn = dbConn(db);
    MYSQL_STMT *st;
    MYSQL_BIND  b[6];
    int okFlag = ok ? 1 : 0;
    unsigned long long vId = reqId;
    unsigned long lRes, lErr;
    solariStatus rc;

    if (!conn || reqId == 0) return ERR_INVALID_ARG;
    rc = dbStmtPrepare(conn, SQL, &st);
    if (rc != SOLARI_OK) return rc;
    dbBindI32(&b[0], &okFlag);                                   /* state IF */
    dbBindI32(&b[1], &okFlag);                                   /* result IF */
    dbBindStr(&b[2], (ok && resultText) ? resultText : NULL, &lRes);
    dbBindI32(&b[3], &okFlag);                                   /* error IF */
    dbBindStr(&b[4], (!ok && resultText) ? resultText : NULL, &lErr);
    dbBindU64(&b[5], &vId);
    rc = dbStmtRunWrite(st, b, 6);
    if (rc != SOLARI_OK) { mysql_stmt_close(st); return rc; }
    /* Exactly one 'claimed' row must have matched. Zero means the id was wrong or
     * the row was already settled/reclaimed — report it instead of the previous
     * unconditional SOLARI_OK, which hid lost or raced completions (review F8). */
    if (mysql_stmt_affected_rows(st) == 0) { mysql_stmt_close(st); return ERR_TLV_END; }
    mysql_stmt_close(st);
    return SOLARI_OK;
}

/* serverDbCtlRequestGet — read one request's state + terminal detail.
 *
 * Input:  reqId       the row to read
 * Output: stateOut    'pending'|'claimed'|'done'|'failed'
 *         detailOut   result (done) or error (failed) text; "" while in flight
 *         SOLARI_OK   row found. ERR_TLV_END if no such request. ERR_DB on fail.
 */
solariStatus serverDbCtlRequestGet(serverDb *db, unsigned long long reqId,
        const char *requestedBy,
        char *stateOut, size_t stateCap, char *detailOut, size_t detailCap)
{
    /* COALESCE picks whichever terminal column is populated; NULL→'' so the
     * caller always gets a clean string. */
    static const char *SQL =
        "SELECT state, COALESCE(result, error, '') FROM ctlRequest WHERE id = ?";
    static const char *SQL_SCOPED =
        "SELECT state, COALESCE(result, error, '') FROM ctlRequest "
        "WHERE id = ? AND requestedBy = ?";
    MYSQL      *conn = dbConn(db);
    MYSQL_STMT *st;
    MYSQL_BIND  in[2], out[2];
    unsigned long long vId = reqId;
    unsigned long lState = 0, lDetail = 0, lUser = 0;
    my_bool nState = 0, nDetail = 0;
    int fetch;
    solariStatus rc;

    if (stateOut && stateCap) stateOut[0] = '\0';
    if (detailOut && detailCap) detailOut[0] = '\0';
    if (!conn || reqId == 0 || !stateOut || !stateCap || !detailOut || !detailCap)
        return ERR_INVALID_ARG;
    if (requestedBy && (!requestedBy[0] || strlen(requestedBy) > 128))
        return ERR_INVALID_ARG;
    rc = dbStmtPrepare(conn, requestedBy ? SQL_SCOPED : SQL, &st);
    if (rc != SOLARI_OK) return rc;
    dbBindU64(&in[0], &vId);
    if (requestedBy) dbBindStr(&in[1], requestedBy, &lUser);
    if (mysql_stmt_bind_param(st, in) != 0 || mysql_stmt_execute(st) != 0) {
        mysql_stmt_close(st); return ERR_DB;
    }
    dbBindOutStr(&out[0], stateOut, (unsigned long)stateCap, &lState, &nState);
    dbBindOutStr(&out[1], detailOut, (unsigned long)detailCap, &lDetail, &nDetail);
    if (mysql_stmt_bind_result(st, out) != 0) { mysql_stmt_close(st); return ERR_DB; }
    fetch = mysql_stmt_fetch(st);
    if (fetch == MYSQL_NO_DATA) { mysql_stmt_close(st); return ERR_TLV_END; }
    if (fetch != 0 && fetch != MYSQL_DATA_TRUNCATED) { mysql_stmt_close(st); return ERR_DB; }
    stateOut[(!nState && lState < stateCap) ? lState : stateCap - 1] = '\0';
    detailOut[(!nDetail && lDetail < detailCap) ? lDetail : detailCap - 1] = '\0';
    mysql_stmt_close(st);
    return SOLARI_OK;
}

solariStatus serverDbLoadAlertRules(serverDb *db, const char *scope,
                                    serverAlertRule *out, size_t *count)
{
    static const char *SQL =
        "SELECT ruleId, scope, metric, op, threshold, forSeconds, severity "
        "FROM alertRule WHERE enabled = TRUE AND scope = ? ORDER BY ruleId";
    MYSQL      *conn = dbConn(db);
    MYSQL_STMT *st;
    MYSQL_BIND  pin[1], rout[7];
    unsigned long scopeLen;
    size_t cap, n = 0;
    solariStatus rc;
    /* result holders */
    int          rRuleId = 0, rForSec = 0;
    double       rThresh = 0.0;
    char         rScope[8], rMetric[64], rOp[12], rSeverity[8];
    unsigned long lScope, lMetric, lOp, lSeverity;
    my_bool      nThresh, nForSec;

    if (!out || !count) return ERR_INVALID_ARG;
    cap = *count;
    *count = 0;
    if (!conn || !scope) return ERR_INVALID_ARG;
    if (cap == 0) return SOLARI_OK;

    rc = dbStmtPrepare(conn, SQL, &st);
    if (rc != SOLARI_OK) return rc;

    dbBindStr(&pin[0], scope, &scopeLen);
    if (mysql_stmt_bind_param(st, pin) != 0) { mysql_stmt_close(st); return ERR_DB; }
    if (mysql_stmt_execute(st) != 0)         { mysql_stmt_close(st); return ERR_DB; }

    memset(rout, 0, sizeof rout);
    dbBindI32(&rout[0], &rRuleId);
    dbBindOutStr(&rout[1], rScope, sizeof rScope, &lScope, NULL);
    dbBindOutStr(&rout[2], rMetric, sizeof rMetric, &lMetric, NULL);
    dbBindOutStr(&rout[3], rOp, sizeof rOp, &lOp, NULL);
    dbBindDouble(&rout[4], &rThresh); rout[4].is_null = &nThresh;
    dbBindI32(&rout[5], &rForSec);    rout[5].is_null = &nForSec;
    dbBindOutStr(&rout[6], rSeverity, sizeof rSeverity, &lSeverity, NULL);

    if (mysql_stmt_bind_result(st, rout) != 0) { mysql_stmt_close(st); return ERR_DB; }
    if (mysql_stmt_store_result(st) != 0)      { mysql_stmt_close(st); return ERR_DB; }

    while (n < cap && mysql_stmt_fetch(st) == 0) {
        serverAlertRule *r = &out[n];
        memset(r, 0, sizeof *r);
        r->ruleId     = rRuleId;
        r->threshold  = nThresh ? 0.0 : rThresh;
        r->forSeconds = nForSec ? 0   : rForSec;
        memcpy(r->scope,    rScope,    sizeof r->scope);    r->scope[sizeof r->scope - 1] = '\0';
        memcpy(r->metric,   rMetric,   sizeof r->metric);   r->metric[sizeof r->metric - 1] = '\0';
        memcpy(r->op,       rOp,       sizeof r->op);       r->op[sizeof r->op - 1] = '\0';
        memcpy(r->severity, rSeverity, sizeof r->severity); r->severity[sizeof r->severity - 1] = '\0';
        n++;
    }

    mysql_stmt_free_result(st);
    mysql_stmt_close(st);
    *count = n;
    return SOLARI_OK;
}

/* ===================================================================== */
/* config / convergence (§10 nodeConfig)                                  */
/* ===================================================================== */

solariStatus serverDbSetNodeTargetEpoch(serverDb *db, uint64_t nodeId,
                                        uint64_t targetEpoch, const char *configJson)
{
    static const char *SQL =
        "INSERT INTO nodeConfig (nodeId, targetEpoch, configBlob, lastDirectiveAt) "
        "VALUES (?, ?, ?, UTC_TIMESTAMP()) "
        "ON DUPLICATE KEY UPDATE "
        "  targetEpoch=VALUES(targetEpoch), configBlob=VALUES(configBlob), "
        "  lastDirectiveAt=UTC_TIMESTAMP()";
    MYSQL      *conn = dbConn(db);
    MYSQL_STMT *st;
    MYSQL_BIND  b[3];
    unsigned long long vNode = nodeId, vEpoch = targetEpoch;
    unsigned long cLen;
    solariStatus rc;

    if (!conn) return ERR_INVALID_ARG;
    rc = dbStmtPrepare(conn, SQL, &st);
    if (rc != SOLARI_OK) return rc;
    dbBindU64(&b[0], &vNode);
    dbBindU64(&b[1], &vEpoch);
    dbBindStr(&b[2], (configJson && configJson[0]) ? configJson : NULL, &cLen);
    rc = dbStmtRunWrite(st, b, 3);
    mysql_stmt_close(st);
    return rc;
}

solariStatus serverDbSetNodeApplied(serverDb *db, uint64_t nodeId,
                                    uint64_t appliedEpoch, const char *lastResult,
                                    uint64_t whenUnixMs)
{
    static const char *SQL =
        "UPDATE nodeConfig SET appliedEpoch = ?, lastResult = ?, "
        "  lastDirectiveAt = FROM_UNIXTIME(? / 1000) WHERE nodeId = ?";
    MYSQL      *conn = dbConn(db);
    MYSQL_STMT *st;
    MYSQL_BIND  b[4];
    unsigned long long vApplied = appliedEpoch;
    unsigned long long vWhen = whenUnixMs ? whenUnixMs : solariNowUnixMs();
    unsigned long long vNode = nodeId;
    unsigned long rLen;
    solariStatus rc;

    if (!conn) return ERR_INVALID_ARG;
    rc = dbStmtPrepare(conn, SQL, &st);
    if (rc != SOLARI_OK) return rc;
    dbBindU64(&b[0], &vApplied);
    dbBindStr(&b[1], (lastResult && lastResult[0]) ? lastResult : NULL, &rLen);
    dbBindU64(&b[2], &vWhen);
    dbBindU64(&b[3], &vNode);
    rc = dbStmtRunWrite(st, b, 4);
    mysql_stmt_close(st);
    return rc;
}

solariStatus serverDbControlDirectiveAdd(serverDb *db, uint64_t nodeId,
                                         uint32_t correlationId, uint8_t verb,
                                         uint64_t targetEpoch)
{
    static const char *SQL =
        "INSERT INTO pendingControlDirective "
        "(nodeId,correlationId,verb,targetEpoch) VALUES (?,?,?,?)";
    MYSQL *conn = dbConn(db);
    MYSQL_STMT *st = NULL;
    MYSQL_BIND b[4];
    unsigned long long vNode = nodeId, vCorr = correlationId;
    unsigned long long vEpoch = targetEpoch, vVerb = verb;
    solariStatus rc;
    if (!conn || !nodeId || !correlationId || !verb) return ERR_INVALID_ARG;
    rc = dbStmtPrepare(conn, SQL, &st);
    if (rc != SOLARI_OK) return rc;
    dbBindU64(&b[0], &vNode);
    dbBindU64(&b[1], &vCorr);
    dbBindU64(&b[2], &vVerb);
    dbBindU64(&b[3], &vEpoch);
    rc = dbStmtRunWrite(st, b, 4);
    mysql_stmt_close(st);
    return rc;
}

solariStatus serverDbControlDirectiveCancel(serverDb *db, uint64_t nodeId,
                                            uint32_t correlationId)
{
    static const char *SQL =
        "DELETE FROM pendingControlDirective WHERE nodeId=? AND correlationId=?";
    MYSQL *conn = dbConn(db);
    MYSQL_STMT *st = NULL;
    MYSQL_BIND b[2];
    unsigned long long vNode = nodeId, vCorr = correlationId;
    solariStatus rc;
    if (!conn || !nodeId || !correlationId) return ERR_INVALID_ARG;
    rc = dbStmtPrepare(conn, SQL, &st);
    if (rc != SOLARI_OK) return rc;
    dbBindU64(&b[0], &vNode);
    dbBindU64(&b[1], &vCorr);
    rc = dbStmtRunWrite(st, b, 2);
    mysql_stmt_close(st);
    return rc;
}

solariStatus serverDbControlResultApply(serverDb *db, uint64_t nodeId,
                                        uint32_t correlationId, uint8_t verb,
                                        uint64_t appliedEpoch, bool haveAppliedEpoch,
                                        uint16_t errorCode,
                                        const char *lastResult,
                                        uint64_t whenUnixMs)
{
    static const char *DELETE_SQL =
        "DELETE FROM pendingControlDirective WHERE nodeId=? AND correlationId=? "
        "AND verb=? AND (?<>0 OR ?=0 OR ?=0 OR targetEpoch=?)";
    static const char *UPDATE_SQL =
        "UPDATE nodeConfig SET appliedEpoch=IF(?<>0 AND ?=0 AND ?<>0,?,appliedEpoch),lastResult=?, "
        "lastDirectiveAt=FROM_UNIXTIME(? / 1000) WHERE nodeId=?";
    MYSQL *conn = NULL;
    MYSQL_STMT *st = NULL;
    MYSQL_BIND b[7];
    unsigned long long vNode=nodeId, vCorr=correlationId, vEpoch=appliedEpoch;
    unsigned long long vWhen=whenUnixMs ? whenUnixMs : solariNowUnixMs();
    unsigned long long vFailed=(errorCode == 0 ? 0 : 1);
    unsigned long long vVerb=verb;
    unsigned long long vConvergent=(verb == CTRL_SET_CONFIG || verb == CTRL_PROVISION) ? 1 : 0;
    unsigned long long vHaveEpoch=haveAppliedEpoch ? 1 : 0;
    unsigned long rLen=0;
    bool txn=false;
    solariStatus rc;
    if (!db || !db->cfg || !nodeId || !correlationId || !verb) return ERR_INVALID_ARG;
    /* This operation must be indivisible across DELETE + UPDATE. Use a
     * transaction-only connection with automatic reconnect disabled: a
     * reconnect would create a new autocommit session and could split the
     * transaction. The normal pooled connection keeps its reconnect policy. */
    conn = dbOpenTxnConn(db);
    if (!conn) return ERR_DB;
    if (mysql_autocommit(conn, 0) != 0) {
        mysql_close(conn);
        return ERR_DB;
    }
    txn=true;
    rc=dbStmtPrepare(conn, DELETE_SQL, &st);
    if (rc != SOLARI_OK) goto finish;
    dbBindU64(&b[0], &vNode); dbBindU64(&b[1], &vCorr);
    dbBindU64(&b[2], &vVerb); dbBindU64(&b[3], &vFailed);
    dbBindU64(&b[4], &vConvergent); dbBindU64(&b[5], &vHaveEpoch);
    dbBindU64(&b[6], &vEpoch);
    rc=dbStmtRunWrite(st,b,7);
    if (rc != SOLARI_OK) goto finish;
    if (mysql_stmt_affected_rows(st) != 1) { rc=ERR_AUTH_ROLE; goto finish; }
    mysql_stmt_close(st); st=NULL;

    rc=dbStmtPrepare(conn, UPDATE_SQL, &st);
    if (rc != SOLARI_OK) goto finish;
    dbBindU64(&b[0], &vConvergent); dbBindU64(&b[1], &vFailed);
    dbBindU64(&b[2], &vHaveEpoch); dbBindU64(&b[3], &vEpoch);
    dbBindStr(&b[4], (lastResult && lastResult[0]) ? lastResult : NULL, &rLen);
    dbBindU64(&b[5], &vWhen); dbBindU64(&b[6], &vNode);
    rc=dbStmtRunWrite(st,b,7);
    if (rc != SOLARI_OK) goto finish;
    mysql_stmt_close(st); st=NULL;
    if (mysql_commit(conn) != 0) { rc=ERR_DB; goto finish; }
    txn=false; rc=SOLARI_OK;
finish:
    if (st) mysql_stmt_close(st);
    if (txn) mysql_rollback(conn);
    mysql_close(conn);
    return rc;
}

/* ===================================================================== */
/* lease ops (§10 serverLease) - the conditional UPDATE IS the election    */
/* ===================================================================== */

solariStatus serverDbLeaseClaim(serverDb *db, uint64_t selfNodeId,
                                uint32_t ttlSec, bool *won, uint64_t *epoch)
{
    /* Single-row election (id=1). A live current holder renews without changing
     * its fencing epoch. A claim against an expired lease transfers the holder
     * and advances the epoch exactly once, even when the same node identity
     * reacquires after expiry. A losing claim against a live lease must leave
     * the row unchanged; in particular, standby polling must not invalidate the
     * active holder's fencing token. */
    static const char *SQL_UPSERT =
        "INSERT INTO serverLease (id, leaseHolder, leaseEpoch, expiresAt) "
        "VALUES (1, ?, 1, UTC_TIMESTAMP(3) + INTERVAL ? SECOND) "
        "ON DUPLICATE KEY UPDATE "
        "  leaseEpoch = leaseEpoch + IF(expiresAt < UTC_TIMESTAMP(3), 1, 0), "
        "  leaseHolder = IF(leaseHolder = VALUES(leaseHolder) "
        "                   OR expiresAt < UTC_TIMESTAMP(3), "
        "                   VALUES(leaseHolder), leaseHolder), "
        "  expiresAt = IF(leaseHolder = VALUES(leaseHolder) "
        "                 OR expiresAt < UTC_TIMESTAMP(3), "
        "                 UTC_TIMESTAMP(3) + INTERVAL ? SECOND, expiresAt)";
    MYSQL      *conn = dbConn(db);
    MYSQL_STMT *st;
    MYSQL_BIND  b[3];
    unsigned long long vSelf = selfNodeId;
    int                vTtl  = (int)(ttlSec ? ttlSec : 15);
    int                vTtl2 = vTtl;
    solariStatus rc;
    uint64_t holder = 0, ep = 0;

    if (won)   *won = false;
    if (epoch) *epoch = 0;
    if (!conn) return ERR_INVALID_ARG;

    rc = dbStmtPrepare(conn, SQL_UPSERT, &st);
    if (rc != SOLARI_OK) return rc;
    dbBindU64(&b[0], &vSelf);
    dbBindI32(&b[1], &vTtl);
    dbBindI32(&b[2], &vTtl2);
    rc = dbStmtRunWrite(st, b, 3);
    mysql_stmt_close(st);
    if (rc != SOLARI_OK) return rc;

    /* Read back the authoritative holder/epoch to decide won and report epoch. */
    rc = serverDbLeaseRead(db, &holder, &ep);
    if (rc != SOLARI_OK) return rc;

    if (won)   *won   = (holder == selfNodeId);
    if (epoch) *epoch = ep;
    return SOLARI_OK;
}

solariStatus serverDbLeaseRead(serverDb *db, uint64_t *holder, uint64_t *epoch)
{
    static const char *SQL =
        "SELECT IF(expiresAt < UTC_TIMESTAMP(3), 0, leaseHolder), leaseEpoch "
        "FROM serverLease WHERE id = 1";
    MYSQL      *conn = dbConn(db);
    MYSQL_STMT *st;
    MYSQL_BIND  rout[2];
    unsigned long long rHolder = 0, rEpoch = 0;
    my_bool      nHolder = 0, nEpoch = 0;
    int          fetch;
    solariStatus rc;

    if (holder) *holder = 0;
    if (epoch)  *epoch = 0;
    if (!conn) return ERR_INVALID_ARG;

    rc = dbStmtPrepare(conn, SQL, &st);
    if (rc != SOLARI_OK) return rc;
    if (mysql_stmt_execute(st) != 0) { mysql_stmt_close(st); return ERR_DB; }

    memset(rout, 0, sizeof rout);
    dbBindOutU64(&rout[0], &rHolder, &nHolder);
    dbBindOutU64(&rout[1], &rEpoch, &nEpoch);
    if (mysql_stmt_bind_result(st, rout) != 0) { mysql_stmt_close(st); return ERR_DB; }
    if (mysql_stmt_store_result(st) != 0)      { mysql_stmt_close(st); return ERR_DB; }

    fetch = mysql_stmt_fetch(st);
    if (fetch == 0) {
        if (holder) *holder = nHolder ? 0 : rHolder;
        if (epoch)  *epoch  = nEpoch ? 0 : rEpoch;
    }
    /* No row yet (lease never claimed) leaves the zeroed out-params - that is a
     * valid "unheld" answer, not an error. */
    mysql_stmt_free_result(st);
    mysql_stmt_close(st);
    return SOLARI_OK;
}

solariStatus serverDbLeaseRelease(serverDb *db, uint64_t selfNodeId,
                                  uint64_t epoch)
{
    static const char *SQL =
        "UPDATE serverLease SET expiresAt=UTC_TIMESTAMP(3)-INTERVAL 1 SECOND "
        "WHERE id=1 AND leaseHolder=? AND leaseEpoch=? "
        "AND expiresAt>UTC_TIMESTAMP(3)";
    MYSQL *conn = dbConn(db);
    MYSQL_STMT *st;
    MYSQL_BIND b[2];
    unsigned long long holder = selfNodeId, fencingEpoch = epoch;
    solariStatus rc;

    if (!conn || !selfNodeId || !epoch) return ERR_INVALID_ARG;
    rc = dbStmtPrepare(conn, SQL, &st);
    if (rc != SOLARI_OK) return rc;
    dbBindU64(&b[0], &holder);
    dbBindU64(&b[1], &fencingEpoch);
    rc = dbStmtRunWrite(st, b, 2);
    mysql_stmt_close(st);
    return rc;
}

/* Start a transaction fenced by the current InnoDB lease row. Holding this row
 * lock through the caller's writes orders them before any successor claim. */
static solariStatus dbScanFenceBegin(serverDb *db, MYSQL *conn, uint64_t ownerNodeId,
                                     uint64_t leaseEpoch)
{
    static const char *SQL =
        "SELECT leaseHolder,leaseEpoch,(expiresAt>UTC_TIMESTAMP(3)) "
        "FROM serverLease WHERE id=1 FOR UPDATE";
    MYSQL_STMT *st = NULL;
    MYSQL_BIND out[3];
    unsigned long long holder = 0, epoch = 0;
    int live = 0, fetch;
    my_bool nHolder = 0, nEpoch = 0;
    solariStatus rc = ERR_DB;

    if (dbTxnBegin(conn) != SOLARI_OK) return ERR_DB;
    if (dbStmtPrepare(conn, SQL, &st) != SOLARI_OK) goto done;
    if (mysql_stmt_execute(st) != 0) goto done;
    memset(out, 0, sizeof out);
    dbBindOutU64(&out[0], &holder, &nHolder);
    dbBindOutU64(&out[1], &epoch, &nEpoch);
    dbBindOutI32(&out[2], &live, NULL);
    if (mysql_stmt_bind_result(st, out) != 0 || mysql_stmt_store_result(st) != 0)
        goto done;
    fetch = mysql_stmt_fetch(st);
    if (fetch == 0 && !nHolder && !nEpoch && holder == ownerNodeId
        && epoch == leaseEpoch && live) {
        rc = SOLARI_OK;
    } else if (fetch == MYSQL_NO_DATA || fetch == 0) {
        rc = ERR_LEASE_LOST;
    }

done:
    if (st) mysql_stmt_close(st);
    if (rc != SOLARI_OK) rc = dbTxnFinish(db, conn, false, rc);
    return rc;
}

static solariStatus dbScanFenceFinish(serverDb *db, MYSQL *conn, bool commit,
                                      solariStatus outcome)
{
    return dbTxnFinish(db, conn, commit, outcome);
}

solariStatus serverDbDiscoveryScanSubmit(serverDb *db, uint64_t ownerNodeId,
                                         uint64_t leaseEpoch, const char *requestedBy,
                                         const char *cidr, const char *ports,
                                         uint32_t hostsTotal, uint64_t *scanIdOut)
{
    static const char *SQL_STALE =
        "UPDATE discoveryScanJob SET status='failed',errorCode=?,"
        "errorText='lease owner changed before scan completed',"
        "updatedAt=UTC_TIMESTAMP(3),completedAt=UTC_TIMESTAMP(3) "
        "WHERE status IN ('queued','running') AND (ownerNodeId<>? OR leaseEpoch<>?)";
    static const char *SQL_ACTIVE =
        "SELECT scanId FROM discoveryScanJob WHERE status IN ('queued','running') "
        "ORDER BY scanId LIMIT 1 FOR UPDATE";
    static const char *SQL_PRUNE =
        "DELETE FROM discoveryScanJob WHERE status IN ('completed','failed','cancelled') "
        "AND (createdAt<UTC_TIMESTAMP(3)-INTERVAL 30 DAY OR scanId<=("
        "SELECT cutoff FROM (SELECT scanId AS cutoff FROM discoveryScanJob "
        "WHERE status IN ('completed','failed','cancelled') ORDER BY scanId DESC "
        "LIMIT 1 OFFSET 1000) retained))";
    static const char *SQL_INSERT =
        "INSERT INTO discoveryScanJob "
        "(requestedBy,ownerNodeId,leaseEpoch,cidr,ports,status,hostsTotal,createdAt,updatedAt) "
        "VALUES (?,?,?,?,?,'queued',?,UTC_TIMESTAMP(3),UTC_TIMESTAMP(3))";
    MYSQL *conn = dbConn(db);
    MYSQL_STMT *st = NULL;
    MYSQL_BIND b[6], out;
    unsigned long lUser = 0, lCidr = 0, lPorts = 0;
    unsigned long long owner = ownerNodeId, epoch = leaseEpoch, id = 0;
    unsigned long long maxId = 0;
    uint64_t outId = 0;
    unsigned long long total = hostsTotal;
    int errCode = ERR_LEASE_LOST;
    my_bool nMax = 0;
    int fetch;
    solariStatus rc;

    if (scanIdOut) *scanIdOut = 0;
    if (!conn || !scanIdOut || !requestedBy || !requestedBy[0] || !cidr
        || !cidr[0] || !ports || hostsTotal == 0
        || strlen(requestedBy) > 128 || strlen(cidr) > 32 || strlen(ports) > 255)
        return ERR_INVALID_ARG;
    rc = dbScanFenceBegin(db, conn, ownerNodeId, leaseEpoch);
    if (rc != SOLARI_OK) return rc;

    rc = dbStmtPrepare(conn, SQL_STALE, &st);
    if (rc != SOLARI_OK) goto fail;
    dbBindI32(&b[0], &errCode);
    dbBindU64(&b[1], &owner);
    dbBindU64(&b[2], &epoch);
    rc = dbStmtRunWrite(st, b, 3);
    mysql_stmt_close(st); st = NULL;
    if (rc != SOLARI_OK) goto fail;

    rc = dbStmtPrepare(conn, SQL_ACTIVE, &st);
    if (rc != SOLARI_OK) goto fail;
    if (mysql_stmt_execute(st) != 0) { rc = ERR_DB; goto fail; }
    memset(&out, 0, sizeof out);
    dbBindOutU64(&out, &maxId, &nMax);
    if (mysql_stmt_bind_result(st, &out) != 0 || mysql_stmt_store_result(st) != 0) {
        rc = ERR_DB; goto fail;
    }
    fetch = mysql_stmt_fetch(st);
    mysql_stmt_free_result(st);
    mysql_stmt_close(st); st = NULL;
    if (fetch == 0) {
        return dbScanFenceFinish(db, conn, true, ERR_CONN_RETRY);
    }
    if (fetch != MYSQL_NO_DATA) { rc = ERR_DB; goto fail; }

    rc = dbStmtPrepare(conn, SQL_PRUNE, &st);
    if (rc != SOLARI_OK) goto fail;
    rc = dbStmtRunWrite(st, NULL, 0);
    mysql_stmt_close(st); st = NULL;
    if (rc != SOLARI_OK) goto fail;

    rc = dbStmtPrepare(conn, SQL_INSERT, &st);
    if (rc != SOLARI_OK) goto fail;
    dbBindStr(&b[0], requestedBy, &lUser);
    dbBindU64(&b[1], &owner);
    dbBindU64(&b[2], &epoch);
    dbBindStr(&b[3], cidr, &lCidr);
    dbBindStr(&b[4], ports, &lPorts);
    dbBindU64(&b[5], &total);
    rc = dbStmtRunWrite(st, b, 6);
    if (rc == SOLARI_OK) id = mysql_stmt_insert_id(st);
    mysql_stmt_close(st); st = NULL;
    if (rc != SOLARI_OK || id == 0) { if (rc == SOLARI_OK) rc = ERR_DB; goto fail; }
    outId = (uint64_t)id;
    rc = dbScanFenceFinish(db, conn, true, SOLARI_OK);
    if (rc == SOLARI_OK) *scanIdOut = outId;
    return rc;

fail:
    if (st) mysql_stmt_close(st);
    (void)nMax;
    return dbScanFenceFinish(db, conn, false, rc);
}

solariStatus serverDbDiscoveryScanClaim(serverDb *db, uint64_t ownerNodeId,
                                        uint64_t leaseEpoch,
                                        serverDiscoveryScanJob *out)
{
    static const char *SQL_STALE =
        "UPDATE discoveryScanJob SET status='failed',errorCode=?,"
        "errorText='scan worker restarted or lease owner changed',"
        "updatedAt=UTC_TIMESTAMP(3),completedAt=UTC_TIMESTAMP(3) "
        "WHERE status IN ('queued','running') AND (ownerNodeId<>? OR leaseEpoch<>? "
        "OR (ownerNodeId=? AND leaseEpoch=? AND status='running'))";
    static const char *SQL_FIND =
        "SELECT scanId,requestedBy,ownerNodeId,leaseEpoch,cidr,ports,hostsTotal "
        "FROM discoveryScanJob WHERE ownerNodeId=? AND leaseEpoch=? AND status='queued' "
        "ORDER BY scanId LIMIT 1 FOR UPDATE";
    static const char *SQL_START =
        "UPDATE discoveryScanJob SET status='running',startedAt=UTC_TIMESTAMP(3),"
        "updatedAt=UTC_TIMESTAMP(3) WHERE scanId=? AND ownerNodeId=? AND leaseEpoch=? "
        "AND status='queued'";
    MYSQL *conn = dbConn(db);
    MYSQL_STMT *st = NULL;
    MYSQL_BIND in[5], result[7];
    unsigned long long owner = ownerNodeId, epoch = leaseEpoch, errId = 0;
    unsigned long long scanId = 0, resultOwner = 0, resultEpoch = 0, hostsTotal = 0;
    unsigned long userLen = 0, cidrLen = 0, portsLen = 0;
    char user[129] = {0}, cidr[33] = {0}, ports[256] = {0};
    my_bool nScan = 0, nOwner = 0, nEpoch = 0, nTotal = 0;
    int errCode = ERR_LEASE_LOST, fetch;
    my_ulonglong changed;
    solariStatus rc;

    if (!conn || !out) return ERR_INVALID_ARG;
    memset(out, 0, sizeof *out);
    rc = dbScanFenceBegin(db, conn, ownerNodeId, leaseEpoch);
    if (rc != SOLARI_OK) return rc;
    rc = dbStmtPrepare(conn, SQL_STALE, &st);
    if (rc != SOLARI_OK) goto fail;
    dbBindI32(&in[0], &errCode);
    dbBindU64(&in[1], &owner);
    dbBindU64(&in[2], &epoch);
    dbBindU64(&in[3], &owner);
    dbBindU64(&in[4], &epoch);
    rc = dbStmtRunWrite(st, in, 5);
    mysql_stmt_close(st); st = NULL;
    if (rc != SOLARI_OK) goto fail;

    rc = dbStmtPrepare(conn, SQL_FIND, &st);
    if (rc != SOLARI_OK) goto fail;
    dbBindU64(&in[0], &owner);
    dbBindU64(&in[1], &epoch);
    if (mysql_stmt_bind_param(st, in) != 0 || mysql_stmt_execute(st) != 0) {
        rc = ERR_DB; goto fail;
    }
    memset(result, 0, sizeof result);
    dbBindOutU64(&result[0], &scanId, &nScan);
    dbBindOutStr(&result[1], user, sizeof user, &userLen, NULL);
    dbBindOutU64(&result[2], &resultOwner, &nOwner);
    dbBindOutU64(&result[3], &resultEpoch, &nEpoch);
    dbBindOutStr(&result[4], cidr, sizeof cidr, &cidrLen, NULL);
    dbBindOutStr(&result[5], ports, sizeof ports, &portsLen, NULL);
    dbBindOutU64(&result[6], &hostsTotal, &nTotal);
    if (mysql_stmt_bind_result(st, result) != 0 || mysql_stmt_store_result(st) != 0) {
        rc = ERR_DB; goto fail;
    }
    fetch = mysql_stmt_fetch(st);
    if (fetch == MYSQL_NO_DATA) {
        mysql_stmt_free_result(st);
        mysql_stmt_close(st); st = NULL;
        return dbScanFenceFinish(db, conn, true, ERR_CONN_RETRY);
    }
    if (fetch != 0 || nScan || nOwner || nEpoch || nTotal
        || userLen >= sizeof user || cidrLen >= sizeof cidr || portsLen >= sizeof ports) {
        rc = ERR_DB; goto fail;
    }
    mysql_stmt_free_result(st);
    mysql_stmt_close(st); st = NULL;

    rc = dbStmtPrepare(conn, SQL_START, &st);
    if (rc != SOLARI_OK) goto fail;
    errId = scanId;
    dbBindU64(&in[0], &errId);
    dbBindU64(&in[1], &owner);
    dbBindU64(&in[2], &epoch);
    rc = dbStmtRunWrite(st, in, 3);
    changed = (rc == SOLARI_OK) ? mysql_stmt_affected_rows(st) : 0;
    mysql_stmt_close(st); st = NULL;
    if (rc != SOLARI_OK || changed == 0) {
        if (rc == SOLARI_OK) rc = ERR_TLV_END;
        goto fail;
    }
    out->scanId = scanId;
    out->ownerNodeId = resultOwner;
    out->leaseEpoch = resultEpoch;
    out->hostsTotal = (uint32_t)hostsTotal;
    memcpy(out->requestedBy, user, userLen); out->requestedBy[userLen] = '\0';
    memcpy(out->cidr, cidr, cidrLen); out->cidr[cidrLen] = '\0';
    memcpy(out->ports, ports, portsLen); out->ports[portsLen] = '\0';
    snprintf(out->status, sizeof out->status, "running");
    return dbScanFenceFinish(db, conn, true, SOLARI_OK);

fail:
    if (st) mysql_stmt_close(st);
    return dbScanFenceFinish(db, conn, false, rc);
}

solariStatus serverDbDiscoveryScanUpdate(serverDb *db, uint64_t scanId,
                                         uint64_t ownerNodeId, uint64_t leaseEpoch,
                                         const char *status, uint32_t hostsDone,
                                         uint32_t hostsFound, int32_t errorCode,
                                         const char *errorText)
{
    static const char *SQL_CHECK =
        "SELECT hostsTotal FROM discoveryScanJob WHERE scanId=? AND ownerNodeId=? "
        "AND leaseEpoch=? AND status='running' FOR UPDATE";
    static const char *SQL_UPDATE =
        "UPDATE discoveryScanJob SET status=?,hostsDone=?,hostsFound=?,"
        "errorCode=NULLIF(?,0),errorText=?,updatedAt=UTC_TIMESTAMP(3),"
        "completedAt=IF(? IN ('completed','failed','cancelled'),UTC_TIMESTAMP(3),completedAt) "
        "WHERE scanId=? AND ownerNodeId=? AND leaseEpoch=? AND status='running'";
    MYSQL *conn = dbConn(db);
    MYSQL_STMT *st = NULL;
    MYSQL_BIND in[9], result;
    unsigned long long id = scanId, owner = ownerNodeId, epoch = leaseEpoch;
    unsigned long long total = 0, done = hostsDone, found = hostsFound;
    unsigned long statusLen = 0, errorLen = 0;
    int error = errorCode, fetch;
    my_bool nTotal = 0;
    const bool terminal = status && (!strcmp(status, "completed")
        || !strcmp(status, "failed") || !strcmp(status, "cancelled"));
    solariStatus rc;

    if (!conn || scanId == 0 || !status
        || (strcmp(status, "running") && !terminal)
        || (errorText && strlen(errorText) > 255)) return ERR_INVALID_ARG;
    rc = dbScanFenceBegin(db, conn, ownerNodeId, leaseEpoch);
    if (rc != SOLARI_OK) return rc;
    rc = dbStmtPrepare(conn, SQL_CHECK, &st);
    if (rc != SOLARI_OK) goto fail;
    dbBindU64(&in[0], &id); dbBindU64(&in[1], &owner); dbBindU64(&in[2], &epoch);
    if (mysql_stmt_bind_param(st, in) != 0 || mysql_stmt_execute(st) != 0) { rc = ERR_DB; goto fail; }
    memset(&result, 0, sizeof result);
    dbBindOutU64(&result, &total, &nTotal);
    if (mysql_stmt_bind_result(st, &result) != 0 || mysql_stmt_store_result(st) != 0) { rc = ERR_DB; goto fail; }
    fetch = mysql_stmt_fetch(st);
    mysql_stmt_free_result(st); mysql_stmt_close(st); st = NULL;
    if (fetch == MYSQL_NO_DATA) { rc = ERR_TLV_END; goto fail; }
    if (fetch != 0 || nTotal || hostsDone > total || hostsFound > hostsDone) { rc = ERR_INVALID_ARG; goto fail; }

    rc = dbStmtPrepare(conn, SQL_UPDATE, &st);
    if (rc != SOLARI_OK) goto fail;
    dbBindStr(&in[0], status, &statusLen);
    dbBindU64(&in[1], &done); dbBindU64(&in[2], &found); dbBindI32(&in[3], &error);
    dbBindStr(&in[4], (errorText && errorText[0]) ? errorText : NULL, &errorLen);
    dbBindStr(&in[5], status, &statusLen);
    dbBindU64(&in[6], &id); dbBindU64(&in[7], &owner); dbBindU64(&in[8], &epoch);
    rc = dbStmtRunWrite(st, in, 9);
    mysql_stmt_close(st); st = NULL;
    if (rc != SOLARI_OK) goto fail;
    return dbScanFenceFinish(db, conn, true, SOLARI_OK);

fail:
    if (st) mysql_stmt_close(st);
    return dbScanFenceFinish(db, conn, false, rc);
}

solariStatus serverDbDiscoveryScanGet(serverDb *db, uint64_t scanId,
                                      const char *requestedBy,
                                      serverDiscoveryScanJob *out)
{
    static const char *SQL =
        "SELECT scanId,requestedBy,ownerNodeId,leaseEpoch,cidr,ports,status,"
        "hostsTotal,hostsDone,hostsFound,errorCode,errorText FROM discoveryScanJob "
        "WHERE scanId=? AND requestedBy=?";
    MYSQL *conn = dbConn(db);
    MYSQL_STMT *st = NULL;
    MYSQL_BIND in[2], result[12];
    unsigned long long id = scanId, owner = 0, epoch = 0, total = 0, done = 0, found = 0;
    unsigned long userLen = 0, cidrLen = 0, portsLen = 0, statusLen = 0, errorLen = 0;
    char user[129] = {0}, cidr[33] = {0}, ports[256] = {0}, status[16] = {0}, errorText[256] = {0};
    int errorCode = 0;
    my_bool nId = 0, nOwner = 0, nEpoch = 0, nTotal = 0, nDone = 0, nFound = 0;
    my_bool nError = 0;
    int fetch;
    solariStatus rc;

    if (!conn || !out || !requestedBy || !requestedBy[0] || strlen(requestedBy) > 128)
        return ERR_INVALID_ARG;
    memset(out, 0, sizeof *out);
    rc = dbStmtPrepare(conn, SQL, &st);
    if (rc != SOLARI_OK) return rc;
    dbBindU64(&in[0], &id);
    dbBindStr(&in[1], requestedBy, &userLen);
    if (mysql_stmt_bind_param(st, in) != 0 || mysql_stmt_execute(st) != 0) {
        mysql_stmt_close(st); return ERR_DB;
    }
    memset(result, 0, sizeof result);
    dbBindOutU64(&result[0], &id, &nId);
    dbBindOutStr(&result[1], user, sizeof user, &userLen, NULL);
    dbBindOutU64(&result[2], &owner, &nOwner);
    dbBindOutU64(&result[3], &epoch, &nEpoch);
    dbBindOutStr(&result[4], cidr, sizeof cidr, &cidrLen, NULL);
    dbBindOutStr(&result[5], ports, sizeof ports, &portsLen, NULL);
    dbBindOutStr(&result[6], status, sizeof status, &statusLen, NULL);
    dbBindOutU64(&result[7], &total, &nTotal);
    dbBindOutU64(&result[8], &done, &nDone);
    dbBindOutU64(&result[9], &found, &nFound);
    dbBindOutI32(&result[10], &errorCode, &nError);
    dbBindOutStr(&result[11], errorText, sizeof errorText, &errorLen, NULL);
    if (mysql_stmt_bind_result(st, result) != 0 || mysql_stmt_store_result(st) != 0) {
        mysql_stmt_close(st); return ERR_DB;
    }
    fetch = mysql_stmt_fetch(st);
    mysql_stmt_free_result(st); mysql_stmt_close(st);
    if (fetch == MYSQL_NO_DATA) return ERR_TLV_END;
    if (fetch != 0 || nId || nOwner || nEpoch || nTotal || nDone || nFound
        || userLen >= sizeof user || cidrLen >= sizeof cidr || portsLen >= sizeof ports
        || statusLen >= sizeof status || errorLen >= sizeof errorText)
        return ERR_DB;
    out->scanId = id; out->ownerNodeId = owner; out->leaseEpoch = epoch;
    out->hostsTotal = (uint32_t)total; out->hostsDone = (uint32_t)done;
    out->hostsFound = (uint32_t)found; out->errorCode = nError ? 0 : errorCode;
    memcpy(out->requestedBy, user, userLen); out->requestedBy[userLen] = '\0';
    memcpy(out->cidr, cidr, cidrLen); out->cidr[cidrLen] = '\0';
    memcpy(out->ports, ports, portsLen); out->ports[portsLen] = '\0';
    memcpy(out->status, status, statusLen); out->status[statusLen] = '\0';
    memcpy(out->errorText, errorText, errorLen); out->errorText[errorLen] = '\0';
    return SOLARI_OK;
}

solariStatus serverDbUpsertDiscoveredFenced(serverDb *db,
                                            const serverDiscEntity *entity,
                                            uint64_t whenUnixMs,
                                            uint64_t ownerNodeId,
                                            uint64_t leaseEpoch,
                                            uint64_t *discId)
{
    MYSQL *conn = dbConn(db);
    solariStatus rc;
    if (!conn || !entity) return ERR_INVALID_ARG;
    rc = dbScanFenceBegin(db, conn, ownerNodeId, leaseEpoch);
    if (rc != SOLARI_OK) return rc;
    rc = serverDbUpsertDiscovered(db, entity, whenUnixMs, discId);
    return dbScanFenceFinish(db, conn, rc == SOLARI_OK, rc);
}

/* ===================================================================== */
/* discovery (Handoff §5 discovered)                                      */
/* ===================================================================== */

solariStatus serverDbUpsertDiscovered(serverDb *db, const serverDiscEntity *e,
                                      uint64_t whenUnixMs, uint64_t *discId)
{
    static const char *SQL =
        "INSERT INTO discovered "
        "  (host, ip, kind, via, services, segId, arch, "
        "   mac, vendor, osName, deviceRole, sysDescr, mdnsName, "
        "   seenCount, firstSeenAt, lastSeenAt, enrichedAt) "
        "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, 1, "
        "        FROM_UNIXTIME(? / 1000), FROM_UNIXTIME(? / 1000), "
        "        IF(? <> 0, FROM_UNIXTIME(? / 1000), NULL)) "
        "ON DUPLICATE KEY UPDATE "
        "  host=VALUES(host), via=VALUES(via), services=VALUES(services), "
        "  segId=VALUES(segId), arch=VALUES(arch), "
        "  mac=IF(VALUES(mac) IS NULL OR VALUES(mac)='', mac, VALUES(mac)), "
        "  vendor=IF(VALUES(vendor) IS NULL OR VALUES(vendor)='', vendor, VALUES(vendor)), "
        "  osName=IF(VALUES(osName) IS NULL OR VALUES(osName)='', osName, VALUES(osName)), "
        "  deviceRole=IF(VALUES(deviceRole) IS NULL OR VALUES(deviceRole)='', deviceRole, VALUES(deviceRole)), "
        "  sysDescr=IF(VALUES(sysDescr) IS NULL OR VALUES(sysDescr)='', sysDescr, VALUES(sysDescr)), "
        "  mdnsName=IF(VALUES(mdnsName) IS NULL OR VALUES(mdnsName)='', mdnsName, VALUES(mdnsName)), "
        "  enrichedAt=IF((VALUES(mac) IS NOT NULL AND VALUES(mac)<>'') OR "
        "                (VALUES(vendor) IS NOT NULL AND VALUES(vendor)<>'') OR "
        "                (VALUES(osName) IS NOT NULL AND VALUES(osName)<>'') OR "
        "                (VALUES(deviceRole) IS NOT NULL AND VALUES(deviceRole)<>'') OR "
        "                (VALUES(sysDescr) IS NOT NULL AND VALUES(sysDescr)<>'') OR "
        "                (VALUES(mdnsName) IS NOT NULL AND VALUES(mdnsName)<>''), "
        "                VALUES(enrichedAt), enrichedAt), "
        "  seenCount = seenCount + 1, lastSeenAt = VALUES(lastSeenAt), "
        "  discId = LAST_INSERT_ID(discId)";
    MYSQL      *conn = dbConn(db);
    MYSQL_STMT *st;
    MYSQL_BIND  b[17];
    unsigned long long vWhen = whenUnixMs ? whenUnixMs : solariNowUnixMs();
    unsigned long long vWhen2 = vWhen;
    unsigned long long vWhen3 = vWhen;
    int vHasEnrich;
    unsigned long lHost, lIp, lKind, lVia, lSvc, lSeg, lArch;
    unsigned long lMac, lVendor, lOs, lRole, lDescr, lMdns;
    solariStatus rc;

    if (discId) *discId = 0;
    if (!conn || !e) return ERR_INVALID_ARG;
    if (e->ip[0] == '\0' || e->kind[0] == '\0') return ERR_INVALID_ARG;

    rc = dbStmtPrepare(conn, SQL, &st);
    if (rc != SOLARI_OK) return rc;
    dbBindStr(&b[0], (e->host[0]) ? e->host : NULL, &lHost);
    dbBindStr(&b[1], e->ip, &lIp);
    dbBindStr(&b[2], e->kind, &lKind);
    dbBindStr(&b[3], (e->via[0]) ? e->via : "scp_advert", &lVia);
    dbBindStr(&b[4], (e->servicesJson[0]) ? e->servicesJson : NULL, &lSvc);
    dbBindStr(&b[5], (e->segId[0]) ? e->segId : NULL, &lSeg);
    dbBindStr(&b[6], (e->arch[0]) ? e->arch : NULL, &lArch);
    dbBindStr(&b[7], (e->mac[0]) ? e->mac : NULL, &lMac);
    dbBindStr(&b[8], (e->vendor[0]) ? e->vendor : NULL, &lVendor);
    dbBindStr(&b[9], (e->osName[0]) ? e->osName : NULL, &lOs);
    dbBindStr(&b[10], (e->deviceRole[0]) ? e->deviceRole : NULL, &lRole);
    dbBindStr(&b[11], (e->sysDescr[0]) ? e->sysDescr : NULL, &lDescr);
    dbBindStr(&b[12], (e->mdnsName[0]) ? e->mdnsName : NULL, &lMdns);
    dbBindU64(&b[13], &vWhen);
    dbBindU64(&b[14], &vWhen2);
    vHasEnrich = (e->mac[0] || e->vendor[0] || e->osName[0] ||
                  e->deviceRole[0] || e->sysDescr[0] || e->mdnsName[0]) ? 1 : 0;
    dbBindI32(&b[15], &vHasEnrich);
    dbBindU64(&b[16], &vWhen3);
    rc = dbStmtRunWrite(st, b, 17);
    if (rc == SOLARI_OK && discId) {
        unsigned long long id = mysql_stmt_insert_id(st);
        *discId = id;   /* insert id, or existing id via LAST_INSERT_ID(discId) */
    }
    mysql_stmt_close(st);
    return rc;
}

solariStatus serverDbSetDiscoveredStatus(serverDb *db, uint64_t discId,
                                         const char *status)
{
    static const char *SQL = "UPDATE discovered SET status = ? WHERE discId = ?";
    MYSQL      *conn = dbConn(db);
    MYSQL_STMT *st;
    MYSQL_BIND  b[2];
    unsigned long long vId = discId;
    unsigned long sLen;
    solariStatus rc;

    if (!conn || !status) return ERR_INVALID_ARG;
    rc = dbStmtPrepare(conn, SQL, &st);
    if (rc != SOLARI_OK) return rc;
    dbBindStr(&b[0], status, &sLen);
    dbBindU64(&b[1], &vId);
    rc = dbStmtRunWrite(st, b, 2);
    mysql_stmt_close(st);
    return rc;
}

solariStatus serverDbDiscoveredIsKnown(serverDb *db, const char *ip,
                                       const char *kind, bool *exists)
{
    /* "known" = already an enrolled node at this address. We treat the node
     * table as authoritative; kind is accepted for signature symmetry and to
     * let a future schema scope the lookup. */
    /* "known" = (ip,kind) already corresponds to an enrolled/adopted entity:
     * an approved enrollment at this address, or a discovered row already
     * marked 'adopted'. Either keeps it out of the operator's "new" list. */
    static const char *SQL =
        "SELECT EXISTS(SELECT 1 FROM enrollment "
        "  WHERE ip = ? AND status = 'approved') "
        "    OR EXISTS(SELECT 1 FROM discovered "
        "  WHERE ip = ? AND kind = ? AND status = 'adopted')";
    MYSQL      *conn = dbConn(db);
    MYSQL_STMT *st;
    MYSQL_BIND  pin[3], rout[1];
    unsigned long lIp, lIp2, lKind;
    unsigned long long rExists = 0;
    my_bool      nExists = 0;
    solariStatus rc;

    if (exists) *exists = false;
    if (!conn || !ip || !kind) return ERR_INVALID_ARG;

    rc = dbStmtPrepare(conn, SQL, &st);
    if (rc != SOLARI_OK) return rc;
    dbBindStr(&pin[0], ip, &lIp);
    dbBindStr(&pin[1], ip, &lIp2);
    dbBindStr(&pin[2], kind, &lKind);
    if (mysql_stmt_bind_param(st, pin) != 0) { mysql_stmt_close(st); return ERR_DB; }
    if (mysql_stmt_execute(st) != 0)         { mysql_stmt_close(st); return ERR_DB; }

    memset(rout, 0, sizeof rout);
    dbBindOutU64(&rout[0], &rExists, &nExists);
    if (mysql_stmt_bind_result(st, rout) != 0) { mysql_stmt_close(st); return ERR_DB; }
    if (mysql_stmt_store_result(st) != 0)      { mysql_stmt_close(st); return ERR_DB; }
    if (mysql_stmt_fetch(st) == 0 && exists)
        *exists = (!nExists && rExists != 0);
    mysql_stmt_free_result(st);
    mysql_stmt_close(st);
    return SOLARI_OK;
}

solariStatus serverDbGetDiscovered(serverDb *db, uint64_t discId,
                                   serverDiscEntity *out)
{
    static const char *SQL =
        "SELECT host, ip, kind, via, services, segId, arch, "
        "       mac, vendor, osName, deviceRole, sysDescr, mdnsName "
        "FROM discovered WHERE discId = ?";
    MYSQL      *conn = dbConn(db);
    MYSQL_STMT *st;
    MYSQL_BIND  pin[1], rout[13];
    unsigned long long vId = discId;
    unsigned long lHost, lIp, lKind, lVia, lSvc, lSeg, lArch;
    unsigned long lMac, lVendor, lOs, lRole, lDescr, lMdns;
    my_bool nHost, nIp, nKind, nVia, nSvc, nSeg, nArch;
    my_bool nMac, nVendor, nOs, nRole, nDescr, nMdns;
    int fetch;
    solariStatus rc;

    if (out) memset(out, 0, sizeof *out);
    if (!conn || !out) return ERR_INVALID_ARG;

    rc = dbStmtPrepare(conn, SQL, &st);
    if (rc != SOLARI_OK) return rc;
    dbBindU64(&pin[0], &vId);
    if (mysql_stmt_bind_param(st, pin) != 0) { mysql_stmt_close(st); return ERR_DB; }
    if (mysql_stmt_execute(st) != 0)         { mysql_stmt_close(st); return ERR_DB; }

    memset(rout, 0, sizeof rout);
    dbBindOutStr(&rout[0], out->host, sizeof out->host, &lHost, &nHost);
    dbBindOutStr(&rout[1], out->ip, sizeof out->ip, &lIp, &nIp);
    dbBindOutStr(&rout[2], out->kind, sizeof out->kind, &lKind, &nKind);
    dbBindOutStr(&rout[3], out->via, sizeof out->via, &lVia, &nVia);
    dbBindOutStr(&rout[4], out->servicesJson, sizeof out->servicesJson, &lSvc, &nSvc);
    dbBindOutStr(&rout[5], out->segId, sizeof out->segId, &lSeg, &nSeg);
    dbBindOutStr(&rout[6], out->arch, sizeof out->arch, &lArch, &nArch);
    dbBindOutStr(&rout[7], out->mac, sizeof out->mac, &lMac, &nMac);
    dbBindOutStr(&rout[8], out->vendor, sizeof out->vendor, &lVendor, &nVendor);
    dbBindOutStr(&rout[9], out->osName, sizeof out->osName, &lOs, &nOs);
    dbBindOutStr(&rout[10], out->deviceRole, sizeof out->deviceRole, &lRole, &nRole);
    dbBindOutStr(&rout[11], out->sysDescr, sizeof out->sysDescr, &lDescr, &nDescr);
    dbBindOutStr(&rout[12], out->mdnsName, sizeof out->mdnsName, &lMdns, &nMdns);
    if (mysql_stmt_bind_result(st, rout) != 0) { mysql_stmt_close(st); return ERR_DB; }
    if (mysql_stmt_store_result(st) != 0)      { mysql_stmt_close(st); return ERR_DB; }

    fetch = mysql_stmt_fetch(st);
    mysql_stmt_free_result(st);
    mysql_stmt_close(st);
    if (fetch != 0) return ERR_DB;   /* no such row */
    return SOLARI_OK;
}

/* ===================================================================== */
/* topology (Handoff §5 networkGear, lldpEdge)                            */
/* ===================================================================== */

solariStatus serverDbWriteLldpEdge(serverDb *db, const serverLldpEdge *edge,
                                   uint64_t whenUnixMs)
{
    static const char *SQL =
        "INSERT INTO lldpEdge "
        "  (nodeId, gearId, localIf, peerPort, linkType, speedMbps, rssi, "
        "   viaLldp, observedAt) "
        "VALUES (?, ?, ?, ?, ?, ?, ?, ?, FROM_UNIXTIME(? / 1000))";
    MYSQL      *conn = dbConn(db);
    MYSQL_STMT *st;
    MYSQL_BIND  b[9];
    unsigned long long vNode = edge ? edge->nodeId : 0;
    unsigned long long vWhen = whenUnixMs ? whenUnixMs : solariNowUnixMs();
    int vSpeed, vRssi, vVia;
    unsigned long lGear, lLocal, lPeer, lLink;
    solariStatus rc;

    if (!conn || !edge) return ERR_INVALID_ARG;
    vSpeed = edge->speedMbps;
    vRssi  = edge->rssi;
    vVia   = edge->viaLldp ? 1 : 0;

    rc = dbStmtPrepare(conn, SQL, &st);
    if (rc != SOLARI_OK) return rc;
    if (edge->nodeId == 0) { memset(&b[0], 0, sizeof b[0]); b[0].buffer_type = MYSQL_TYPE_NULL; }
    else                   dbBindU64(&b[0], &vNode);
    dbBindStr(&b[1], (edge->gearId[0]) ? edge->gearId : NULL, &lGear);
    dbBindStr(&b[2], (edge->localIf[0]) ? edge->localIf : NULL, &lLocal);
    dbBindStr(&b[3], (edge->peerPort[0]) ? edge->peerPort : NULL, &lPeer);
    dbBindStr(&b[4], (edge->linkType[0]) ? edge->linkType : "wired", &lLink);
    dbBindI32(&b[5], &vSpeed);
    dbBindI32(&b[6], &vRssi);
    dbBindI32(&b[7], &vVia);
    dbBindU64(&b[8], &vWhen);
    rc = dbStmtRunWrite(st, b, 9);
    mysql_stmt_close(st);
    return rc;
}

solariStatus serverDbUpsertNetGear(serverDb *db, const serverNetGear *gear,
                                   uint64_t whenUnixMs)
{
    static const char *SQL =
        "INSERT INTO networkGear "
        "  (gearId, name, kind, model, segId, ports, uplinkGearId, "
        "   wireless, mgmtIp, lastSeenAt) "
        "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, FROM_UNIXTIME(? / 1000)) "
        "ON DUPLICATE KEY UPDATE "
        "  name=VALUES(name), kind=VALUES(kind), model=VALUES(model), "
        "  segId=VALUES(segId), ports=VALUES(ports), "
        "  uplinkGearId=VALUES(uplinkGearId), wireless=VALUES(wireless), "
        "  mgmtIp=VALUES(mgmtIp), lastSeenAt=VALUES(lastSeenAt)";
    MYSQL      *conn = dbConn(db);
    MYSQL_STMT *st;
    MYSQL_BIND  b[10];
    unsigned long long vWhen = whenUnixMs ? whenUnixMs : solariNowUnixMs();
    int vPorts, vWireless;
    unsigned long lGear, lName, lKind, lModel, lSeg, lUplink, lMgmt;
    solariStatus rc;

    if (!conn || !gear) return ERR_INVALID_ARG;
    if (gear->gearId[0] == '\0') return ERR_INVALID_ARG;
    vPorts    = gear->ports;
    vWireless = gear->wireless ? 1 : 0;

    rc = dbStmtPrepare(conn, SQL, &st);
    if (rc != SOLARI_OK) return rc;
    dbBindStr(&b[0], gear->gearId, &lGear);
    dbBindStr(&b[1], (gear->name[0]) ? gear->name : gear->gearId, &lName);
    dbBindStr(&b[2], (gear->kind[0]) ? gear->kind : "other", &lKind);
    dbBindStr(&b[3], (gear->model[0]) ? gear->model : NULL, &lModel);
    dbBindStr(&b[4], (gear->segId[0]) ? gear->segId : NULL, &lSeg);
    dbBindI32(&b[5], &vPorts);
    dbBindStr(&b[6], (gear->uplinkGearId[0]) ? gear->uplinkGearId : NULL, &lUplink);
    dbBindI32(&b[7], &vWireless);
    dbBindStr(&b[8], (gear->mgmtIp[0]) ? gear->mgmtIp : NULL, &lMgmt);
    dbBindU64(&b[9], &vWhen);
    rc = dbStmtRunWrite(st, b, 10);
    mysql_stmt_close(st);
    return rc;
}

/* ===================================================================== */
/* enrollment CRUD (Handoff §5 enrollment, §7.3)                          */
/* ===================================================================== */

solariStatus serverDbEnrollCreate(serverDb *db, const serverEnrollment *e,
                                  uint64_t whenUnixMs, uint64_t *enrId)
{
    static const char *SQL =
        "INSERT INTO enrollment (host, ip, role, certFp, status, requestedAt) "
        "VALUES (?, ?, ?, ?, ?, FROM_UNIXTIME(? / 1000))";
    MYSQL      *conn = dbConn(db);
    MYSQL_STMT *st;
    MYSQL_BIND  b[6];
    unsigned long long vWhen = whenUnixMs ? whenUnixMs : solariNowUnixMs();
    unsigned long lHost, lIp, lRole, lFp, lStat;
    solariStatus rc;

    if (enrId) *enrId = 0;
    if (!conn || !e) return ERR_INVALID_ARG;

    rc = dbStmtPrepare(conn, SQL, &st);
    if (rc != SOLARI_OK) return rc;
    dbBindStr(&b[0], (e->host[0]) ? e->host : NULL, &lHost);
    dbBindStr(&b[1], (e->ip[0]) ? e->ip : NULL, &lIp);
    dbBindStr(&b[2], dbRoleStr(e->role), &lRole);
    dbBindStr(&b[3], (e->certFp[0]) ? e->certFp : NULL, &lFp);
    dbBindStr(&b[4], dbEnrollStatusStr(e->status), &lStat);
    dbBindU64(&b[5], &vWhen);
    rc = dbStmtRunWrite(st, b, 6);
    if (rc == SOLARI_OK && enrId) *enrId = mysql_stmt_insert_id(st);
    mysql_stmt_close(st);
    return rc;
}

static int dbIsTokenHash(const char *s)
{
    size_t i;
    if (!s || strlen(s) != 64) return 0;
    for (i = 0; i < 64; ++i) {
        char c = s[i];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') ||
              (c >= 'A' && c <= 'F'))) return 0;
    }
    return 1;
}

solariStatus serverDbEnrollCreateToken(serverDb *db, const serverEnrollment *e,
                                       const char *tokenHashHex,
                                       uint64_t expiresUnixMs,
                                       uint64_t whenUnixMs, uint64_t *enrId)
{
    static const char *SQL =
        "INSERT INTO enrollment (host, ip, role, certFp, status, requestedAt, "
        "  enrollTokenHash, tokenExpiresAt) "
        "VALUES (?, ?, ?, NULL, 'token', FROM_UNIXTIME(? / 1000), ?, "
        "  FROM_UNIXTIME(? / 1000))";
    MYSQL *conn = dbConn(db);
    MYSQL_STMT *st;
    MYSQL_BIND b[6];
    unsigned long long vWhen = whenUnixMs ? whenUnixMs : solariNowUnixMs();
    unsigned long long vExpires = expiresUnixMs;
    unsigned long lHost, lIp, lRole, lHash;
    solariStatus rc;

    if (enrId) *enrId = 0;
    if (!conn || !e || !dbIsTokenHash(tokenHashHex) ||
        expiresUnixMs <= (whenUnixMs ? whenUnixMs : solariNowUnixMs()) ||
        e->status != ENROLL_TOKEN || e->host[0] == '\0') return ERR_INVALID_ARG;
    rc = dbStmtPrepare(conn, SQL, &st);
    if (rc != SOLARI_OK) return rc;
    dbBindStr(&b[0], e->host, &lHost);
    dbBindStr(&b[1], e->ip[0] ? e->ip : NULL, &lIp);
    dbBindStr(&b[2], dbRoleStr(e->role), &lRole);
    dbBindU64(&b[3], &vWhen);
    dbBindStr(&b[4], tokenHashHex, &lHash);
    dbBindU64(&b[5], &vExpires);
    rc = dbStmtRunWrite(st, b, 6);
    if (rc == SOLARI_OK && enrId) *enrId = mysql_stmt_insert_id(st);
    mysql_stmt_close(st);
    return rc;
}

solariStatus serverDbEnrollSetCsr(serverDb *db, uint64_t enrId,
                                  const char *csrPem, const char *certFp)
{
    static const char *SQL =
        "UPDATE enrollment SET csrPem = ?, certFp = ?, status = 'pending' "
        "WHERE enrId = ? AND status = 'token'";
    MYSQL      *conn = dbConn(db);
    MYSQL_STMT *st;
    MYSQL_BIND  b[3];
    unsigned long long vId = enrId;
    unsigned long lCsr, lFp;
    solariStatus rc;

    if (!conn || !csrPem) return ERR_INVALID_ARG;
    rc = dbStmtPrepare(conn, SQL, &st);
    if (rc != SOLARI_OK) return rc;
    dbBindStr(&b[0], csrPem, &lCsr);
    dbBindStr(&b[1], (certFp && certFp[0]) ? certFp : NULL, &lFp);
    dbBindU64(&b[2], &vId);
    rc = dbStmtRunWrite(st, b, 3);
    mysql_stmt_close(st);
    return rc;
}

solariStatus serverDbEnrollSubmitToken(serverDb *db, const char *tokenHashHex,
                                       const char *hostFqdn, solariRole role,
                                       const char *csrPem, const char *certFp,
                                       uint64_t nowUnixMs)
{
    static const char *UPDATE =
        "UPDATE enrollment SET csrPem=IF(status='token', ?, csrPem), "
        "  certFp=IF(status='token', ?, certFp), "
        "  status=IF(status='token','pending',status) "
        "WHERE enrollTokenHash=? AND host=? AND role=? AND "
        " ((status='token' AND tokenExpiresAt > FROM_UNIXTIME(? / 1000)) OR "
        "  (status='pending' AND certFp=?))";
    static const char *VERIFY =
        "SELECT enrId FROM enrollment WHERE enrollTokenHash=? AND host=? "
        " AND role=? AND status IN ('pending','approved') AND certFp=? LIMIT 1";
    MYSQL *conn = dbConn(db);
    MYSQL_STMT *st = NULL;
    MYSQL_BIND b[7], pin[4], rout[1];
    unsigned long long vNow = nowUnixMs ? nowUnixMs : solariNowUnixMs();
    unsigned long long vEnrId = 0;
    unsigned long lCsr, lFp, lHash, lHost, lRole, lVerifyFp;
    my_bool nEnr = 0;
    solariStatus rc;
    int fetch;

    if (!conn || !dbIsTokenHash(tokenHashHex) || !hostFqdn || !hostFqdn[0] ||
        !csrPem || !csrPem[0] || !certFp || !certFp[0]) return ERR_INVALID_ARG;
    rc = dbStmtPrepare(conn, UPDATE, &st);
    if (rc != SOLARI_OK) return rc;
    dbBindStr(&b[0], csrPem, &lCsr);
    dbBindStr(&b[1], certFp, &lFp);
    dbBindStr(&b[2], tokenHashHex, &lHash);
    dbBindStr(&b[3], hostFqdn, &lHost);
    dbBindStr(&b[4], dbRoleStr(role), &lRole);
    dbBindU64(&b[5], &vNow);
    dbBindStr(&b[6], certFp, &lVerifyFp);
    rc = dbStmtRunWrite(st, b, 7);
    mysql_stmt_close(st);
    if (rc != SOLARI_OK) return rc;

    /* Confirm the row is pending with the same CSR fingerprint. This makes a
     * retried POST safe after a lost HTTP response while rejecting token reuse
     * for a different host, role, or key. */
    rc = dbStmtPrepare(conn, VERIFY, &st);
    if (rc != SOLARI_OK) return rc;
    dbBindStr(&pin[0], tokenHashHex, &lHash);
    dbBindStr(&pin[1], hostFqdn, &lHost);
    dbBindStr(&pin[2], dbRoleStr(role), &lRole);
    dbBindStr(&pin[3], certFp, &lVerifyFp);
    if (mysql_stmt_bind_param(st, pin) != 0 || mysql_stmt_execute(st) != 0) {
        mysql_stmt_close(st);
        return ERR_DB;
    }
    memset(rout, 0, sizeof rout);
    dbBindOutU64(&rout[0], &vEnrId, &nEnr);
    if (mysql_stmt_bind_result(st, rout) != 0 || mysql_stmt_store_result(st) != 0) {
        mysql_stmt_close(st);
        return ERR_DB;
    }
    fetch = mysql_stmt_fetch(st);
    mysql_stmt_free_result(st);
    mysql_stmt_close(st);
    return fetch == 0 && !nEnr ? SOLARI_OK : ERR_AUTH_ROLE;
}

solariStatus serverDbEnrollGetByToken(serverDb *db, const char *tokenHashHex,
                                      serverEnrollment *out, char *certBuf,
                                      size_t certCap)
{
    static const char *SQL =
        "SELECT enrId, host, ip, role, certFp, status, nodeId, issuedCertPem "
        "FROM enrollment WHERE enrollTokenHash=? AND certAckAt IS NULL";
    MYSQL *conn = dbConn(db);
    MYSQL_STMT *st;
    MYSQL_BIND pin[1], rout[8];
    unsigned long long vId = 0, vNode = 0;
    unsigned long lHost, lIp, lRole, lFp, lStatus, lCert;
    char rRole[12], rStatus[12];
    my_bool nId = 0, nHost = 0, nIp = 0, nRole = 0, nFp = 0;
    my_bool nStatus = 0, nNode = 0, nCert = 0;
    int fetch;
    solariStatus rc;

    if (out) memset(out, 0, sizeof *out);
    if (certBuf && certCap) certBuf[0] = '\0';
    if (!conn || !out || !certBuf || certCap < 2 || !dbIsTokenHash(tokenHashHex))
        return ERR_INVALID_ARG;
    rc = dbStmtPrepare(conn, SQL, &st);
    if (rc != SOLARI_OK) return rc;
    dbBindStr(&pin[0], tokenHashHex, &lFp);
    if (mysql_stmt_bind_param(st, pin) != 0 || mysql_stmt_execute(st) != 0) {
        mysql_stmt_close(st);
        return ERR_DB;
    }
    memset(rout, 0, sizeof rout);
    dbBindOutU64(&rout[0], &vId, &nId);
    dbBindOutStr(&rout[1], out->host, sizeof out->host - 1, &lHost, &nHost);
    dbBindOutStr(&rout[2], out->ip, sizeof out->ip - 1, &lIp, &nIp);
    dbBindOutStr(&rout[3], rRole, sizeof rRole - 1, &lRole, &nRole);
    dbBindOutStr(&rout[4], out->certFp, sizeof out->certFp - 1, &lFp, &nFp);
    dbBindOutStr(&rout[5], rStatus, sizeof rStatus - 1, &lStatus, &nStatus);
    dbBindOutU64(&rout[6], &vNode, &nNode);
    dbBindOutStr(&rout[7], certBuf, (unsigned long)(certCap - 1), &lCert, &nCert);
    if (mysql_stmt_bind_result(st, rout) != 0 || mysql_stmt_store_result(st) != 0) {
        mysql_stmt_close(st);
        return ERR_DB;
    }
    fetch = mysql_stmt_fetch(st);
    if (fetch == MYSQL_DATA_TRUNCATED) {
        mysql_stmt_free_result(st);
        mysql_stmt_close(st);
        return ERR_BUFFER_FULL;
    }
    if (fetch == 0 && !nId && !nStatus) {
        out->enrId = vId;
        out->nodeId = nNode ? 0 : vNode;
        rRole[sizeof rRole - 1] = '\0';
        rStatus[sizeof rStatus - 1] = '\0';
        out->role = dbRoleParse(rRole);
        out->status = dbEnrollStatusParse(rStatus);
        if (!nHost) out->host[lHost < sizeof out->host ? lHost : sizeof out->host - 1] = '\0';
        if (!nIp) out->ip[lIp < sizeof out->ip ? lIp : sizeof out->ip - 1] = '\0';
        if (!nFp) out->certFp[lFp < sizeof out->certFp ? lFp : sizeof out->certFp - 1] = '\0';
        if (nCert) certBuf[0] = '\0';
        else certBuf[lCert < certCap ? lCert : certCap - 1] = '\0';
    }
    mysql_stmt_free_result(st);
    mysql_stmt_close(st);
    if (fetch == MYSQL_NO_DATA) return ERR_AUTH_ROLE;
    return fetch == 0 ? SOLARI_OK : ERR_DB;
}

solariStatus serverDbEnrollAckToken(serverDb *db, const char *tokenHashHex,
                                    uint64_t whenUnixMs)
{
    static const char *SQL =
        "UPDATE enrollment SET certAckAt=FROM_UNIXTIME(? / 1000), "
        "  enrollTokenHash=NULL WHERE enrollTokenHash=? AND status='approved' "
        "  AND issuedCertPem IS NOT NULL AND certAckAt IS NULL";
    MYSQL *conn = dbConn(db);
    MYSQL_STMT *st;
    MYSQL_BIND b[2];
    unsigned long long vWhen = whenUnixMs ? whenUnixMs : solariNowUnixMs();
    unsigned long lHash;
    solariStatus rc;
    if (!conn || !dbIsTokenHash(tokenHashHex)) return ERR_INVALID_ARG;
    rc = dbStmtPrepare(conn, SQL, &st);
    if (rc != SOLARI_OK) return rc;
    dbBindU64(&b[0], &vWhen);
    dbBindStr(&b[1], tokenHashHex, &lHash);
    rc = dbStmtRunWrite(st, b, 2);
    if (rc == SOLARI_OK && mysql_stmt_affected_rows(st) != 1) rc = ERR_AUTH_ROLE;
    mysql_stmt_close(st);
    return rc;
}

solariStatus serverDbEnrollDecide(serverDb *db, uint64_t enrId,
                                  serverEnrollStatus status, const char *decidedBy,
                                  uint64_t whenUnixMs)
{
    static const char *SQL =
        "UPDATE enrollment SET status = ?, decidedBy = ?, "
        "  enrollTokenHash=IF(?='rejected', NULL, enrollTokenHash), "
        "  csrPem=IF(?='rejected', NULL, csrPem), "
        "  decidedAt = FROM_UNIXTIME(? / 1000) "
        "WHERE enrId = ? AND status IN ('token','pending')";
    MYSQL      *conn = dbConn(db);
    MYSQL_STMT *st;
    MYSQL_BIND  b[6];
    unsigned long long vWhen = whenUnixMs ? whenUnixMs : solariNowUnixMs();
    unsigned long long vId = enrId;
    unsigned long lStat, lBy, lStatusAgain, lStatusForCsr;
    const char *statusText = dbEnrollStatusStr(status);
    solariStatus rc;

    if (!conn || (status != ENROLL_APPROVED && status != ENROLL_REJECTED))
        return ERR_INVALID_ARG;
    rc = dbStmtPrepare(conn, SQL, &st);
    if (rc != SOLARI_OK) return rc;
    dbBindStr(&b[0], statusText, &lStat);
    dbBindStr(&b[1], (decidedBy && decidedBy[0]) ? decidedBy : NULL, &lBy);
    dbBindStr(&b[2], statusText, &lStatusAgain);
    dbBindStr(&b[3], statusText, &lStatusForCsr);
    dbBindU64(&b[4], &vWhen);
    dbBindU64(&b[5], &vId);
    rc = dbStmtRunWrite(st, b, 6);
    if (rc == SOLARI_OK && mysql_stmt_affected_rows(st) != 1) rc = ERR_AUTH_ROLE;
    mysql_stmt_close(st);
    return rc;
}

solariStatus serverDbEnrollFinalize(serverDb *db, uint64_t enrId,
                                    uint64_t nodeId, solariRole role,
                                    const char *hostFqdn, const char *certCn,
                                    const char *issuedCertPem,
                                    const char *leafCertFp,
                                    const char *leafCertSerial,
                                    const char *leafIssuerFp,
                                    const char *decidedBy, uint64_t whenUnixMs)
{
    static const char *UPDATE_ENROLL =
        "UPDATE enrollment SET status='approved', decidedBy=?, "
        "  decidedAt=FROM_UNIXTIME(? / 1000), csrPem=NULL, nodeId=?, "
        "  issuedCertPem=? "
        "WHERE enrId=? AND status='pending' AND csrPem IS NOT NULL "
        "  AND issuedCertPem IS NULL";
    MYSQL *conn = dbConn(db);
    MYSQL_STMT *st = NULL;
    MYSQL_BIND b[5];
    MYSQL_BIND certB[4];
    MYSQL_BIND issuanceB[7];
    unsigned long lenFp, lenSerial, lenIssuer, lenIssuedBy;
    unsigned long lenBy, lenCert;
    unsigned long long vWhen = whenUnixMs ? whenUnixMs : solariNowUnixMs();
    unsigned long long vNode = nodeId, vId = enrId;
    solariStatus rc = ERR_DB;

    if (!conn || enrId == 0 || nodeId == 0 || !hostFqdn || !hostFqdn[0] ||
        !certCn || !certCn[0] || !issuedCertPem || !issuedCertPem[0] ||
        !dbHexStringValid(leafCertFp, 64, 64) ||
        !dbHexStringValid(leafCertSerial, 0, 40) ||
        !dbHexStringValid(leafIssuerFp, 64, 64))
        return ERR_INVALID_ARG;
    if (dbTxnBegin(conn) != SOLARI_OK) return ERR_DB;

    /* Claim the pending row first. Its row lock serializes concurrent approval
     * attempts; the loser observes zero affected rows and cannot upsert a node. */
    rc = dbStmtPrepare(conn, UPDATE_ENROLL, &st);
    if (rc != SOLARI_OK) goto finish;
    dbBindStr(&b[0], (decidedBy && decidedBy[0]) ? decidedBy : NULL, &lenBy);
    dbBindU64(&b[1], &vWhen);
    dbBindU64(&b[2], &vNode);
    dbBindStr(&b[3], issuedCertPem, &lenCert);
    dbBindU64(&b[4], &vId);
    rc = dbStmtRunWrite(st, b, 5);
    if (rc != SOLARI_OK) goto finish;
    if (mysql_stmt_affected_rows(st) != 1) {
        rc = ERR_AUTH_ROLE;
        goto finish;
    }
    mysql_stmt_close(st);
    st = NULL;

    rc = serverDbUpsertNode(db, nodeId, role, hostFqdn, certCn, "", "", 0);
    if (rc != SOLARI_OK) goto finish;

    /* Until TLS CRLs are activated fleet-wide, a node with a recorded
     * revocation must not be re-enrolled: NNG 1.x cannot distinguish its old
     * leaf from a replacement by fingerprint after the node row is reactivated.
     * The upsert above holds the node row lock, serializing this check with
     * REVOKE_CERT's exact-identity transaction. Roll back the approval if any
     * certificate for this node has been revoked. */
    rc = dbStmtPrepare(conn,
        "SELECT 1 FROM certificateRevocation WHERE nodeId=? LIMIT 1", &st);
    if (rc != SOLARI_OK) goto finish;
    dbBindU64(&certB[0], &vNode);
    if (mysql_stmt_bind_param(st, certB) != 0 || mysql_stmt_execute(st) != 0 ||
        mysql_stmt_store_result(st) != 0) { rc = ERR_DB; goto finish; }
    rc = mysql_stmt_num_rows(st) ? ERR_AUTH_ROLE : SOLARI_OK;
    mysql_stmt_free_result(st);
    mysql_stmt_close(st);
    st = NULL;
    if (rc != SOLARI_OK) goto finish;

    rc = dbStmtPrepare(conn,
        "INSERT INTO certificateIssuance "
        "(issuerFp,serialHex,leafCertFp,nodeId,enrollmentId,origin,issuedBy,issuedAt) "
        "VALUES (?,?,?,?,?,'enrollment',?,FROM_UNIXTIME(? / 1000))", &st);
    if (rc != SOLARI_OK) goto finish;
    dbBindStr(&issuanceB[0], leafIssuerFp, &lenIssuer);
    dbBindStr(&issuanceB[1], leafCertSerial, &lenSerial);
    dbBindStr(&issuanceB[2], leafCertFp, &lenFp);
    dbBindU64(&issuanceB[3], &vNode);
    dbBindU64(&issuanceB[4], &vId);
    dbBindStr(&issuanceB[5], (decidedBy && decidedBy[0]) ? decidedBy : "unknown",
              &lenIssuedBy);
    dbBindU64(&issuanceB[6], &vWhen);
    rc = dbStmtRunWrite(st, issuanceB, 7);
    if (rc != SOLARI_OK) goto finish;
    if (mysql_stmt_affected_rows(st) != 1) { rc = ERR_AUTH_ROLE; goto finish; }
    mysql_stmt_close(st);
    st = NULL;

    rc = dbStmtPrepare(conn,
        "UPDATE node SET leafCertFp=?, leafCertSerial=?, leafIssuerFp=? WHERE nodeId=?", &st);
    if (rc != SOLARI_OK) goto finish;
    dbBindStr(&certB[0], leafCertFp, &lenFp);
    dbBindStr(&certB[1], leafCertSerial, &lenSerial);
    dbBindStr(&certB[2], leafIssuerFp, &lenIssuer);
    dbBindU64(&certB[3], &vNode);
    rc = dbStmtRunWrite(st, certB, 4);
    if (rc != SOLARI_OK) goto finish;
    if (mysql_stmt_affected_rows(st) != 1) { rc = ERR_DB; goto finish; }
    mysql_stmt_close(st);
    st = NULL;

    rc = SOLARI_OK;

finish:
    if (st) mysql_stmt_close(st);
    return dbTxnFinish(db, conn, rc == SOLARI_OK, rc);
}

solariStatus serverDbEnrollGet(serverDb *db, uint64_t enrId, serverEnrollment *out)
{
    static const char *SQL =
        "SELECT enrId, host, ip, role, certFp, status FROM enrollment "
        "WHERE enrId = ?";
    MYSQL      *conn = dbConn(db);
    MYSQL_STMT *st;
    MYSQL_BIND  pin[1], rout[6];
    unsigned long long vId = enrId, rEnrId = 0;
    char rRole[12], rStatus[12];
    unsigned long lHost, lIp, lRole, lFp, lStat;
    my_bool nEnr, nHost, nIp, nRole, nFp, nStat;
    int fetch;
    solariStatus rc;

    if (out) memset(out, 0, sizeof *out);
    if (!conn || !out) return ERR_INVALID_ARG;

    rc = dbStmtPrepare(conn, SQL, &st);
    if (rc != SOLARI_OK) return rc;
    dbBindU64(&pin[0], &vId);
    if (mysql_stmt_bind_param(st, pin) != 0) { mysql_stmt_close(st); return ERR_DB; }
    if (mysql_stmt_execute(st) != 0)         { mysql_stmt_close(st); return ERR_DB; }

    memset(rout, 0, sizeof rout);
    dbBindOutU64(&rout[0], &rEnrId, &nEnr);
    dbBindOutStr(&rout[1], out->host, sizeof out->host, &lHost, &nHost);
    dbBindOutStr(&rout[2], out->ip, sizeof out->ip, &lIp, &nIp);
    dbBindOutStr(&rout[3], rRole, sizeof rRole, &lRole, &nRole);
    dbBindOutStr(&rout[4], out->certFp, sizeof out->certFp, &lFp, &nFp);
    dbBindOutStr(&rout[5], rStatus, sizeof rStatus, &lStat, &nStat);
    if (mysql_stmt_bind_result(st, rout) != 0) { mysql_stmt_close(st); return ERR_DB; }
    if (mysql_stmt_store_result(st) != 0)      { mysql_stmt_close(st); return ERR_DB; }

    fetch = mysql_stmt_fetch(st);
    if (fetch == 0) {
        out->enrId  = rEnrId;
        rRole[sizeof rRole - 1]     = '\0';
        rStatus[sizeof rStatus - 1] = '\0';
        out->role   = dbRoleParse(rRole);
        out->status = dbEnrollStatusParse(rStatus);
    }
    mysql_stmt_free_result(st);
    mysql_stmt_close(st);
    if (fetch != 0) return ERR_DB;
    return SOLARI_OK;
}

solariStatus serverDbEnrollGetCsr(serverDb *db, uint64_t enrId,
                                  char *csrBuf, size_t cap)
{
    static const char *SQL = "SELECT csrPem FROM enrollment WHERE enrId = ?";
    MYSQL      *conn = dbConn(db);
    MYSQL_STMT *st;
    MYSQL_BIND  pin[1], rout[1];
    unsigned long long vId = enrId;
    unsigned long got = 0;
    my_bool nCsr = 0;
    int fetch;
    solariStatus rc;

    if (csrBuf && cap) csrBuf[0] = '\0';
    if (!conn || !csrBuf || cap == 0) return ERR_INVALID_ARG;

    rc = dbStmtPrepare(conn, SQL, &st);
    if (rc != SOLARI_OK) return rc;
    dbBindU64(&pin[0], &vId);
    if (mysql_stmt_bind_param(st, pin) != 0) { mysql_stmt_close(st); return ERR_DB; }
    if (mysql_stmt_execute(st) != 0)         { mysql_stmt_close(st); return ERR_DB; }

    memset(rout, 0, sizeof rout);
    dbBindOutStr(&rout[0], csrBuf, (unsigned long)(cap - 1), &got, &nCsr);
    if (mysql_stmt_bind_result(st, rout) != 0) { mysql_stmt_close(st); return ERR_DB; }
    if (mysql_stmt_store_result(st) != 0)      { mysql_stmt_close(st); return ERR_DB; }

    fetch = mysql_stmt_fetch(st);
    if (fetch == MYSQL_DATA_TRUNCATED) {
        mysql_stmt_free_result(st);
        mysql_stmt_close(st);
        return ERR_BUFFER_FULL;
    }
    if (fetch == 0 && !nCsr) {
        if (got >= cap) got = (unsigned long)(cap - 1);
        csrBuf[got] = '\0';
    }
    mysql_stmt_free_result(st);
    mysql_stmt_close(st);
    if (fetch != 0) return ERR_DB;   /* no row / NULL csr */
    if (nCsr) return ERR_DB;         /* row exists but CSR already cleared */
    return SOLARI_OK;
}

solariStatus serverDbEnrollClearCsr(serverDb *db, uint64_t enrId)
{
    static const char *SQL = "UPDATE enrollment SET csrPem = NULL WHERE enrId = ?";
    MYSQL      *conn = dbConn(db);
    MYSQL_STMT *st;
    MYSQL_BIND  b[1];
    unsigned long long vId = enrId;
    solariStatus rc;

    if (!conn) return ERR_INVALID_ARG;
    rc = dbStmtPrepare(conn, SQL, &st);
    if (rc != SOLARI_OK) return rc;
    dbBindU64(&b[0], &vId);
    rc = dbStmtRunWrite(st, b, 1);
    mysql_stmt_close(st);
    return rc;
}

/* ===================================================================== */
/* build convergence (Handoff §5 buildArtifact; /api/builds)              */
/* ===================================================================== */

solariStatus serverDbBuildConvergence(serverDb *db, serverBuildRow *out,
                                      size_t *count)
{
    /* Per build, count nodes that have converged: a node whose applied build
     * (matched by arch/os/version) equals this artifact's. We approximate the
     * "converged" signal by joining nodeConfig.appliedEpoch presence against the
     * node's arch/os; the dashboard renders the precise picture. The join is a
     * LEFT JOIN so builds with zero converged nodes still appear. */
    static const char *SQL =
        "SELECT b.buildId, b.arch, b.os, b.version, b.channel, b.sha256, "
        "       b.sizeBytes, "
        "       (SELECT COUNT(*) FROM node n "
        "          JOIN nodeConfig c ON c.nodeId = n.nodeId "
        "         WHERE n.arch = b.arch "
        "           AND c.appliedEpoch IS NOT NULL "
        "           AND c.appliedEpoch = c.targetEpoch) AS nodeCount "
        "FROM buildArtifact b ORDER BY b.publishedAt DESC, b.buildId DESC";
    MYSQL      *conn = dbConn(db);
    MYSQL_STMT *st;
    MYSQL_BIND  rout[8];
    unsigned long long rBuildId = 0, rSize = 0, rNodeCount = 0;
    char rArch[SOLARI_ARCH_MAX], rOs[SOLARI_OSNAME_MAX];
    char rVer[SERVER_VERSION_MAX], rChan[8], rSha[SERVER_SHA256_HEX_LEN + 1];
    unsigned long lArch, lOs, lVer, lChan, lSha;
    my_bool nSize;
    size_t cap, n = 0;
    solariStatus rc;

    if (!out || !count) return ERR_INVALID_ARG;
    cap = *count;
    *count = 0;
    if (!conn) return ERR_INVALID_ARG;
    if (cap == 0) return SOLARI_OK;

    rc = dbStmtPrepare(conn, SQL, &st);
    if (rc != SOLARI_OK) return rc;
    if (mysql_stmt_execute(st) != 0) { mysql_stmt_close(st); return ERR_DB; }

    memset(rout, 0, sizeof rout);
    dbBindOutU64(&rout[0], &rBuildId, NULL);
    dbBindOutStr(&rout[1], rArch, sizeof rArch, &lArch, NULL);
    dbBindOutStr(&rout[2], rOs, sizeof rOs, &lOs, NULL);
    dbBindOutStr(&rout[3], rVer, sizeof rVer, &lVer, NULL);
    dbBindOutStr(&rout[4], rChan, sizeof rChan, &lChan, NULL);
    dbBindOutStr(&rout[5], rSha, sizeof rSha, &lSha, NULL);
    dbBindOutU64(&rout[6], &rSize, &nSize);
    dbBindOutU64(&rout[7], &rNodeCount, NULL);
    if (mysql_stmt_bind_result(st, rout) != 0) { mysql_stmt_close(st); return ERR_DB; }
    if (mysql_stmt_store_result(st) != 0)      { mysql_stmt_close(st); return ERR_DB; }

    while (n < cap && mysql_stmt_fetch(st) == 0) {
        serverBuildRow *r = &out[n];
        memset(r, 0, sizeof *r);
        r->buildId   = rBuildId;
        r->sizeBytes = nSize ? 0 : rSize;
        r->nodeCount = (uint32_t)rNodeCount;
        memcpy(r->arch, rArch, sizeof r->arch);       r->arch[sizeof r->arch - 1] = '\0';
        memcpy(r->os, rOs, sizeof r->os);             r->os[sizeof r->os - 1] = '\0';
        memcpy(r->version, rVer, sizeof r->version);  r->version[sizeof r->version - 1] = '\0';
        memcpy(r->channel, rChan, sizeof r->channel); r->channel[sizeof r->channel - 1] = '\0';
        memcpy(r->sha256, rSha, sizeof r->sha256);    r->sha256[sizeof r->sha256 - 1] = '\0';
        n++;
    }

    mysql_stmt_free_result(st);
    mysql_stmt_close(st);
    *count = n;
    return SOLARI_OK;
}
