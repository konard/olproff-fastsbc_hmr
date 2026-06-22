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

// value.hpp — parsed representation of an HMR match-value / new-value.
//
// HMR values are a small expression language:
//   * built-in variables          $LOCAL_IP, $REMOTE_IP, $TO_HOST, ...
//   * capture back-references      $0, $1 (self-storing) and $rule.$N
//   * string interpolation         sip:$RURI_USER@$LOCAL_IP
//   * concatenation                $rule.$1+edited+$rule.$3   (the '+' operator)
//   * boolean negation             !$whitelist.$check          (match-value)
//
// A Value is parsed into an ordered list of segments (literal text or a
// reference). The code generator walks the segments to either fold a pure
// literal into .rodata or emit runtime lookups + concatenation.

#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace hmr::ast {

// What a `$...` reference resolves to.
enum class RefKind : std::uint8_t {
    Variable,  // built-in HMR variable, e.g. $LOCAL_IP (name = "LOCAL_IP")
    Capture,   // self-storing capture index, e.g. $1 (index = 1)
    RuleRef,   // back-reference to a stored rule/element (name = full path)
};

struct ValueRef {
    RefKind kind = RefKind::Variable;
    std::string name;         // variable name or rule-ref path (no leading '$')
    int capture_index = -1;   // capture index for Capture, or trailing .$N
};

struct ValueSegment {
    bool is_ref = false;
    std::string literal;  // valid when !is_ref
    ValueRef ref;         // valid when is_ref
};

// How the raw token stream should be interpreted.
enum class ValueMode : std::uint8_t {
    Literal,    // quoted string / description: take verbatim, no parsing
    MatchValue, // regex literal, or a $reference expression (no '+' splitting)
    NewValue,   // concatenation expression ('+' operator) + interpolation
};

class Value {
public:
    Value() = default;

    // Parse `raw` according to `mode`. `quoted` forces Literal handling.
    static Value parse(std::string raw, ValueMode mode, bool quoted = false);

    [[nodiscard]] const std::string& raw() const { return raw_; }
    [[nodiscard]] const std::vector<ValueSegment>& segments() const {
        return segments_;
    }
    [[nodiscard]] bool empty() const { return raw_.empty(); }
    [[nodiscard]] bool negated() const { return negated_; }

    // True when every segment is literal text (foldable into a constant).
    [[nodiscard]] bool is_pure_literal() const;
    // Concatenated literal text (only meaningful when is_pure_literal()).
    [[nodiscard]] std::string literal_text() const;
    // True when the value is exactly one reference and nothing else.
    [[nodiscard]] bool is_single_ref() const;

    // For pattern-rule match-values that are plain regexes (one literal segment
    // and not a $reference), this is the regex source.
    [[nodiscard]] bool is_regex_literal() const;

private:
    std::string raw_;
    std::vector<ValueSegment> segments_;
    bool negated_ = false;
    bool ref_expression_ = false;  // match-value that is a $reference, not regex
};

}  // namespace hmr::ast
