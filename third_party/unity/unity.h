/*
 * unity.h - minimal, API-compatible subset of ThrowTheSwitch/Unity (MIT).
 * See README.md in this directory. Replace with upstream Unity when vendoring
 * for real; the test suites use only the surface declared here.
 */
#ifndef UNITY_H
#define UNITY_H

#include <stdint.h>
#include <string.h>

/* User-provided fixtures (may be empty). */
void setUp(void);
void tearDown(void);

/* Framework state and primitives (defined in unity.c). */
void   unityBegin(const char *file);
int    unityEnd(void);
void   unityDefaultTestRun(void (*func)(void), const char *name, int line);
void   unityFail(const char *msg, int line);

/* Assertion back-ends. */
void unityAssertEqualNumber(int64_t expected, int64_t actual, int line, const char *msg);
void unityAssertEqualUNumber(uint64_t expected, uint64_t actual, int line, const char *msg);
void unityAssertBits(uint64_t mask, uint64_t expected, uint64_t actual, int line);
void unityAssertEqualString(const char *expected, const char *actual, int line);
void unityAssertEqualMemory(const void *expected, const void *actual, size_t len, int line);
void unityAssertNotNull(const void *ptr, int line, const char *msg);
void unityAssertNull(const void *ptr, int line, const char *msg);

/* Public macros (mirror upstream names). */
#define UNITY_BEGIN()  unityBegin(__FILE__)
#define UNITY_END()    unityEnd()
#define RUN_TEST(f)    unityDefaultTestRun(f, #f, __LINE__)

#define TEST_FAIL_MESSAGE(msg)        unityFail((msg), __LINE__)
#define TEST_ASSERT_TRUE(c)           unityAssertEqualNumber(1, (c) ? 1 : 0, __LINE__, "expected TRUE")
#define TEST_ASSERT_FALSE(c)          unityAssertEqualNumber(0, (c) ? 1 : 0, __LINE__, "expected FALSE")
#define TEST_ASSERT(c)                TEST_ASSERT_TRUE(c)

#define TEST_ASSERT_EQUAL_INT(e,a)    unityAssertEqualNumber((int64_t)(e), (int64_t)(a), __LINE__, "int mismatch")
#define TEST_ASSERT_EQUAL(e,a)        TEST_ASSERT_EQUAL_INT(e,a)
#define TEST_ASSERT_EQUAL_UINT(e,a)   unityAssertEqualUNumber((uint64_t)(e),(uint64_t)(a),__LINE__,"uint mismatch")
#define TEST_ASSERT_EQUAL_UINT8(e,a)  unityAssertEqualUNumber((uint64_t)(uint8_t)(e),(uint64_t)(uint8_t)(a),__LINE__,"u8 mismatch")
#define TEST_ASSERT_EQUAL_UINT16(e,a) unityAssertEqualUNumber((uint64_t)(uint16_t)(e),(uint64_t)(uint16_t)(a),__LINE__,"u16 mismatch")
#define TEST_ASSERT_EQUAL_UINT32(e,a) unityAssertEqualUNumber((uint64_t)(uint32_t)(e),(uint64_t)(uint32_t)(a),__LINE__,"u32 mismatch")
#define TEST_ASSERT_EQUAL_UINT64(e,a) unityAssertEqualUNumber((uint64_t)(e),(uint64_t)(a),__LINE__,"u64 mismatch")
#define TEST_ASSERT_EQUAL_HEX32(e,a)  unityAssertEqualUNumber((uint64_t)(uint32_t)(e),(uint64_t)(uint32_t)(a),__LINE__,"hex32 mismatch")
#define TEST_ASSERT_EQUAL_size_t(e,a) unityAssertEqualUNumber((uint64_t)(e),(uint64_t)(a),__LINE__,"size_t mismatch")
#define TEST_ASSERT_EQUAL_PTR(e,a)    unityAssertEqualUNumber((uint64_t)(uintptr_t)(e),(uint64_t)(uintptr_t)(a),__LINE__,"ptr mismatch")

#define TEST_ASSERT_EQUAL_STRING(e,a) unityAssertEqualString((e),(a),__LINE__)
#define TEST_ASSERT_EQUAL_MEMORY(e,a,len) unityAssertEqualMemory((e),(a),(size_t)(len),__LINE__)

#define TEST_ASSERT_NULL(p)           unityAssertNull((p), __LINE__, "expected NULL")
#define TEST_ASSERT_NOT_NULL(p)       unityAssertNotNull((p), __LINE__, "expected non-NULL")

#endif /* UNITY_H */
