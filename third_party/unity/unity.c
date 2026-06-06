/*
 * unity.c - minimal Unity-compatible runner. See README.md.
 */
#include "unity.h"
#include <stdio.h>

static int   gTests = 0;
static int   gFailures = 0;
static int   gCurrentFailed = 0;
static const char *gFile = "";

void unityBegin(const char *file)
{
    gFile = file;
    gTests = 0;
    gFailures = 0;
    printf("Unity (shim) running: %s\n", file);
}

int unityEnd(void)
{
    printf("-----------------------\n");
    printf("%d Tests %d Failures\n", gTests, gFailures);
    printf("%s\n", gFailures ? "FAIL" : "OK");
    return gFailures;
}

void unityDefaultTestRun(void (*func)(void), const char *name, int line)
{
    (void)line;
    gCurrentFailed = 0;
    gTests++;
    setUp();
    func();
    tearDown();
    if (gCurrentFailed) {
        gFailures++;
        printf("  [FAIL] %s\n", name);
    } else {
        printf("  [PASS] %s\n", name);
    }
}

static void reportFail(int line, const char *msg)
{
    gCurrentFailed = 1;
    printf("    %s:%d: %s\n", gFile, line, msg);
}

void unityFail(const char *msg, int line)
{
    reportFail(line, msg ? msg : "TEST_FAIL");
}

void unityAssertEqualNumber(int64_t expected, int64_t actual, int line, const char *msg)
{
    if (expected != actual) {
        char b[160];
        snprintf(b, sizeof b, "%s (expected %lld, got %lld)",
                 msg, (long long)expected, (long long)actual);
        reportFail(line, b);
    }
}

void unityAssertEqualUNumber(uint64_t expected, uint64_t actual, int line, const char *msg)
{
    if (expected != actual) {
        char b[160];
        snprintf(b, sizeof b, "%s (expected %llu, got %llu)",
                 msg, (unsigned long long)expected, (unsigned long long)actual);
        reportFail(line, b);
    }
}

void unityAssertBits(uint64_t mask, uint64_t expected, uint64_t actual, int line)
{
    if ((expected & mask) != (actual & mask)) {
        reportFail(line, "bit mask mismatch");
    }
}

void unityAssertEqualString(const char *expected, const char *actual, int line)
{
    if (expected == NULL || actual == NULL) {
        if (expected != actual) reportFail(line, "string NULL mismatch");
        return;
    }
    if (strcmp(expected, actual) != 0) {
        char b[256];
        snprintf(b, sizeof b, "string mismatch (expected \"%s\", got \"%s\")", expected, actual);
        reportFail(line, b);
    }
}

void unityAssertEqualMemory(const void *expected, const void *actual, size_t len, int line)
{
    if (memcmp(expected, actual, len) != 0) {
        size_t i;
        const unsigned char *e = (const unsigned char *)expected;
        const unsigned char *a = (const unsigned char *)actual;
        for (i = 0; i < len; i++) {
            if (e[i] != a[i]) {
                char b[128];
                snprintf(b, sizeof b, "memory mismatch at byte %lu (expected 0x%02x, got 0x%02x)",
                         (unsigned long)i, e[i], a[i]);
                reportFail(line, b);
                return;
            }
        }
    }
}

void unityAssertNotNull(const void *ptr, int line, const char *msg)
{
    if (ptr == NULL) reportFail(line, msg);
}

void unityAssertNull(const void *ptr, int line, const char *msg)
{
    if (ptr != NULL) reportFail(line, msg);
}
