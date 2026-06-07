/*
 * test_spool.c - store-and-forward fault behavior (section 14 fault suite):
 * enqueue while "offline", drain in order on "reconnect", honor backoff after a
 * failed send, and survive a process restart (reopen).
 */
#include "unity.h"
#include "solari/solariSpool.h"
#include "solari/solariError.h"

#include <stdio.h>
#include <string.h>

#define DBPATH "/tmp/solari_spool_test.db"

void setUp(void)   { remove(DBPATH); }
void tearDown(void){ remove(DBPATH); }

static solariStatus pushStr(solariSpool *s, const char *str)
{
    return solariSpoolPush(s, (const uint8_t *)str, strlen(str));
}

static void test_open_empty(void)
{
    solariSpool *s = NULL;
    uint64_t rowId = 7;
    const uint8_t *frame = (const uint8_t *)1;
    size_t len = 9;

    TEST_ASSERT_EQUAL_INT(SOLARI_OK, solariSpoolOpen(DBPATH, &s));
    TEST_ASSERT_EQUAL_size_t(0, solariSpoolDepth(s));
    /* empty queue: SOLARI_OK with frame == NULL, not an error */
    TEST_ASSERT_EQUAL_INT(SOLARI_OK, solariSpoolPeek(s, &rowId, &frame, &len));
    TEST_ASSERT_NULL(frame);
    TEST_ASSERT_EQUAL_size_t(0, len);
    TEST_ASSERT_EQUAL_UINT64(0, rowId);
    solariSpoolClose(s);
}

static void test_push_peek_ack_fifo(void)
{
    solariSpool *s = NULL;
    uint64_t id = 0;
    const uint8_t *frame = NULL;
    size_t len = 0;

    TEST_ASSERT_EQUAL_INT(SOLARI_OK, solariSpoolOpen(DBPATH, &s));
    TEST_ASSERT_EQUAL_INT(SOLARI_OK, pushStr(s, "AAA"));
    TEST_ASSERT_EQUAL_INT(SOLARI_OK, pushStr(s, "BBBB"));
    TEST_ASSERT_EQUAL_INT(SOLARI_OK, pushStr(s, "CCCCC"));
    TEST_ASSERT_EQUAL_size_t(3, solariSpoolDepth(s));

    /* oldest first */
    TEST_ASSERT_EQUAL_INT(SOLARI_OK, solariSpoolPeek(s, &id, &frame, &len));
    TEST_ASSERT_EQUAL_size_t(3, len);
    TEST_ASSERT_EQUAL_MEMORY("AAA", frame, 3);
    TEST_ASSERT_EQUAL_INT(SOLARI_OK, solariSpoolAck(s, id));
    TEST_ASSERT_EQUAL_size_t(2, solariSpoolDepth(s));

    TEST_ASSERT_EQUAL_INT(SOLARI_OK, solariSpoolPeek(s, &id, &frame, &len));
    TEST_ASSERT_EQUAL_MEMORY("BBBB", frame, 4);
    TEST_ASSERT_EQUAL_INT(SOLARI_OK, solariSpoolAck(s, id));

    TEST_ASSERT_EQUAL_INT(SOLARI_OK, solariSpoolPeek(s, &id, &frame, &len));
    TEST_ASSERT_EQUAL_MEMORY("CCCCC", frame, 5);
    TEST_ASSERT_EQUAL_INT(SOLARI_OK, solariSpoolAck(s, id));

    TEST_ASSERT_EQUAL_size_t(0, solariSpoolDepth(s));
    solariSpoolClose(s);
}

/* A nacked frame backs off, so the NEXT ready frame is served instead. */
static void test_nack_backoff_skips(void)
{
    solariSpool *s = NULL;
    uint64_t id1 = 0, id2 = 0;
    const uint8_t *frame = NULL;
    size_t len = 0;

    TEST_ASSERT_EQUAL_INT(SOLARI_OK, solariSpoolOpen(DBPATH, &s));
    pushStr(s, "first");
    pushStr(s, "second");

    /* peek -> "first"; simulate failed send -> Nack (backoff ~1s) */
    TEST_ASSERT_EQUAL_INT(SOLARI_OK, solariSpoolPeek(s, &id1, &frame, &len));
    TEST_ASSERT_EQUAL_MEMORY("first", frame, 5);
    TEST_ASSERT_EQUAL_INT(SOLARI_OK, solariSpoolNack(s, id1));

    /* both still queued (nack does not remove) */
    TEST_ASSERT_EQUAL_size_t(2, solariSpoolDepth(s));

    /* next peek must skip the backing-off "first" and return "second" */
    TEST_ASSERT_EQUAL_INT(SOLARI_OK, solariSpoolPeek(s, &id2, &frame, &len));
    TEST_ASSERT_EQUAL_MEMORY("second", frame, 6);
    TEST_ASSERT_TRUE(id2 != id1);

    solariSpoolClose(s);
}

/* Frames persist across a close/reopen (process restart). */
static void test_persistence_across_reopen(void)
{
    solariSpool *s = NULL;
    uint64_t id = 0;
    const uint8_t *frame = NULL;
    size_t len = 0;

    TEST_ASSERT_EQUAL_INT(SOLARI_OK, solariSpoolOpen(DBPATH, &s));
    pushStr(s, "durable");
    solariSpoolClose(s);

    s = NULL;
    TEST_ASSERT_EQUAL_INT(SOLARI_OK, solariSpoolOpen(DBPATH, &s));
    TEST_ASSERT_EQUAL_size_t(1, solariSpoolDepth(s));
    TEST_ASSERT_EQUAL_INT(SOLARI_OK, solariSpoolPeek(s, &id, &frame, &len));
    TEST_ASSERT_EQUAL_MEMORY("durable", frame, 7);
    TEST_ASSERT_EQUAL_INT(SOLARI_OK, solariSpoolAck(s, id));
    TEST_ASSERT_EQUAL_size_t(0, solariSpoolDepth(s));
    solariSpoolClose(s);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_open_empty);
    RUN_TEST(test_push_peek_ack_fifo);
    RUN_TEST(test_nack_backoff_skips);
    RUN_TEST(test_persistence_across_reopen);
    return UNITY_END();
}
