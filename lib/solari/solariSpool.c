/*
 * solariSpool.c - SQLite store-and-forward queue (section 6.6).
 *
 * Backed by a single table:
 *   spool(rowId INTEGER PK, frame BLOB, attempts INT, enqueuedAt INT,
 *         nextRetryAt INT)   -- times are Unix ms
 *
 * NOTE: this includes the minimal SQLite declarations in
 * third_party/sqlite/sqlite3_min.h and links the platform libsqlite3. When the
 * full amalgamation is vendored, change the include below to "sqlite3.h".
 */
#include "solari/solariSpool.h"
#include "solari/solariLog.h"
#include "solari/solariTime.h"

#include "sqlite/sqlite3_min.h"

#include <stdlib.h>
#include <string.h>

/* Backoff schedule: base * 2^(attempts-1), capped. */
#define SPOOL_BACKOFF_BASE_MS 1000ull
#define SPOOL_BACKOFF_MAX_MS  60000ull

struct solariSpool {
    sqlite3 *db;
    uint8_t *readBuf;     /* reusable buffer returned by Peek */
    size_t   readCap;
};

static const char *SCHEMA_SQL =
    "CREATE TABLE IF NOT EXISTS spool ("
    "  rowId       INTEGER PRIMARY KEY AUTOINCREMENT,"
    "  frame       BLOB    NOT NULL,"
    "  attempts    INTEGER NOT NULL DEFAULT 0,"
    "  enqueuedAt  INTEGER NOT NULL,"
    "  nextRetryAt INTEGER NOT NULL DEFAULT 0"
    ");";

static uint64_t backoffMs(int attempts)
{
    uint64_t ms = SPOOL_BACKOFF_BASE_MS;
    int i;
    if (attempts <= 1) return SPOOL_BACKOFF_BASE_MS;
    for (i = 1; i < attempts && ms < SPOOL_BACKOFF_MAX_MS; i++) {
        ms <<= 1;
    }
    return ms > SPOOL_BACKOFF_MAX_MS ? SPOOL_BACKOFF_MAX_MS : ms;
}

solariStatus solariSpoolOpen(const char *dbPath, solariSpool **out)
{
    solariSpool *s;
    char *errmsg = NULL;

    if (!dbPath || !out) return ERR_INVALID_ARG;

    s = (solariSpool *)calloc(1, sizeof *s);
    if (!s) return ERR_PLATFORM;

    if (sqlite3_open_v2(dbPath, &s->db,
                        SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, NULL) != SQLITE_OK) {
        solariLogf(SOLARI_LOG_ERROR, "spool: open %s failed: %s",
                   dbPath, s->db ? sqlite3_errmsg(s->db) : "?");
        sqlite3_close(s->db);
        free(s);
        return ERR_DB;
    }
    if (sqlite3_exec(s->db, SCHEMA_SQL, NULL, NULL, &errmsg) != SQLITE_OK) {
        solariLogf(SOLARI_LOG_ERROR, "spool: schema failed: %s", errmsg ? errmsg : "?");
        sqlite3_close(s->db);
        free(s);
        return ERR_DB;
    }
    *out = s;
    return SOLARI_OK;
}

solariStatus solariSpoolPush(solariSpool *s, const uint8_t *frame, size_t len)
{
    sqlite3_stmt *st = NULL;
    solariStatus rc = SOLARI_OK;

    if (!s || !frame || len == 0) return ERR_INVALID_ARG;

    if (sqlite3_prepare_v2(s->db,
            "INSERT INTO spool(frame, attempts, enqueuedAt, nextRetryAt) "
            "VALUES (?, 0, ?, 0);", -1, &st, NULL) != SQLITE_OK) {
        return ERR_DB;
    }
    sqlite3_bind_blob(st, 1, frame, (int)len, SQLITE_TRANSIENT);
    sqlite3_bind_int64(st, 2, (sqlite3_int64)solariNowUnixMs());
    if (sqlite3_step(st) != SQLITE_DONE) {
        solariLogf(SOLARI_LOG_ERROR, "spool: push failed: %s", sqlite3_errmsg(s->db));
        rc = ERR_DB;
    }
    sqlite3_finalize(st);
    return rc;
}

