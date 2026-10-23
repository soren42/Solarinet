/* Fault-inject monitor state persistence and verify CONTROL_RESULT truth. */
#define _POSIX_C_SOURCE 200809L
#include "unity.h"
#include "monitor.h"
#include "solari/solariMsg.h"
#include "solari/solariTlv.h"
#include "../test_temp_path.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static char testDir[512];
static monitorConfig cfg;
static monitorControlState state;
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
                                           "solari-monitor-control-test"));
    monitorConfigDefaults(&cfg);
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

static solariStatus applyDirective(void)
{
    solariControl directive;
    monitorControlOutcome outcome;
    uint8_t wire[512], reply[MONITOR_CTRL_RESULT_CAP];
    size_t length = 0, replyLength = 0;
    uint16_t count = 0, replyCount = 0;
    solariStatus rc;
    const char *blob = "{\"probe\":{\"roundIntervalSec\":17}}";

    memset(&directive, 0, sizeof directive);
    directive.verb = CTRL_SET_CONFIG;
    directive.targetEpoch = 7;
    directive.targetNode = 42;
    directive.payload = (const uint8_t *)blob;
    directive.payloadLen = (uint16_t)strlen(blob);
    TEST_ASSERT_EQUAL_INT(SOLARI_OK,
        solariMsgBuildControl(&directive, wire, sizeof wire, &length, &count));
    rc = monitorHandleControlDurable(&state, &cfg, 42, wire, length,
                                     reply, sizeof reply, &replyLength,
                                     &replyCount, &outcome);
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

static void assertNotActivated(void)
{
    uint32_t oldInterval = cfg.roundIntervalSec;
    TEST_ASSERT_NOT_EQUAL(SOLARI_OK, applyDirective());
    TEST_ASSERT_EQUAL_UINT64(3, state.appliedEpoch);
    TEST_ASSERT_EQUAL_UINT32(oldInterval, cfg.roundIntervalSec);
}

static void test_success_survives_restart(void)
{
    monitorConfig restored;
    monitorControlState restoredState;
    TEST_ASSERT_EQUAL_INT(SOLARI_OK, applyDirective());
    TEST_ASSERT_EQUAL_UINT64(7, state.appliedEpoch);
    TEST_ASSERT_EQUAL_UINT64(7, cfg.configEpoch);
    TEST_ASSERT_EQUAL_UINT32(17, cfg.roundIntervalSec);
    monitorConfigDefaults(&restored);
    snprintf(restored.ctrlStateFile, sizeof restored.ctrlStateFile, "%s",
             cfg.ctrlStateFile);
    TEST_ASSERT_EQUAL_INT(SOLARI_OK,
        monitorControlStateLoad(&restoredState, &restored));
    TEST_ASSERT_EQUAL_UINT64(7, restoredState.appliedEpoch);
    TEST_ASSERT_EQUAL_UINT64(7, restored.configEpoch);
    TEST_ASSERT_EQUAL_UINT32(17, restored.roundIntervalSec);
}

static void test_file_sync_failure_keeps_previous_epoch(void)
{
    failSyncNth = 1;
    assertNotActivated();
    TEST_ASSERT_EQUAL_INT(-1, access(cfg.ctrlStateFile, F_OK));
}

static void test_directory_sync_failure_does_not_acknowledge(void)
{
    failSyncNth = 2;
    assertNotActivated();
}

static void test_rename_failure_keeps_previous_epoch(void)
{
    failRename = 1;
    assertNotActivated();
    TEST_ASSERT_EQUAL_INT(-1, access(cfg.ctrlStateFile, F_OK));
}

static void test_missing_state_path_fails_closed(void)
{
    cfg.ctrlStateFile[0] = '\0';
    cfg.spoolDb[0] = '\0';
    assertNotActivated();
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_success_survives_restart);
    RUN_TEST(test_file_sync_failure_keeps_previous_epoch);
    RUN_TEST(test_directory_sync_failure_does_not_acknowledge);
    RUN_TEST(test_rename_failure_keeps_previous_epoch);
    RUN_TEST(test_missing_state_path_fails_closed);
    return UNITY_END();
}
