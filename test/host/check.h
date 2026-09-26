#ifndef BTPDEV_TEST_CHECK_H
#define BTPDEV_TEST_CHECK_H

// A deliberately tiny test harness: CHECK() records a failure and keeps going,
// test_result() is main()'s return value. No framework to install on the
// host or in CI.

#include <cstdio>
#include <cstring>

namespace btpdev_test {
inline int& failures() {
    static int count = 0;
    return count;
}
}  // namespace btpdev_test

#define CHECK(condition)                                                     \
    do {                                                                     \
        if (!(condition)) {                                                  \
            std::printf("%s:%d: CHECK(%s) failed\n", __FILE__, __LINE__,     \
                        #condition);                                         \
            ++btpdev_test::failures();                                       \
        }                                                                    \
    } while (0)

#define CHECK_STR_EQ(actual, expected)                                       \
    do {                                                                     \
        const char* check_a_ = (actual);                                     \
        const char* check_e_ = (expected);                                   \
        if (check_a_ == nullptr || std::strcmp(check_a_, check_e_) != 0) {   \
            std::printf("%s:%d: \"%s\" != \"%s\"\n", __FILE__, __LINE__,     \
                        check_a_ ? check_a_ : "(null)", check_e_);           \
            ++btpdev_test::failures();                                       \
        }                                                                    \
    } while (0)

#define CHECK_CONTAINS(haystack, needle)                                     \
    do {                                                                     \
        if (std::strstr((haystack), (needle)) == nullptr) {                  \
            std::printf("%s:%d: \"%s\" does not contain \"%s\"\n", __FILE__, \
                        __LINE__, (haystack), (needle));                     \
            ++btpdev_test::failures();                                       \
        }                                                                    \
    } while (0)

inline int test_result(const char* suite) {
    if (btpdev_test::failures() == 0) {
        std::printf("%s: ok\n", suite);
        return 0;
    }
    std::printf("%s: %d failure(s)\n", suite, btpdev_test::failures());
    return 1;
}

#endif  // BTPDEV_TEST_CHECK_H
