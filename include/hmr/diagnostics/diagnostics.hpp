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

// Diagnostics.hpp — error model built on std::expected (C++23/26).
//
// Every fallible operation in the front-end returns hmr::Result<T>, i.e.
// std::expected<T, hmr::Error>.  An Error carries a list of diagnostics with
// source locations so the CLI can render compiler-style messages.

#pragma once

#include <cstdint>
#include <expected>
#include <format>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace hmr {

struct SourceLocation {
    std::uint32_t line = 0;    // 1-based; 0 == unknown
    std::uint32_t column = 0;  // 1-based; 0 == unknown
};

enum class DiagSeverity : std::uint8_t { Error, Warning, Note };

struct Diagnostic {
    DiagSeverity severity = DiagSeverity::Error;
    std::string message;
    SourceLocation loc;

    [[nodiscard]] std::string format() const {
        std::string_view sev = severity == DiagSeverity::Error     ? "error"
                               : severity == DiagSeverity::Warning ? "warning"
                                                                   : "note";
        if (loc.line == 0)
            return std::format("{}: {}", sev, message);
        return std::format("{}:{}: {}: {}", loc.line, loc.column, sev, message);
    }
};

// Aggregate error carrying one or more diagnostics. The first diagnostic is the
// primary error; subsequent ones are related notes/warnings.
class Error {
public:
    Error() = default;

    explicit Error(std::string message, SourceLocation loc = {}) {
        diagnostics_.push_back(
            {DiagSeverity::Error, std::move(message), loc});
    }

    explicit Error(Diagnostic diag) { diagnostics_.push_back(std::move(diag)); }

    explicit Error(std::vector<Diagnostic> diags)
        : diagnostics_(std::move(diags)) {}

    [[nodiscard]] const std::vector<Diagnostic>& diagnostics() const {
        return diagnostics_;
    }

    Error& add(Diagnostic diag) {
        diagnostics_.push_back(std::move(diag));
        return *this;
    }

    [[nodiscard]] std::string message() const {
        return diagnostics_.empty() ? std::string{"unknown error"}
                                    : diagnostics_.front().message;
    }

    // Render every diagnostic, one per line.
    [[nodiscard]] std::string format() const {
        std::string out;
        for (const auto& d : diagnostics_) {
            out += d.format();
            out.push_back('\n');
        }
        if (!out.empty()) out.pop_back();
        return out;
    }

private:
    std::vector<Diagnostic> diagnostics_;
};

// The canonical fallible-result alias used throughout the front-end.
template <class T>
using Result = std::expected<T, Error>;

// Convenience helper to build a std::unexpected<Error> from a message + loc.
[[nodiscard]] inline std::unexpected<Error> make_error(std::string message,
                                                       SourceLocation loc = {}) {
    return std::unexpected<Error>(Error{std::move(message), loc});
}

template <class... Args>
[[nodiscard]] std::unexpected<Error> make_error_at(
    SourceLocation loc, std::format_string<Args...> fmt, Args&&... args) {
    return std::unexpected<Error>(
        Error{std::format(fmt, std::forward<Args>(args)...), loc});
}

}  // namespace hmr
