// SPDX-License-Identifier: MIT
//
// Minimal, dependency-free test helpers shared by the test executables.

#ifndef LOCKFREE_SPSC_TESTS_TEST_COMMON_HPP
#define LOCKFREE_SPSC_TESTS_TEST_COMMON_HPP

#include <cstdio>
#include <cstdlib>

namespace spsc_test {

inline int& failure_count() noexcept {
    static int failures = 0;
    return failures;
}

inline void report_failure(const char* expr, const char* file, int line) noexcept {
    std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", file, line, expr);
    ++failure_count();
}

inline int finish(const char* suite) noexcept {
    if (failure_count() != 0) {
        std::fprintf(stderr, "[%s] %d check(s) failed\n", suite, failure_count());
        return EXIT_FAILURE;
    }
    std::printf("[%s] all checks passed\n", suite);
    return EXIT_SUCCESS;
}

}  // namespace spsc_test

#define SPSC_CHECK(expr)                                                  \
    do {                                                                  \
        if (!(expr)) {                                                    \
            ::spsc_test::report_failure(#expr, __FILE__, __LINE__);       \
        }                                                                 \
    } while (false)

#define SPSC_CHECK_THROWS(expr, exception_type)                           \
    do {                                                                  \
        bool spsc_thrown = false;                                         \
        try {                                                             \
            static_cast<void>(expr);                                      \
        } catch (const exception_type&) {                                 \
            spsc_thrown = true;                                           \
        }                                                                 \
        if (!spsc_thrown) {                                               \
            ::spsc_test::report_failure(#expr " throws " #exception_type, \
                                        __FILE__, __LINE__);              \
        }                                                                 \
    } while (false)

#endif  // LOCKFREE_SPSC_TESTS_TEST_COMMON_HPP
