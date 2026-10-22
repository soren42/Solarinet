/* Fault-inject the real config persistence path before its convergence reply. */
#define _POSIX_C_SOURCE 200809L
#include "unity.h"
#include "clientControl.h"
#include "solari/solariMsg.h"
#include "solari/solariTlv.h"
#include "../test_temp_path.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static char testDir[512];
static clientConfig cfg;
static clientControlState state;
static unsigned failSyncNth;
static int failRename;

int __real_fsync(int fd);
int __real_rename(const char *from, const char *to);
int __wrap_fsync(int fd)
{
    if (failSyncNth && --failSyncNth == 0) { errno = EIO; return -1; }
    return __real_fsync(fd);
}
int __wrap_rename(const char *from, const char *to)
{
    if (failRename) { errno = EIO; return -1; }
    return __real_rename(from, to);
}

void setUp(void)
{
    char statePath[512];
    int written;
    TEST_ASSERT_NOT_NULL(solariTestTempDir(testDir, sizeof testDir,
                                           "solari-control-test"));
    clientConfigDefaults(&cfg);
    written = snprintf(statePath, sizeof statePath, "%s/state", testDir);
    TEST_ASSERT_TRUE(written >= 0 && (size_t)written < sizeof statePath);
    TEST_ASSERT_TRUE((size_t)written < sizeof cfg.ctrlStateFile);
    memcpy(cfg.ctrlStateFile, statePath, (size_t)written + 1);
    state.appliedEpoch = 3;
    failSyncNth = 0;
    failRename = 0;
}

void tearDown(void)
{
    unlink(cfg.ctrlStateFile);
    rmdir(testDir);
}

/* Submit epoch 7 and assert that its reply reports the actual final state. */
static solariStatus applyPatch(uint64_t targetEpoch, const char *blob)
{
    solariControl directive;
    clientControlOutcome outcome;
    uint8_t wire[512], reply[CLIENT_CTRL_RESULT_CAP];
    size_t length, replyLength;
    uint16_t count, replyCount;
    solariStatus rc;
    memset(&directive, 0, sizeof directive);
    directive.verb = CTRL_SET_CONFIG;
    directive.targetEpoch = targetEpoch;
    directive.targetNode = 42;
    directive.payload = (const uint8_t *)blob;
    directive.payloadLen = (uint16_t)strlen(blob);
    TEST_ASSERT_EQUAL_INT(SOLARI_OK,
        solariMsgBuildControl(&directive, wire, sizeof wire, &length, &count));
    rc = clientControlHandleDurable(&state, &cfg, 42, wire, length,
                                   reply, sizeof reply, &replyLength, &replyCount,
                                   &outcome);
    TEST_ASSERT_TRUE(outcome.wantReply);
    {
        solariTlvReader reader;
        const uint8_t *value;
        uint16_t type, valueLength, error = 0;
        uint64_t epoch = 0;
        solariTlvReaderInit(&reader, reply, replyLength);
        while (solariTlvNext(&reader, &type, &value, &valueLength) == SOLARI_OK) {
            if (type == TLV_ERROR_CODE) solariTlvReadU16(value, valueLength, &error);
            if (type == TLV_CTRL_TARGET_EPOCH) solariTlvReadU64(value, valueLength, &epoch);
        }
        TEST_ASSERT_EQUAL_UINT64(state.appliedEpoch, epoch);
        TEST_ASSERT_EQUAL_INT(rc == SOLARI_OK, error == 0);
    }
    return rc;
}

static solariStatus applyDirective(void)
{
    return applyPatch(7, "{\"sampleIntervalSec\":20}");
}

static void test_success_survives_restart(void)
{
    clientConfig restored;
    clientControlState restoredState;
    TEST_ASSERT_EQUAL_INT(SOLARI_OK, applyDirective());
    TEST_ASSERT_EQUAL_UINT64(7, state.appliedEpoch);
    TEST_ASSERT_EQUAL_UINT64(7, cfg.configEpoch);
    TEST_ASSERT_EQUAL_UINT32(20, cfg.sampleIntervalSec);
    clientConfigDefaults(&restored);
    snprintf(restored.ctrlStateFile, sizeof restored.ctrlStateFile, "%s", cfg.ctrlStateFile);
    TEST_ASSERT_EQUAL_INT(SOLARI_OK, clientControlStateLoad(&restoredState, &restored));
    TEST_ASSERT_EQUAL_UINT64(7, restoredState.appliedEpoch);
    TEST_ASSERT_EQUAL_UINT64(7, restored.configEpoch);
    TEST_ASSERT_EQUAL_UINT32(20, restored.sampleIntervalSec);
}

static void assertNotActivated(void)
{
    TEST_ASSERT_NOT_EQUAL(SOLARI_OK, applyDirective());
    TEST_ASSERT_EQUAL_UINT64(3, state.appliedEpoch);
    TEST_ASSERT_EQUAL_UINT32(15, cfg.sampleIntervalSec);
}

static void test_file_sync_failure_keeps_previous_epoch(void)
{
    failSyncNth = 1;
    assertNotActivated();
    TEST_ASSERT_EQUAL_INT(-1, access(cfg.ctrlStateFile, F_OK));
}

