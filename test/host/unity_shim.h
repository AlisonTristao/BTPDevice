#ifndef BTPDEV_UNITY_SHIM_H
#define BTPDEV_UNITY_SHIM_H

// The few Unity macros the tests carried over from bally_OS use, mapped onto
// check.h, so those tests move here with no rewrite (and stay diffable
// against their origin).

#include <cstring>

#include "check.h"

#define TEST_ASSERT_TRUE(x) CHECK(x)
#define TEST_ASSERT_FALSE(x) CHECK(!(x))
#define TEST_ASSERT_NULL(x) CHECK((x) == nullptr)
#define TEST_ASSERT_NOT_NULL(x) CHECK((x) != nullptr)
#define TEST_ASSERT_EQUAL(e, a) CHECK((e) == (a))
#define TEST_ASSERT_NOT_EQUAL(e, a) CHECK((e) != (a))
#define TEST_ASSERT_EQUAL_INT(e, a) CHECK((e) == (a))
#define TEST_ASSERT_EQUAL_UINT8(e, a) CHECK((e) == (a))
#define TEST_ASSERT_EQUAL_UINT16(e, a) CHECK((e) == (a))
#define TEST_ASSERT_EQUAL_UINT32(e, a) CHECK((e) == (a))
#define TEST_ASSERT_EQUAL_STRING(e, a) CHECK_STR_EQ((a), (e))
#define TEST_ASSERT_EQUAL_UINT8_ARRAY(e, a, n) CHECK(std::memcmp((e), (a), (n)) == 0)
#define RUN_TEST(f) f()
#define UNITY_BEGIN() static_cast<void>(0)
#define UNITY_END() test_result(__FILE__)

#endif  // BTPDEV_UNITY_SHIM_H
