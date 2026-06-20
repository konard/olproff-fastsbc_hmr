// SPDX-License-Identifier: MIT
//
// Unit tests for the indentation-aware lexer.

#include <vector>

#include "Test.hpp"
#include "hmr/parser/Lexer.hpp"

using namespace hmr::parser;

namespace {
std::vector<TokenKind> kinds(const std::vector<Token>& toks) {
    std::vector<TokenKind> k;
    for (const auto& t : toks) k.push_back(t.kind);
    return k;
}
}  // namespace

TEST(Lexer, IndentDedent) {
    auto r = Lexer("sip-manipulation\n"
                   "        header-rule\n"
                   "                name foo\n")
                 .tokenize();
    REQUIRE(r.has_value());
    std::vector<TokenKind> expected = {
        TokenKind::Word,    TokenKind::Newline,  // sip-manipulation
        TokenKind::Indent,  TokenKind::Word,     // header-rule
        TokenKind::Newline, TokenKind::Indent,
        TokenKind::Word,    TokenKind::Word,      // name foo
        TokenKind::Newline, TokenKind::Dedent,
        TokenKind::Dedent,  TokenKind::EndOfFile,
    };
    CHECK(kinds(*r) == expected);
}

TEST(Lexer, CommentsAndBlankLinesIgnored) {
    auto r = Lexer("# header comment\n"
                   "sip-manipulation\n"
                   "\n"
                   "        name  X   # trailing\n")
                 .tokenize();
    REQUIRE(r.has_value());
    // sip-manipulation NEWLINE INDENT name X NEWLINE DEDENT EOF
    REQUIRE(r->size() == 8);
    CHECK_EQ((*r)[0].kind, TokenKind::Word);
    CHECK_EQ((*r)[0].text, std::string("sip-manipulation"));
    CHECK_EQ((*r)[3].text, std::string("name"));
    CHECK_EQ((*r)[4].text, std::string("X"));
}

TEST(Lexer, QuotedString) {
    auto r = Lexer("        description  \"hello world\"\n").tokenize();
    REQUIRE(r.has_value());
    // The first content token is the word `description`, then the string.
    bool sawString = false;
    for (const auto& t : *r) {
        if (t.kind == TokenKind::String) {
            CHECK_EQ(t.text, std::string("hello world"));
            sawString = true;
        }
    }
    CHECK(sawString);
}

TEST(Lexer, UnterminatedStringIsError) {
    auto r = Lexer("name  \"oops\n").tokenize();
    CHECK(!r.has_value());
}

TEST(Lexer, InconsistentDedentIsError) {
    auto r = Lexer("a\n"
                   "        b\n"
                   "    c\n")  // dedent to col 4 matches no outer level
                 .tokenize();
    CHECK(!r.has_value());
}
