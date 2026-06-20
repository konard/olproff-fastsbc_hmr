// SPDX-License-Identifier: MIT
//
// Token.hpp — token model for the indentation-aware HMR lexer.

#pragma once

#include <cstdint>
#include <string>
#include <string_view>

#include "hmr/diagnostics/Diagnostics.hpp"

namespace hmr::parser {

enum class TokenKind : std::uint8_t {
    EndOfFile,  // end of input
    Newline,    // logical end of a (non-blank) line
    Indent,     // indentation increased
    Dedent,     // indentation decreased
    Word,       // a bareword chunk: keyword, attribute key, or bare value
    String,     // a double-quoted string literal (quotes stripped)
};

[[nodiscard]] constexpr std::string_view to_string(TokenKind k) noexcept {
    switch (k) {
        case TokenKind::EndOfFile: return "EOF";
        case TokenKind::Newline:   return "NEWLINE";
        case TokenKind::Indent:    return "INDENT";
        case TokenKind::Dedent:    return "DEDENT";
        case TokenKind::Word:      return "WORD";
        case TokenKind::String:    return "STRING";
    }
    return "?";
}

struct Token {
    TokenKind kind = TokenKind::EndOfFile;
    std::string text;     // lexeme (for Word/String); empty for structural tokens
    SourceLocation loc;   // 1-based line/column of the first character

    [[nodiscard]] bool is(TokenKind k) const noexcept { return kind == k; }

    // True when this is a content token whose text equals `s`.
    [[nodiscard]] bool isWord(std::string_view s) const noexcept {
        return kind == TokenKind::Word && text == s;
    }

    // Content tokens carry a value (Word or String).
    [[nodiscard]] bool isContent() const noexcept {
        return kind == TokenKind::Word || kind == TokenKind::String;
    }
};

}  // namespace hmr::parser
