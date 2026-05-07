#pragma once

// Tiny in-tree test harness: zero dependencies, no global registration, just
// macros that print useful failure messages and accumulate a per-process
// failure counter.
//
// Usage:
//   int main() {
//       IDX_CHECK_EQ(1 + 1, 2);
//       IDX_CHECK_NEAR(0.1 + 0.2, 0.3, 1e-9);
//       return idx::testing::report();
//   }

#include <atomic>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <string_view>

namespace idx::testing {

inline std::atomic<int>& checks_total() {
    static std::atomic<int> v{0};
    return v;
}

inline std::atomic<int>& checks_failed() {
    static std::atomic<int> v{0};
    return v;
}

inline int report(std::string_view test_name = "") {
    const int failed = checks_failed().load();
    const int total = checks_total().load();
    if (failed == 0) {
        std::cout << "[ok] " << (test_name.empty() ? "test" : test_name)
                  << " (" << total << " checks)\n";
        return 0;
    }
    std::cerr << "[fail] " << (test_name.empty() ? "test" : test_name) << ": "
              << failed << " / " << total << " checks failed\n";
    return EXIT_FAILURE;
}

}  // namespace idx::testing

#define IDX_CHECK(expr)                                                              \
    do {                                                                             \
        idx::testing::checks_total().fetch_add(1, std::memory_order_relaxed);        \
        if (!(expr)) {                                                               \
            idx::testing::checks_failed().fetch_add(1, std::memory_order_relaxed);   \
            std::cerr << __FILE__ << ":" << __LINE__                                 \
                      << ": IDX_CHECK failed: " << #expr << '\n';                    \
        }                                                                            \
    } while (0)

#define IDX_CHECK_EQ(a, b)                                                           \
    do {                                                                             \
        idx::testing::checks_total().fetch_add(1, std::memory_order_relaxed);        \
        const auto _idx_a = (a);                                                     \
        const auto _idx_b = (b);                                                     \
        if (!(_idx_a == _idx_b)) {                                                   \
            idx::testing::checks_failed().fetch_add(1, std::memory_order_relaxed);   \
            std::cerr << __FILE__ << ":" << __LINE__                                 \
                      << ": IDX_CHECK_EQ failed: " << #a << " == " << #b             \
                      << "\n  lhs = " << _idx_a                                      \
                      << "\n  rhs = " << _idx_b << '\n';                             \
        }                                                                            \
    } while (0)

#define IDX_CHECK_NEAR(a, b, eps)                                                    \
    do {                                                                             \
        idx::testing::checks_total().fetch_add(1, std::memory_order_relaxed);        \
        const double _idx_a = static_cast<double>(a);                                \
        const double _idx_b = static_cast<double>(b);                                \
        if (!(std::abs(_idx_a - _idx_b) <= (eps))) {                                 \
            idx::testing::checks_failed().fetch_add(1, std::memory_order_relaxed);   \
            std::cerr << __FILE__ << ":" << __LINE__                                 \
                      << ": IDX_CHECK_NEAR failed: " << #a << " ~= " << #b           \
                      << "\n  lhs = " << _idx_a                                      \
                      << "\n  rhs = " << _idx_b                                      \
                      << "\n  eps = " << (eps) << '\n';                              \
        }                                                                            \
    } while (0)

#define IDX_CHECK_TRUE(expr) IDX_CHECK(expr)
#define IDX_CHECK_FALSE(expr) IDX_CHECK(!(expr))
