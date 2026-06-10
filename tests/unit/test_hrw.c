/*
 * test_hrw.c - rendezvous (HRW) target ownership (sec 8.2): determinism, exact
 * replication factor, and minimal churn when a monitor leaves the fleet.
 */
#include "unity.h"
#include "monitor.h"

#include <stdio.h>

void setUp(void) {}
void tearDown(void) {}

static void test_determinism(void)
{
    uint64_t fleet[3] = { 111, 222, 333 };
    bool a = monitorOwnsTarget(222, fleet, 3, "tcp:host:443", 2);
    bool b = monitorOwnsTarget(222, fleet, 3, "tcp:host:443", 2);
    TEST_ASSERT_EQUAL_INT(a, b);
    /* a non-member never owns */
    TEST_ASSERT_FALSE(monitorOwnsTarget(999, fleet, 3, "tcp:host:443", 2));
}

static void test_exact_replication(void)
{
    uint64_t fleet[5] = { 1, 2, 3, 4, 5 };
    int ti;
    for (ti = 0; ti < 60; ti++) {
        char tid[32];
        int owners = 0, j;
        snprintf(tid, sizeof tid, "tcp:host%d:443", ti);
        for (j = 0; j < 5; j++)
            if (monitorOwnsTarget(fleet[j], fleet, 5, tid, 2)) owners++;
        TEST_ASSERT_EQUAL_INT(2, owners);            /* exactly k own each target */
    }
}

static void test_minimal_churn(void)
{
    uint64_t fleet5[5] = { 1, 2, 3, 4, 5 };
    uint64_t fleet4[4] = { 1, 2, 3, 4 };             /* node 5 removed */
    int ti, changed = 0, total = 0;

    for (ti = 0; ti < 200; ti++) {
        char tid[32];
        int j;
        bool removedOwned;
        snprintf(tid, sizeof tid, "tcp:host%d:443", ti);
        removedOwned = monitorOwnsTarget(5, fleet5, 5, tid, 2);
        for (j = 0; j < 4; j++) {
            bool before = monitorOwnsTarget(fleet4[j], fleet5, 5, tid, 2);
            bool after  = monitorOwnsTarget(fleet4[j], fleet4, 4, tid, 2);
            total++;
            if (before != after) {
                changed++;
                /* a survivor's ownership may change ONLY for targets the
                 * departed node owned - that is the definition of low churn */
                TEST_ASSERT_TRUE(removedOwned);
            }
        }
    }
    /* only node-5's targets reassign: well under 20% of (node,target) pairs */
    TEST_ASSERT_TRUE(changed * 5 < total);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_determinism);
    RUN_TEST(test_exact_replication);
    RUN_TEST(test_minimal_churn);
    return UNITY_END();
}
