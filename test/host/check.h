#ifndef TRACEKIT_TEST_CHECK_H
#define TRACEKIT_TEST_CHECK_H

// A deliberately tiny test harness: CHECK() records a failure and keeps going,
// test_result() is main()'s return value. No framework to install on the
// host or in CI.

#include <cstdio>
#include <cstring>
#include <string>

namespace tracekit_test {
inline int& failures() {
    static int count = 0;
    return count;
}

// A copy of a C string taken while it is still alive. CHECK_STR_EQ(f().c_str(), ...)
// hands over a pointer into a temporary std::string that dies at the end of
// that statement: comparing through the pointer afterwards read freed memory
// (a long string, out of the small-string buffer, came back as garbage).
struct StrCopy {
    explicit StrCopy(const char* text) : null(text == nullptr), value(text != nullptr ? text : "") {}
    bool null;
    std::string value;
};
}  // namespace tracekit_test

#define CHECK(condition)                                                     \
    do {                                                                     \
        if (!(condition)) {                                                  \
            std::printf("%s:%d: CHECK(%s) failed\n", __FILE__, __LINE__,     \
                        #condition);                                         \
            ++tracekit_test::failures();                                       \
        }                                                                    \
    } while (0)

#define CHECK_STR_EQ(actual, expected)                                       \
    do {                                                                     \
        const tracekit_test::StrCopy check_a_((actual));                     \
        const char* check_e_ = (expected);                                   \
        if (check_a_.null || check_a_.value != check_e_) {                   \
            std::printf("%s:%d: \"%s\" != \"%s\"\n", __FILE__, __LINE__,     \
                        check_a_.null ? "(null)" : check_a_.value.c_str(),   \
                        check_e_);                                           \
            ++tracekit_test::failures();                                       \
        }                                                                    \
    } while (0)

#define CHECK_CONTAINS(haystack, needle)                                     \
    do {                                                                     \
        if (std::strstr((haystack), (needle)) == nullptr) {                  \
            std::printf("%s:%d: \"%s\" does not contain \"%s\"\n", __FILE__, \
                        __LINE__, (haystack), (needle));                     \
            ++tracekit_test::failures();                                       \
        }                                                                    \
    } while (0)

inline int test_result(const char* suite) {
    if (tracekit_test::failures() == 0) {
        std::printf("%s: ok\n", suite);
        return 0;
    }
    std::printf("%s: %d failure(s)\n", suite, tracekit_test::failures());
    return 1;
}

#endif  // TRACEKIT_TEST_CHECK_H
