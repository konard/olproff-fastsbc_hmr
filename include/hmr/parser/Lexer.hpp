// SPDX-License-Identifier: MIT
//
// Lexer.hpp — custom indentation-aware lexer for the HMR DSL.
//
// The HMR configuration language is line oriented and uses Python-style
// indentation to express block nesting (sip-manipulation > header-rule >
// element-rule).  This lexer turns raw source text into a flat token stream
// where indentation changes are made explicit via INDENT/DEDENT tokens, so the
// recursive-descent parser can treat block structure with a simple grammar.
//
// Lexing rules:
//   * A logical line is split into whitespace-delimited chunks. A chunk is
//     either a double-quoted string or a run of non-whitespace characters.
//   * `#` beginning a chunk starts a comment that runs to end of line.
//   * Blank lines and comment-only lines do not affect indentation.
//   * Tabs expand to the next multiple of 8 columns for indentation purposes.

#pragma once

#include <string_view>
#include <vector>

#include "hmr/diagnostics/Diagnostics.hpp"
#include "hmr/parser/Token.hpp"

namespace hmr::parser {

class Lexer {
public:
    explicit Lexer(std::string_view source) : src_(source) {}

    // Tokenize the entire input. Returns the full token stream (always ending
    // with EndOfFile) or an Error describing an indentation problem.
    [[nodiscard]] Result<std::vector<Token>> tokenize();

    // Width of one tab in columns when measuring indentation.
    static constexpr int kTabWidth = 8;

private:
    struct PhysLine {
        std::string_view text;  // raw line content (without newline)
        std::uint32_t number;   // 1-based line number
    };

    std::string_view src_;
    std::vector<Token> tokens_;
    std::vector<int> indents_{0};  // indentation stack, starts at column 0

    Result<void> lexLine(const PhysLine& line);
    [[nodiscard]] static int measureIndent(std::string_view line,
                                           std::size_t& firstNonWs);
};

}  // namespace hmr::parser
