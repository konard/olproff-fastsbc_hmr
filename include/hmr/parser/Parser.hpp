// SPDX-License-Identifier: MIT
//
// Parser.hpp — recursive-descent parser building an AST from the token stream.
//
// Grammar (see grammar/Hmr.g4 for the ANTLR4 form):
//   ruleset      : 'sip-manipulation' NAME? NEWLINE block(attr | headerRule)
//   headerRule   : 'header-rule' NAME? NEWLINE block(attr | elementRule)
//   elementRule  : 'element-rule' NAME? NEWLINE block(attr)
//   attr         : KEY value* NEWLINE
// where block(x) is INDENT x+ DEDENT. Unknown attribute keys and unsupported
// sub-blocks (mime-rules, ...) are skipped with a warning so full Oracle
// configurations parse without aborting.

#pragma once

#include <string_view>
#include <vector>

#include "hmr/ast/Ast.hpp"
#include "hmr/diagnostics/Diagnostics.hpp"
#include "hmr/parser/Token.hpp"

namespace hmr::parser {

class Parser {
public:
    explicit Parser(std::vector<Token> tokens) : toks_(std::move(tokens)) {}

    // Parse a full ruleset. Non-fatal issues accumulate in warnings().
    [[nodiscard]] Result<ast::Ruleset> parse();

    [[nodiscard]] const std::vector<Diagnostic>& warnings() const {
        return warnings_;
    }

    // Convenience: lex + parse `source` in one call.
    [[nodiscard]] static Result<ast::Ruleset> parseString(
        std::string_view source);

private:
    std::vector<Token> toks_;
    std::size_t pos_ = 0;
    std::vector<Diagnostic> warnings_;

    [[nodiscard]] const Token& cur() const { return toks_[pos_]; }
    [[nodiscard]] const Token& peek(std::size_t n = 1) const {
        std::size_t i = pos_ + n;
        return i < toks_.size() ? toks_[i] : toks_.back();
    }
    [[nodiscard]] bool at(TokenKind k) const { return cur().kind == k; }
    const Token& advance() { return toks_[pos_++]; }

    Result<void> expect(TokenKind k, std::string_view what);
    void warn(std::string message, SourceLocation loc);

    Result<ast::Ruleset> parseRuleset();
    Result<ast::HeaderRule> parseHeaderRule();
    Result<ast::ElementRule> parseElementRule();

    // Collect the content tokens of an attribute line into (raw, quoted).
    struct AttrValue {
        std::string raw;
        bool quoted = false;     // single double-quoted token
        std::size_t count = 0;   // number of content tokens
        SourceLocation loc;
    };
    AttrValue readAttrValue();

    // Consume an entire INDENT..DEDENT block, ignoring its contents.
    void skipBlock();
};

}  // namespace hmr::parser
