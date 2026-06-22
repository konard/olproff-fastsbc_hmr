// fastsbc_hmr - The HMR DSL was inspired by Oracle's HMR language. It is a compiler based on the LLVM backend that generates a library for dynamic loading.
// Copyright (C) 2026  fastsbc_hmr contributors
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program.  If not, see <https://www.gnu.org/licenses/>.

// test.hpp — a tiny, dependency-free unit-test harness.
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
inline int& current_failures() {
    static int f = 0;
    return f;
}

inline void report_failure(std::string_view file, int line,
                          std::string_view expr) {
    ++current_failures();
    std::fprintf(stderr, "    FAIL %.*s:%d  %.*s\n",
                 static_cast<int>(file.size()), file.data(), line,
                 static_cast<int>(expr.size()), expr.data());
}

inline int run() {
    int failed = 0;
    int passed = 0;
    for (const auto& c : registry()) {
        current_failures() = 0;
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
        if (current_failures() == 0 && !threw) {
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
        if (!(cond)) ::hmrtest::report_failure(__FILE__, __LINE__, #cond);  \
    } while (0)

#define CHECK_EQ(a, b)                                                     \
    do {                                                                   \
        if (!((a) == (b)))                                                 \
            ::hmrtest::report_failure(__FILE__, __LINE__, #a " == " #b);    \
    } while (0)

#define CHECK_NE(a, b)                                                     \
    do {                                                                   \
        if (!((a) != (b)))                                                 \
            ::hmrtest::report_failure(__FILE__, __LINE__, #a " != " #b);    \
    } while (0)

#define REQUIRE(cond)                                                      \
    do {                                                                   \
        if (!(cond)) {                                                     \
            ::hmrtest::report_failure(__FILE__, __LINE__, #cond);           \
            return;                                                        \
        }                                                                  \
    } while (0)

#define TEST_MAIN()                                                        \
    int main() { return ::hmrtest::run(); }