solariStatus solariSpoolPeek(solariSpool *s, uint64_t *rowId,
                             const uint8_t **frame, size_t *len)
{
    sqlite3_stmt *st = NULL;
    int step;
    solariStatus rc = SOLARI_OK;

    if (!s || !rowId || !frame || !len) return ERR_INVALID_ARG;
    *rowId = 0; *frame = NULL; *len = 0;

    if (sqlite3_prepare_v2(s->db,
            "SELECT rowId, frame FROM spool WHERE nextRetryAt <= ? "
            "ORDER BY rowId LIMIT 1;", -1, &st, NULL) != SQLITE_OK) {
        return ERR_DB;
    }
    sqlite3_bind_int64(st, 1, (sqlite3_int64)solariNowUnixMs());

    step = sqlite3_step(st);
    if (step == SQLITE_ROW) {
        const void *blob = sqlite3_column_blob(st, 1);
        int n = sqlite3_column_bytes(st, 1);
        if (n > 0) {
            if ((size_t)n > s->readCap) {
                uint8_t *nb = (uint8_t *)realloc(s->readBuf, (size_t)n);
                if (!nb) { sqlite3_finalize(st); return ERR_PLATFORM; }
                s->readBuf = nb;
                s->readCap = (size_t)n;
            }
            memcpy(s->readBuf, blob, (size_t)n);
            *rowId = (uint64_t)sqlite3_column_int64(st, 0);
            *frame = s->readBuf;
            *len = (size_t)n;
        }
    } else if (step != SQLITE_DONE) {
        rc = ERR_DB;   /* DONE just means the queue had nothing ready */
    }
    sqlite3_finalize(st);
    return rc;
}

solariStatus solariSpoolAck(solariSpool *s, uint64_t rowId)
{
    sqlite3_stmt *st = NULL;
    solariStatus rc = SOLARI_OK;
    if (!s) return ERR_INVALID_ARG;
    if (sqlite3_prepare_v2(s->db, "DELETE FROM spool WHERE rowId = ?;", -1, &st, NULL) != SQLITE_OK)
        return ERR_DB;
    sqlite3_bind_int64(st, 1, (sqlite3_int64)rowId);
    if (sqlite3_step(st) != SQLITE_DONE) rc = ERR_DB;
    sqlite3_finalize(st);
    return rc;
}

solariStatus solariSpoolNack(solariSpool *s, uint64_t rowId)
{
    sqlite3_stmt *st = NULL;
    solariStatus rc = SOLARI_OK;
    int attempts = 1;

    if (!s) return ERR_INVALID_ARG;

    /* read current attempts to compute backoff */
    if (sqlite3_prepare_v2(s->db, "SELECT attempts FROM spool WHERE rowId = ?;",
                           -1, &st, NULL) != SQLITE_OK) {
        return ERR_DB;
    }
    sqlite3_bind_int64(st, 1, (sqlite3_int64)rowId);
    if (sqlite3_step(st) == SQLITE_ROW) {
        attempts = (int)sqlite3_column_int64(st, 0) + 1;
    }
    sqlite3_finalize(st);
    st = NULL;

    if (sqlite3_prepare_v2(s->db,
            "UPDATE spool SET attempts = attempts + 1, nextRetryAt = ? "
            "WHERE rowId = ?;", -1, &st, NULL) != SQLITE_OK) {
        return ERR_DB;
    }
    sqlite3_bind_int64(st, 1, (sqlite3_int64)(solariNowUnixMs() + backoffMs(attempts)));
    sqlite3_bind_int64(st, 2, (sqlite3_int64)rowId);
    if (sqlite3_step(st) != SQLITE_DONE) rc = ERR_DB;
    sqlite3_finalize(st);
    return rc;
}

size_t solariSpoolDepth(solariSpool *s)
{
    sqlite3_stmt *st = NULL;
    size_t depth = 0;
    if (!s) return 0;
    if (sqlite3_prepare_v2(s->db, "SELECT COUNT(*) FROM spool;", -1, &st, NULL) != SQLITE_OK)
        return 0;
    if (sqlite3_step(st) == SQLITE_ROW) {
        depth = (size_t)sqlite3_column_int64(st, 0);
    }
    sqlite3_finalize(st);
    return depth;
}

void solariSpoolClose(solariSpool *s)
{
    if (!s) return;
    if (s->db) sqlite3_close(s->db);
    free(s->readBuf);
    free(s);
}