static void test_partial_patches_preserve_full_effective_config(void)
{
    clientConfig restored;
    clientControlState restoredState;
    TEST_ASSERT_EQUAL_INT(SOLARI_OK, applyDirective());
    TEST_ASSERT_EQUAL_INT(SOLARI_OK, applyPatch(8,
        "{\"processes\":[\"sshd\",\"quote\\\"test\"],"
        "\"logfiles\":[\"/tmp/app.log : error\"]}"));
    clientConfigDefaults(&restored);
    snprintf(restored.ctrlStateFile, sizeof restored.ctrlStateFile, "%s", cfg.ctrlStateFile);
    TEST_ASSERT_EQUAL_INT(SOLARI_OK, clientControlStateLoad(&restoredState, &restored));
    TEST_ASSERT_EQUAL_UINT64(8, restoredState.appliedEpoch);
    TEST_ASSERT_EQUAL_UINT32(20, restored.sampleIntervalSec);
    TEST_ASSERT_EQUAL_UINT8(2, restored.procCount);
    TEST_ASSERT_EQUAL_STRING("quote\"test", restored.procs[1]);
    TEST_ASSERT_EQUAL_STRING("/tmp/app.log", restored.logs[0].path);
    TEST_ASSERT_EQUAL_STRING("error", restored.logs[0].regex);
}

static void test_rename_failure_keeps_previous_epoch(void)
{
    failRename = 1;
    assertNotActivated();
    TEST_ASSERT_EQUAL_INT(-1, access(cfg.ctrlStateFile, F_OK));
}

static void test_directory_sync_failure_retries_without_false_success(void)
{
    failSyncNth = 2;
    assertNotActivated();
    TEST_ASSERT_EQUAL_INT(SOLARI_OK, applyDirective());
    TEST_ASSERT_EQUAL_UINT64(7, state.appliedEpoch);
}

static void test_missing_persistence_path_cannot_converge(void)
{
    cfg.ctrlStateFile[0] = '\0';
    assertNotActivated();
}

static void test_truncated_snapshot_cannot_advance_epoch(void)
{
    char header[160];
    FILE *file;
    TEST_ASSERT_EQUAL_INT(SOLARI_OK, applyDirective());
    file = fopen(cfg.ctrlStateFile, "rb");
    TEST_ASSERT_NOT_NULL(file);
    TEST_ASSERT_NOT_NULL(fgets(header, sizeof header, file));
    fclose(file);
    file = fopen(cfg.ctrlStateFile, "wb");
    TEST_ASSERT_NOT_NULL(file);
    fputs(header, file);                         /* body lost during storage fault */
    fclose(file);
    cfg.configEpoch = 2;
    cfg.sampleIntervalSec = 15;
    TEST_ASSERT_NOT_EQUAL(SOLARI_OK, clientControlStateLoad(&state, &cfg));
    TEST_ASSERT_EQUAL_UINT64(2, state.appliedEpoch);
    TEST_ASSERT_EQUAL_UINT32(15, cfg.sampleIntervalSec);
}

static void test_corrupt_or_legacy_state_cannot_advance_epoch(void)
{
    FILE *file = fopen(cfg.ctrlStateFile, "wb");
    TEST_ASSERT_NOT_NULL(file);
    fputs("9000\n", file);
    fclose(file);
    cfg.configEpoch = 2;
    TEST_ASSERT_NOT_EQUAL(SOLARI_OK, clientControlStateLoad(&state, &cfg));
    TEST_ASSERT_EQUAL_UINT64(2, state.appliedEpoch);
    TEST_ASSERT_EQUAL_UINT32(15, cfg.sampleIntervalSec);
    TEST_ASSERT_EQUAL_INT(SOLARI_OK, applyDirective());
    file = fopen(cfg.ctrlStateFile, "r+b");
    TEST_ASSERT_NOT_NULL(file);
    TEST_ASSERT_EQUAL_INT(0, fseek(file, -2, SEEK_END));
    fputc('X', file);
    fclose(file);
    cfg.configEpoch = 2;                        /* restart's .conf floor */
    cfg.sampleIntervalSec = 15;
    TEST_ASSERT_NOT_EQUAL(SOLARI_OK, clientControlStateLoad(&state, &cfg));
    TEST_ASSERT_EQUAL_UINT64(2, state.appliedEpoch);
    TEST_ASSERT_EQUAL_UINT32(15, cfg.sampleIntervalSec);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_success_survives_restart);
    RUN_TEST(test_partial_patches_preserve_full_effective_config);
    RUN_TEST(test_file_sync_failure_keeps_previous_epoch);
    RUN_TEST(test_rename_failure_keeps_previous_epoch);
    RUN_TEST(test_directory_sync_failure_retries_without_false_success);
    RUN_TEST(test_missing_persistence_path_cannot_converge);
    RUN_TEST(test_truncated_snapshot_cannot_advance_epoch);
    RUN_TEST(test_corrupt_or_legacy_state_cannot_advance_epoch);
    return UNITY_END();
}
