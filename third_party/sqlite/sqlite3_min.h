/*
 * sqlite3_min.h - minimal SQLite C API declarations used by libsolari.
 *
 * This is a STOPGAP. The architecture plan (section 3) calls for vendoring the
 * full SQLite amalgamation (sqlite3.c + sqlite3.h, public domain) under
 * third_party/sqlite/ and compiling it directly. That requires fetching the
 * amalgamation, which needs network access not available in the build sandbox.
 *
 * In the meantime this header declares exactly the subset of the stable SQLite
 * C ABI that solariSpool.c calls, so the spool can be compiled and tested by
 * linking the platform's existing shared library:
 *
 *     gcc ... -l:libsqlite3.so.0
 *
 * The ABI declared here is stable across all SQLite 3.x releases. When the
 * amalgamation is vendored, replace `#include "sqlite/sqlite3_min.h"` in
 * solariSpool.c with `#include "sqlite3.h"` - no other change is needed.
 */
#ifndef SQLITE3_MIN_H
#define SQLITE3_MIN_H

#include <stdint.h>

typedef struct sqlite3 sqlite3;
typedef struct sqlite3_stmt sqlite3_stmt;
typedef int64_t sqlite3_int64;

/* result codes */
#define SQLITE_OK    0
#define SQLITE_ROW   100
#define SQLITE_DONE  101

/* open flags */
#define SQLITE_OPEN_READWRITE 0x00000002
#define SQLITE_OPEN_CREATE    0x00000004

/* sentinel destructors for *_bind_* */
#define SQLITE_STATIC    ((void (*)(void *))0)
#define SQLITE_TRANSIENT ((void (*)(void *))-1)

int  sqlite3_open_v2(const char *filename, sqlite3 **ppDb, int flags, const char *zVfs);
int  sqlite3_close(sqlite3 *);
int  sqlite3_exec(sqlite3 *, const char *sql,
                  int (*callback)(void *, int, char **, char **),
                  void *, char **errmsg);
const char *sqlite3_errmsg(sqlite3 *);

int  sqlite3_prepare_v2(sqlite3 *db, const char *zSql, int nByte,
                        sqlite3_stmt **ppStmt, const char **pzTail);
int  sqlite3_step(sqlite3_stmt *);
int  sqlite3_reset(sqlite3_stmt *);
int  sqlite3_finalize(sqlite3_stmt *);

int  sqlite3_bind_blob(sqlite3_stmt *, int, const void *, int n, void (*)(void *));
int  sqlite3_bind_int64(sqlite3_stmt *, int, sqlite3_int64);

const void *sqlite3_column_blob(sqlite3_stmt *, int iCol);
int         sqlite3_column_bytes(sqlite3_stmt *, int iCol);
sqlite3_int64 sqlite3_column_int64(sqlite3_stmt *, int iCol);

sqlite3_int64 sqlite3_last_insert_rowid(sqlite3 *);
const char *sqlite3_libversion(void);

#endif /* SQLITE3_MIN_H */
