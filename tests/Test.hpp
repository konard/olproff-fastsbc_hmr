// SPDX-License-Identifier: MIT
//
// Test.hpp — a tiny, dependency-free unit-test harness.
//
// Self-contained so CI needs no network access to fetch a framework. Each test
// is registered with TEST(suite, name); assertions record failures and keep
// going. main() (TEST_MAIN) runs every registered test and prints a summary,
// exiting non-zero if any assertion failed.

#pragma once

#include <cstdio>
#include <exception>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace hmrtest {

struct Case {
    std::string suite;
    std::string name;
    std::function<void()> fn;
};

inline std::vector<Case>& registry() {
    static std::vector<Case> r;
    return r;
}

struct Registrar {
    Registrar(std::string suite, std::string name, std::function<void()> fn) {
        registry().push_back({std::move(suite), std::move(name), std::move(fn)});
    }
};

// Per-test failure counter (reset by the runner before each case).
inline int& currentFailures() {
    static int f = 0;
    return f;
}

inline void reportFailure(std::string_view file, int line,
                          std::string_view expr) {
    ++currentFailures();
    std::fprintf(stderr, "    FAIL %.*s:%d  %.*s\n",
                 static_cast<int>(file.size()), file.data(), line,
                 static_cast<int>(expr.size()), expr.data());
}

inline int run() {
    int failed = 0;
    int passed = 0;
    for (const auto& c : registry()) {
        currentFailures() = 0;
        bool threw = false;
        try {
            c.fn();
        } catch (const std::exception& e) {
            threw = true;
            std::fprintf(stderr, "    FAIL %s.%s threw: %s\n", c.suite.c_str(),
                         c.name.c_str(), e.what());
        } catch (...) {
            threw = true;
            std::fprintf(stderr, "    FAIL %s.%s threw unknown exception\n",
                         c.suite.c_str(), c.name.c_str());
        }
        if (currentFailures() == 0 && !threw) {
            ++passed;
            std::fprintf(stdout, "  ok   %s.%s\n", c.suite.c_str(),
                         c.name.c_str());
        } else {
            ++failed;
            std::fprintf(stdout, "  FAIL %s.%s\n", c.suite.c_str(),
                         c.name.c_str());
        }
    }
    std::fprintf(stdout, "\n%d passed, %d failed, %d total\n", passed, failed,
                 passed + failed);
    return failed == 0 ? 0 : 1;
}

}  // namespace hmrtest

#define HMR_CONCAT_(a, b) a##b
#define HMR_CONCAT(a, b) HMR_CONCAT_(a, b)

#define TEST(suite, name)                                                  \
    static void HMR_CONCAT(hmr_test_, __LINE__)();                         \
    static ::hmrtest::Registrar HMR_CONCAT(hmr_reg_, __LINE__){            \
        #suite, #name, &HMR_CONCAT(hmr_test_, __LINE__)};                  \
    static void HMR_CONCAT(hmr_test_, __LINE__)()

#define CHECK(cond)                                                        \
    do {                                                                   \
        if (!(cond)) ::hmrtest::reportFailure(__FILE__, __LINE__, #cond);  \
    } while (0)

#define CHECK_EQ(a, b)                                                     \
    do {                                                                   \
        if (!((a) == (b)))                                                 \
            ::hmrtest::reportFailure(__FILE__, __LINE__, #a " == " #b);    \
    } while (0)

#define CHECK_NE(a, b)                                                     \
    do {                                                                   \
        if (!((a) != (b)))                                                 \
            ::hmrtest::reportFailure(__FILE__, __LINE__, #a " != " #b);    \
    } while (0)

#define REQUIRE(cond)                                                      \
    do {                                                                   \
        if (!(cond)) {                                                     \
            ::hmrtest::reportFailure(__FILE__, __LINE__, #cond);           \
            return;                                                        \
        }                                                                  \
    } while (0)

#define TEST_MAIN()                                                        \
    int main() { return ::hmrtest::run(); }
