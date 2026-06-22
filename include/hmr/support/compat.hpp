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

// Compat.hpp — C++26 feature detection with graceful C++23 fallbacks.
//
// The issue targets C++26 (std::expected, std::generator, static operator(),
// std::print, deducing this).  Not every toolchain that can build this project
// ships all of those library/language features yet (e.g. GCC 13 lacks
// <generator>, <print> and deducing this).  This header detects each feature
// and provides a portable shim so the rest of the codebase can use the modern
// spelling unconditionally:
//
//   * hmr::generator<T>   -> std::generator<T> when available, else a small
//                            coroutine-based fallback implemented here.
//   * hmr::print/println  -> std::print/std::println when available, else a
//                            std::format based fallback.
//
// `static operator()` (C++23) and `std::expected` (C++23) are used directly
// because every supported compiler provides them.

#pragma once

#include <version>

// ---------------------------------------------------------------------------
// Feature detection macros (usable in #if and as constexpr bools).
// ---------------------------------------------------------------------------
#if defined(__cpp_lib_print)
#  define HMR_HAS_STD_PRINT 1
#else
#  define HMR_HAS_STD_PRINT 0
#endif

#if defined(__cpp_lib_generator) && __has_include(<generator>)
#  define HMR_HAS_STD_GENERATOR 1
#else
#  define HMR_HAS_STD_GENERATOR 0
#endif

#if defined(__cpp_explicit_this_parameter)
#  define HMR_HAS_DEDUCING_THIS 1
#else
#  define HMR_HAS_DEDUCING_THIS 0
#endif

#if defined(__cpp_static_call_operator)
#  define HMR_HAS_STATIC_CALL_OPERATOR 1
#else
#  define HMR_HAS_STATIC_CALL_OPERATOR 0
#endif

// `HMR_STATIC_CALL` expands to `static` when the compiler supports C++23
// static operator(), letting call operators be zero-overhead free functions.
#if HMR_HAS_STATIC_CALL_OPERATOR
#  define HMR_STATIC_CALL static
#  define HMR_STATIC_CALL_CONST
#else
#  define HMR_STATIC_CALL
#  define HMR_STATIC_CALL_CONST const
#endif

// ---------------------------------------------------------------------------
// hmr::generator<T>
// ---------------------------------------------------------------------------
#include <coroutine>
#include <exception>
#include <iterator>
#include <utility>

#if HMR_HAS_STD_GENERATOR
#  include <generator>
#endif

namespace hmr::support {

// Minimal single-type, synchronous generator used when <generator> is absent.
// Coroutines themselves are C++20, so this works on every supported compiler.
template <class T>
class fallback_generator {
public:
    struct promise_type {
        T value_{};
        std::exception_ptr exception_;

        fallback_generator get_return_object() {
            return fallback_generator{
                std::coroutine_handle<promise_type>::from_promise(*this)};
        }
        static std::suspend_always initial_suspend() noexcept { return {}; }
        static std::suspend_always final_suspend() noexcept { return {}; }
        void unhandled_exception() { exception_ = std::current_exception(); }
        template <class U = T>
        std::suspend_always yield_value(U&& v) {
            value_ = std::forward<U>(v);
            return {};
        }
        void return_void() noexcept {}
    };

    using handle_type = std::coroutine_handle<promise_type>;

    explicit fallback_generator(handle_type h) noexcept : coro_(h) {}
    fallback_generator(fallback_generator&& o) noexcept
        : coro_(std::exchange(o.coro_, {})) {}
    fallback_generator& operator=(fallback_generator&& o) noexcept {
        if (this != &o) {
            if (coro_) coro_.destroy();
            coro_ = std::exchange(o.coro_, {});
        }
        return *this;
    }
    fallback_generator(const fallback_generator&) = delete;
    fallback_generator& operator=(const fallback_generator&) = delete;
    ~fallback_generator() {
        if (coro_) coro_.destroy();
    }

    struct iterator {
        handle_type coro_{};
        bool done_ = true;

        iterator& operator++() {
            coro_.resume();
            done_ = coro_.done();
            if (done_ && coro_.promise().exception_)
                std::rethrow_exception(coro_.promise().exception_);
            return *this;
        }
        const T& operator*() const { return coro_.promise().value_; }
        bool operator==(std::default_sentinel_t) const { return done_; }
    };

    iterator begin() {
        if (coro_) {
            coro_.resume();
            if (coro_.done() && coro_.promise().exception_)
                std::rethrow_exception(coro_.promise().exception_);
        }
        return iterator{coro_, coro_ ? coro_.done() : true};
    }
    static std::default_sentinel_t end() noexcept { return {}; }

private:
    handle_type coro_{};
};

}  // namespace hmr::support

namespace hmr {

#if HMR_HAS_STD_GENERATOR
template <class T>
using generator = std::generator<T>;
#else
template <class T>
using generator = support::fallback_generator<T>;
#endif

}  // namespace hmr

// ---------------------------------------------------------------------------
// hmr::print / hmr::println
// ---------------------------------------------------------------------------
#include <cstdio>
#include <format>
#include <string>

#if HMR_HAS_STD_PRINT
#  include <print>
#endif

namespace hmr {

template <class... Args>
void print(std::format_string<Args...> fmt, Args&&... args) {
#if HMR_HAS_STD_PRINT
    std::print(fmt, std::forward<Args>(args)...);
#else
    std::fputs(std::format(fmt, std::forward<Args>(args)...).c_str(), stdout);
#endif
}

template <class... Args>
void println(std::format_string<Args...> fmt, Args&&... args) {
#if HMR_HAS_STD_PRINT
    std::println(fmt, std::forward<Args>(args)...);
#else
    std::string s = std::format(fmt, std::forward<Args>(args)...);
    s.push_back('\n');
    std::fputs(s.c_str(), stdout);
#endif
}

inline void println() { std::fputc('\n', stdout); }

template <class... Args>
void eprintln(std::format_string<Args...> fmt, Args&&... args) {
#if HMR_HAS_STD_PRINT
    std::println(stderr, fmt, std::forward<Args>(args)...);
#else
    std::string s = std::format(fmt, std::forward<Args>(args)...);
    s.push_back('\n');
    std::fputs(s.c_str(), stderr);
#endif
}

}  // namespace hmr
