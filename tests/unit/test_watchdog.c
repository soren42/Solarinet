/*
 * test_watchdog.c - the watchdog's deterministic pieces (sec 7.3): the
 * SCP_MSG_WATCHDOG heartbeat frame and the liveness decision. The re-exec path
 * (platSpawnSelf on death) is exercised by a process-level smoke check, not here.
 */
#include "unity.h"
#include "client.h"
#include "solari/solariFrame.h"
#include "solari/solariError.h"

#include <string.h>
#include <unistd.h>
#include <sys/wait.h>

void setUp(void) {}
void tearDown(void) {}

static void test_heartbeat_frame(void)
{
    uint8_t buf[128];
    size_t len = 0;
    solariFrameHeader h;

    TEST_ASSERT_EQUAL_INT(SOLARI_OK,
        clientWatchdogBuildHeartbeat(0x1122334455667788ull, 7, buf, sizeof buf, &len));
    TEST_ASSERT_TRUE(len > 0);

    TEST_ASSERT_EQUAL_INT(SOLARI_OK, solariFrameParse(buf, len, &h, NULL, NULL, NULL));
    TEST_ASSERT_EQUAL_UINT8(SCP_MSG_WATCHDOG, h.msgType);
    TEST_ASSERT_EQUAL_UINT64(0x1122334455667788ull, h.sourceNodeId);
    TEST_ASSERT_EQUAL_UINT32(7, h.seqNo);

    /* a too-small buffer is rejected cleanly, not overrun */
    TEST_ASSERT_EQUAL_INT(ERR_BUFFER_FULL,
        clientWatchdogBuildHeartbeat(1, 1, buf, 4, &len));
}

static void test_supervised_gone(void)
{
    pid_t child;

    /* self is alive */
    TEST_ASSERT_FALSE(clientWatchdogSupervisedGone((int64_t)getpid()));

    /* a child that has exited and been reaped reads as gone */
    child = fork();
    if (child == 0) _exit(0);
    TEST_ASSERT_TRUE(child > 0);
    waitpid(child, NULL, 0);
    TEST_ASSERT_TRUE(clientWatchdogSupervisedGone((int64_t)child));

    /* an invalid pid is treated as gone */
    TEST_ASSERT_TRUE(clientWatchdogSupervisedGone(-1));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_heartbeat_frame);
    RUN_TEST(test_supervised_gone);
    return UNITY_END();
}
