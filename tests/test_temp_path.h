#ifndef SOLARINET_TEST_TEMP_PATH_H
#define SOLARINET_TEST_TEMP_PATH_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Test fixtures honor TMPDIR so isolated runners can avoid a full /tmp. */
static inline int solariTestTempPath(char *dst, size_t cap, const char *leaf)
{
    const char *dir = getenv("TMPDIR");
    size_t len;
    const char *separator;
    int written;

    if (!dir || !*dir) dir = "/tmp";
    len = strlen(dir);
    separator = len && dir[len - 1] == '/' ? "" : "/";
    written = snprintf(dst, cap, "%s%s%s", dir, separator, leaf);
    return written >= 0 && (size_t)written < cap;
}

static inline char *solariTestTempDir(char *dst, size_t cap, const char *prefix)
{
    char leaf[128];
    int written = snprintf(leaf, sizeof leaf, "%s.XXXXXX", prefix);
    if (written < 0 || (size_t)written >= sizeof leaf ||
        !solariTestTempPath(dst, cap, leaf)) return NULL;
    return mkdtemp(dst);
}

#endif
