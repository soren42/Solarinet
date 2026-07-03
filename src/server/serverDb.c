/*
 * serverDb.c - MariaDB persistence layer (§9.1, §10; Handoff §5).
 *
 * Owns the MariaDB Connector/C connection lifecycle, prepared-statement
 * helpers, schema apply, and every data-access function the other subsystems
 * call: node upsert/touch/state, client/monitor report writes (upsert current +
 * append history in one txn), alertEvent/audit append + rule load, nodeConfig
 * target/applied epochs, the serverLease conditional-UPDATE election, discovered
 * upsert/status/known/get, lldpEdge + networkGear writes, enrollment CRUD with
 * CSR hold/clear, and the buildArtifact x nodeConfig convergence join. All time
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

#include <mariadb/mysql.h>
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

/* Clamp the configured pool size into [1, 16]. */
static uint8_t dbClampPoolSize(uint8_t requested)
{
    if (requested == 0) return 1;
    if (requested > 16) return 16;
    return requested;
}

/* Hand back a live connection from the pool. The core is single-threaded, so
 * the primary slot is always the right answer; NULL means the handle is dead. */
static MYSQL *dbConn(serverDb *db)
{
    if (!db || db->poolSize == 0) return NULL;
    return db->conns[0];
}

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
        MYSQL    *conn = mysql_init(NULL);
        my_bool   reconnect = 1;
        unsigned  timeout = 5;
        const char *sock = (cfg->dbSocket[0] != '\0') ? cfg->dbSocket : NULL;

        if (!conn) {
            solariLogf(SOLARI_LOG_ERROR, "serverDb: mysql_init failed (slot %u)", i);
            serverDbClose(db);
            return ERR_DB;
        }
        /* Best-effort auto-reconnect and a bounded connect timeout so a wedged
         * DB does not block the whole server forever. */
        mysql_options(conn, MYSQL_OPT_RECONNECT, &reconnect);
        mysql_options(conn, MYSQL_OPT_CONNECT_TIMEOUT, &timeout);

        if (!mysql_real_connect(conn, cfg->dbHost, cfg->dbUser, cfg->dbPass,
                                cfg->dbName, cfg->dbPort, sock,
                                CLIENT_MULTI_STATEMENTS)) {
            solariLogf(SOLARI_LOG_ERROR,
                       "serverDb: connect to %s:%u/%s failed: %s",
                       cfg->dbHost[0] ? cfg->dbHost : "(socket)",
                       (unsigned)cfg->dbPort, cfg->dbName, mysql_error(conn));
            mysql_close(conn);
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

solariStatus serverDbUpsertProbeTarget(serverDb *db, const char *targetId,
                                       const char *host, int port,
                                       const char *proto, int replFactor,
                                       const char *label, const char *segId,
                                       uint64_t assetId)
{
    static const char *SQL =
        "INSERT INTO probeTarget (targetId, host, port, proto, replFactor, label, segId, assetId) "
        "VALUES (?, ?, ?, ?, ?, ?, ?, ?) "
        "ON DUPLICATE KEY UPDATE host=VALUES(host), port=VALUES(port), "
        "  proto=VALUES(proto), replFactor=VALUES(replFactor), "
        "  label=VALUES(label), segId=VALUES(segId), assetId=VALUES(assetId)";
    MYSQL      *conn = dbConn(db);
    MYSQL_STMT *st;
    MYSQL_BIND  b[8];
    int          vPort = port, vRepl = replFactor;
    unsigned long long vAsset = assetId;
    unsigned long lTid, lHost, lProto, lLabel, lSeg;
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
    rc = dbStmtRunWrite(st, b, 8);
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

solariStatus serverDbWriteClientReport(serverContext *ctx, uint64_t nodeId,
                                       const solariClientReport *rep)
{
    static const char *SQL_CUR =
        "INSERT INTO hostCurrent "
        "  (nodeId, sampledAt, cpuLoadMilli, ramUsedKb, ramTotalKb, "
        "   swapUsedKb, swapTotalKb, disks, ifaces, usbBuses) "
        "VALUES (?, UTC_TIMESTAMP(), ?, ?, ?, ?, ?, ?, ?, ?) "
        "ON DUPLICATE KEY UPDATE "
        "  sampledAt=UTC_TIMESTAMP(), cpuLoadMilli=VALUES(cpuLoadMilli), "
        "  ramUsedKb=VALUES(ramUsedKb), ramTotalKb=VALUES(ramTotalKb), "
        "  swapUsedKb=VALUES(swapUsedKb), swapTotalKb=VALUES(swapTotalKb), "
        "  disks=VALUES(disks), ifaces=VALUES(ifaces), usbBuses=VALUES(usbBuses)";
    static const char *SQL_HIST =
        "INSERT INTO hostHistory "
        "  (nodeId, sampledAt, cpuAvgMilli, ramUsedKb, swapUsedKb, diskMinFreePct, netKbps) "
        "VALUES (?, UTC_TIMESTAMP(), ?, ?, ?, ?, ?)";
    MYSQL      *conn;
    MYSQL_STMT *st;
    MYSQL_BIND  b[10];
    unsigned long long vNode, vRamU, vRamT, vSwapU, vSwapT, vNet;
    int cpuAvg, diskMin;
    unsigned long lCpu, lDisks, lIfaces, lUsb;
    char cpuJson[512], disksJson[4096], ifacesJson[2048];
    static const char *usbJson = "[]";
    solariStatus rc;

    if (!ctx || !ctx->db || !rep) return ERR_INVALID_ARG;
    conn = dbConn(ctx->db);
    if (!conn) return ERR_DB;

    vNode  = nodeId;
    vRamU  = rep->ramUsedKb;  vRamT = rep->ramTotalKb;
    vSwapU = rep->swapUsedKb; vSwapT = rep->swapTotalKb;
    cpuAvg = (int)dbAvgCpuMilli(rep);
    diskMin = dbMinDiskFreePct(rep);
    vNet = dbSumNetKbps(rep);
    dbJsonCpu(rep, cpuJson, sizeof cpuJson);
    dbJsonDisks(rep, disksJson, sizeof disksJson);
    dbJsonIfaces(rep, ifacesJson, sizeof ifacesJson);

    /* One transaction: upsert current (+ JSON detail) + procs + history (§9.1). */
    if (mysql_autocommit(conn, 0) != 0) return ERR_DB;

    rc = dbStmtPrepare(conn, SQL_CUR, &st);
    if (rc != SOLARI_OK) { mysql_rollback(conn); mysql_autocommit(conn, 1); return rc; }
    dbBindU64(&b[0], &vNode);
    dbBindStr(&b[1], cpuJson,   &lCpu);
    dbBindU64(&b[2], &vRamU);
    dbBindU64(&b[3], &vRamT);
    dbBindU64(&b[4], &vSwapU);
    dbBindU64(&b[5], &vSwapT);
    dbBindStr(&b[6], disksJson,  &lDisks);
    dbBindStr(&b[7], ifacesJson, &lIfaces);
    dbBindStr(&b[8], usbJson,    &lUsb);
    rc = dbStmtRunWrite(st, b, 9);
    mysql_stmt_close(st);
    if (rc != SOLARI_OK) { mysql_rollback(conn); mysql_autocommit(conn, 1); return rc; }

    rc = dbWriteProcs(conn, &vNode, rep);
    if (rc != SOLARI_OK) { mysql_rollback(conn); mysql_autocommit(conn, 1); return rc; }

    rc = dbStmtPrepare(conn, SQL_HIST, &st);
    if (rc != SOLARI_OK) { mysql_rollback(conn); mysql_autocommit(conn, 1); return rc; }
    dbBindU64(&b[0], &vNode);
    dbBindI32(&b[1], &cpuAvg);
    dbBindU64(&b[2], &vRamU);
    dbBindU64(&b[3], &vSwapU);
    dbBindI32(&b[4], &diskMin);
    dbBindU64(&b[5], &vNet);
    rc = dbStmtRunWrite(st, b, 6);
    mysql_stmt_close(st);
    if (rc != SOLARI_OK) { mysql_rollback(conn); mysql_autocommit(conn, 1); return rc; }

    if (mysql_commit(conn) != 0) {
        solariLogf(SOLARI_LOG_ERROR, "serverDb: client commit failed: %s",
                   mysql_error(conn));
        mysql_rollback(conn);
        mysql_autocommit(conn, 1);
        return ERR_DB;
    }
    mysql_autocommit(conn, 1);
    return SOLARI_OK;
}

solariStatus serverDbWriteMonitorReport(serverContext *ctx, uint64_t nodeId,
                                        const solariMonitorReport *rep)
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

    if (!ctx || !ctx->db || !rep) return ERR_INVALID_ARG;
    conn = dbConn(ctx->db);
    if (!conn) return ERR_DB;

    if (mysql_autocommit(conn, 0) != 0) return ERR_DB;

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

    if (rc != SOLARI_OK) {
        mysql_rollback(conn);
        mysql_autocommit(conn, 1);
        return rc;
    }
    if (mysql_commit(conn) != 0) {
        solariLogf(SOLARI_LOG_ERROR, "serverDb: monitor commit failed: %s",
                   mysql_error(conn));
        mysql_rollback(conn);
        mysql_autocommit(conn, 1);
        return ERR_DB;
    }
    mysql_autocommit(conn, 1);
    return SOLARI_OK;

fail_nostmt:
    mysql_rollback(conn);
    mysql_autocommit(conn, 1);
    return rc;
}

/* ===================================================================== */
/* alerts / audit (§10 alertEvent, alertRule)                             */
/* ===================================================================== */

solariStatus serverDbWriteAlertEvent(serverDb *db, int ruleId, uint64_t nodeId,
                                     const char *targetId, const char *severity,
                                     const char *detail, uint64_t firedUnixMs)
{
    static const char *SQL =
        "INSERT INTO alertEvent "
        "  (ruleId, nodeId, targetId, firedAt, severity, detail) "
        "VALUES (?, ?, ?, FROM_UNIXTIME(? / 1000), ?, ?)";
    MYSQL      *conn = dbConn(db);
    MYSQL_STMT *st;
    MYSQL_BIND  b[6];
    int                vRule = ruleId;
    unsigned long long vNode = nodeId;
    unsigned long long vWhen = firedUnixMs ? firedUnixMs : solariNowUnixMs();
    unsigned long tLen, sLen, dLen;
    solariStatus rc;

    if (!conn) return ERR_INVALID_ARG;
    rc = dbStmtPrepare(conn, SQL, &st);
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

    rc = dbStmtRunWrite(st, b, 6);
    mysql_stmt_close(st);
    return rc;
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

/* ===================================================================== */
/* lease ops (§10 serverLease) - the conditional UPDATE IS the election    */
/* ===================================================================== */

solariStatus serverDbLeaseClaim(serverDb *db, uint64_t selfNodeId,
                                uint32_t ttlSec, bool *won, uint64_t *epoch)
{
    /* Single-row election (id=1). Two paths in one txn:
     *  (a) renew/claim when we already hold it OR it has expired OR is unheld:
     *      bump expiry, advance epoch only on a *fresh* takeover.
     *  (b) lose when another live holder owns it.
     * The atomic UPDATE's affected-rows tells us whether we won. */
    static const char *SQL_UPSERT =
        "INSERT INTO serverLease (id, leaseHolder, leaseEpoch, expiresAt) "
        "VALUES (1, ?, 1, UTC_TIMESTAMP(3) + INTERVAL ? SECOND) "
        "ON DUPLICATE KEY UPDATE "
        "  leaseEpoch = leaseEpoch + IF(leaseHolder = VALUES(leaseHolder), 0, 1), "
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

/* ===================================================================== */
/* discovery (Handoff §5 discovered)                                      */
/* ===================================================================== */

solariStatus serverDbUpsertDiscovered(serverDb *db, const serverDiscEntity *e,
                                      uint64_t whenUnixMs, uint64_t *discId)
{
    static const char *SQL =
        "INSERT INTO discovered "
        "  (host, ip, kind, via, services, segId, arch, "
        "   seenCount, firstSeenAt, lastSeenAt) "
        "VALUES (?, ?, ?, ?, ?, ?, ?, 1, "
        "        FROM_UNIXTIME(? / 1000), FROM_UNIXTIME(? / 1000)) "
        "ON DUPLICATE KEY UPDATE "
        "  host=VALUES(host), via=VALUES(via), services=VALUES(services), "
        "  segId=VALUES(segId), arch=VALUES(arch), "
        "  seenCount = seenCount + 1, lastSeenAt = VALUES(lastSeenAt)";
    MYSQL      *conn = dbConn(db);
    MYSQL_STMT *st;
    MYSQL_BIND  b[9];
    unsigned long long vWhen = whenUnixMs ? whenUnixMs : solariNowUnixMs();
    unsigned long long vWhen2 = vWhen;
    unsigned long lHost, lIp, lKind, lVia, lSvc, lSeg, lArch;
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
    dbBindU64(&b[7], &vWhen);
    dbBindU64(&b[8], &vWhen2);
    rc = dbStmtRunWrite(st, b, 9);
    if (rc == SOLARI_OK && discId) {
        unsigned long long id = mysql_stmt_insert_id(st);
        *discId = id;   /* 0 on a pure UPDATE; caller may re-resolve by (ip,kind) */
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
        "SELECT host, ip, kind, via, services, segId, arch "
        "FROM discovered WHERE discId = ?";
    MYSQL      *conn = dbConn(db);
    MYSQL_STMT *st;
    MYSQL_BIND  pin[1], rout[7];
    unsigned long long vId = discId;
    unsigned long lHost, lIp, lKind, lVia, lSvc, lSeg, lArch;
    my_bool nHost, nIp, nKind, nVia, nSvc, nSeg, nArch;
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

solariStatus serverDbEnrollDecide(serverDb *db, uint64_t enrId,
                                  serverEnrollStatus status, const char *decidedBy,
                                  uint64_t whenUnixMs)
{
    static const char *SQL =
        "UPDATE enrollment SET status = ?, decidedBy = ?, "
        "  decidedAt = FROM_UNIXTIME(? / 1000) WHERE enrId = ?";
    MYSQL      *conn = dbConn(db);
    MYSQL_STMT *st;
    MYSQL_BIND  b[4];
    unsigned long long vWhen = whenUnixMs ? whenUnixMs : solariNowUnixMs();
    unsigned long long vId = enrId;
    unsigned long lStat, lBy;
    solariStatus rc;

    if (!conn) return ERR_INVALID_ARG;
    rc = dbStmtPrepare(conn, SQL, &st);
    if (rc != SOLARI_OK) return rc;
    dbBindStr(&b[0], dbEnrollStatusStr(status), &lStat);
    dbBindStr(&b[1], (decidedBy && decidedBy[0]) ? decidedBy : NULL, &lBy);
    dbBindU64(&b[2], &vWhen);
    dbBindU64(&b[3], &vId);
    rc = dbStmtRunWrite(st, b, 4);
    mysql_stmt_close(st);
    return rc;
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
