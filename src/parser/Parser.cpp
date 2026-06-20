// SPDX-License-Identifier: MIT

#include "hmr/parser/Parser.hpp"

#include <format>

#include "hmr/ast/AstFactory.hpp"
#include "hmr/parser/Lexer.hpp"

namespace hmr::parser {

using namespace hmr::ast;

void Parser::warn(std::string message, SourceLocation loc) {
    warnings_.push_back({DiagSeverity::Warning, std::move(message), loc});
}

Result<void> Parser::expect(TokenKind k, std::string_view what) {
    if (!at(k)) {
        return make_error_at(cur().loc, "expected {} but found {}", what,
                             cur().isContent() ? cur().text
                                               : std::string(to_string(cur().kind)));
    }
    advance();
    return {};
}

Parser::AttrValue Parser::readAttrValue() {
    AttrValue v;
    v.loc = cur().loc;
    std::string raw;
    bool firstString = false;
    while (cur().isContent()) {
        if (v.count == 0) firstString = cur().is(TokenKind::String);
        if (!raw.empty()) raw.push_back(' ');
        raw += cur().text;
        ++v.count;
        advance();
    }
    v.raw = std::move(raw);
    v.quoted = (v.count == 1 && firstString);
    return v;
}

void Parser::skipBlock() {
    // Called positioned just after a block keyword's NEWLINE. If the block has
    // an indented body, consume it in full (handling nesting).
    if (!at(TokenKind::Indent)) return;
    int depth = 0;
    do {
        if (at(TokenKind::Indent)) ++depth;
        else if (at(TokenKind::Dedent)) --depth;
        else if (at(TokenKind::EndOfFile)) return;
        advance();
    } while (depth > 0);
}

Result<ElementRule> Parser::parseElementRule() {
    ElementRule er;
    er.loc = cur().loc;
    advance();  // 'element-rule'
    if (cur().isContent()) er.name = advance().text;  // optional inline name
    if (auto r = expect(TokenKind::Newline, "newline after 'element-rule'"); !r)
        return std::unexpected(r.error());

    if (!at(TokenKind::Indent)) return er;  // empty element-rule
    advance();  // INDENT

    while (!at(TokenKind::Dedent) && !at(TokenKind::EndOfFile)) {
        if (!at(TokenKind::Word)) {
            return make_error_at(cur().loc,
                                 "expected attribute name in element-rule");
        }
        std::string key = AstFactory::lower(cur().text);
        SourceLocation keyLoc = cur().loc;
        advance();
        AttrValue val = readAttrValue();

        if (key == "name") {
            er.name = val.raw;
        } else if (key == "parameter-name") {
            er.parameterName = val.raw;
        } else if (key == "type") {
            if (val.count) {
                auto t = AstFactory::elementType(val.raw);
                if (!t)
                    return make_error_at(val.loc, "unknown element type '{}'",
                                         val.raw);
                er.type = *t;
            }
        } else if (key == "action") {
            if (val.count) {
                auto a = AstFactory::elementAction(val.raw);
                if (!a)
                    return make_error_at(val.loc, "unknown element action '{}'",
                                         val.raw);
                er.action = *a;
            }
        } else if (key == "match-val-type") {
            if (val.count) {
                auto m = AstFactory::matchValType(val.raw);
                if (!m)
                    return make_error_at(val.loc,
                                         "unknown match-val-type '{}'", val.raw);
                er.matchValType = *m;
            }
        } else if (key == "comparison-type") {
            if (val.count) {
                auto c = AstFactory::comparison(val.raw);
                if (!c)
                    return make_error_at(val.loc,
                                         "unknown comparison-type '{}'", val.raw);
                er.comparison = *c;
            }
        } else if (key == "match-value") {
            er.matchValue = Value::parse(val.raw, ValueMode::MatchValue, val.quoted);
        } else if (key == "new-value") {
            er.newValue = Value::parse(val.raw, ValueMode::NewValue, val.quoted);
        } else {
            warn(std::format("ignoring unknown element-rule attribute '{}'", key),
                 keyLoc);
        }
        if (auto r = expect(TokenKind::Newline, "newline after attribute"); !r)
            return std::unexpected(r.error());
    }
    if (auto r = expect(TokenKind::Dedent, "dedent ending element-rule"); !r)
        return std::unexpected(r.error());
    return er;
}

Result<HeaderRule> Parser::parseHeaderRule() {
    HeaderRule hr;
    hr.loc = cur().loc;
    advance();  // 'header-rule'
    if (cur().isContent()) hr.name = advance().text;  // optional inline name
    if (auto r = expect(TokenKind::Newline, "newline after 'header-rule'"); !r)
        return std::unexpected(r.error());

    if (!at(TokenKind::Indent)) return hr;  // empty header-rule
    advance();  // INDENT

    while (!at(TokenKind::Dedent) && !at(TokenKind::EndOfFile)) {
        if (!at(TokenKind::Word)) {
            return make_error_at(cur().loc,
                                 "expected attribute name in header-rule");
        }
        std::string key = AstFactory::lower(cur().text);
        SourceLocation keyLoc = cur().loc;

        if (key == "element-rule" || key == "element-rules") {
            auto er = parseElementRule();
            if (!er) return std::unexpected(er.error());
            hr.elementRules.push_back(std::move(*er));
            continue;
        }
        // Unsupported nested blocks (mime, sdp, ...): skip gracefully.
        if (AstFactory::isBlockKeyword(key) && key != "header-rule") {
            warn(std::format("skipping unsupported block '{}'", key), keyLoc);
            advance();
            if (cur().isContent()) advance();
            if (at(TokenKind::Newline)) advance();
            skipBlock();
            continue;
        }

        advance();  // key
        AttrValue val = readAttrValue();

        if (key == "name") {
            hr.name = val.raw;
        } else if (key == "header-name") {
            hr.headerName = val.raw;
        } else if (key == "action") {
            if (val.count) {
                auto a = AstFactory::headerAction(val.raw);
                if (!a)
                    return make_error_at(val.loc, "unknown header action '{}'",
                                         val.raw);
                hr.action = *a;
            }
        } else if (key == "comparison-type") {
            if (val.count) {
                auto c = AstFactory::comparison(val.raw);
                if (!c)
                    return make_error_at(val.loc,
                                         "unknown comparison-type '{}'", val.raw);
                hr.comparison = *c;
            }
        } else if (key == "msg-type") {
            if (val.count) {
                auto m = AstFactory::msgType(val.raw);
                if (!m)
                    return make_error_at(val.loc, "unknown msg-type '{}'",
                                         val.raw);
                hr.msgType = *m;
            }
        } else if (key == "methods") {
            hr.methods = AstFactory::parseMethods(val.raw);
        } else if (key == "match-value") {
            hr.matchValue = Value::parse(val.raw, ValueMode::MatchValue, val.quoted);
        } else if (key == "new-value") {
            hr.newValue = Value::parse(val.raw, ValueMode::NewValue, val.quoted);
        } else {
            warn(std::format("ignoring unknown header-rule attribute '{}'", key),
                 keyLoc);
        }
        if (auto r = expect(TokenKind::Newline, "newline after attribute"); !r)
            return std::unexpected(r.error());
    }
    if (auto r = expect(TokenKind::Dedent, "dedent ending header-rule"); !r)
        return std::unexpected(r.error());
    return hr;
}

Result<Ruleset> Parser::parseRuleset() {
    Ruleset rs;
    // Skip stray leading newlines.
    while (at(TokenKind::Newline)) advance();

    if (!cur().isWord("sip-manipulation")) {
        return make_error_at(cur().loc,
                             "expected 'sip-manipulation' at start of ruleset");
    }
    rs.loc = cur().loc;
    advance();
    if (cur().isContent()) rs.name = advance().text;  // optional inline name
    if (auto r = expect(TokenKind::Newline, "newline after 'sip-manipulation'");
        !r)
        return std::unexpected(r.error());

    if (!at(TokenKind::Indent)) return rs;  // empty ruleset
    advance();  // INDENT

    while (!at(TokenKind::Dedent) && !at(TokenKind::EndOfFile)) {
        if (!at(TokenKind::Word)) {
            return make_error_at(cur().loc,
                                 "expected attribute name in sip-manipulation");
        }
        std::string key = AstFactory::lower(cur().text);
        SourceLocation keyLoc = cur().loc;

        if (key == "header-rule" || key == "header-rules") {
            auto hr = parseHeaderRule();
            if (!hr) return std::unexpected(hr.error());
            rs.headerRules.push_back(std::move(*hr));
            continue;
        }
        if (AstFactory::isBlockKeyword(key)) {
            warn(std::format("skipping unsupported block '{}'", key), keyLoc);
            advance();
            if (cur().isContent()) advance();
            if (at(TokenKind::Newline)) advance();
            skipBlock();
            continue;
        }

        advance();  // key
        AttrValue val = readAttrValue();

        if (key == "name") {
            rs.name = val.raw;
        } else if (key == "description") {
            rs.description = val.raw;
        } else if (key == "import") {
            rs.importFile = val.raw;
        } else if (key == "export") {
            rs.exportFile = val.raw;
        } else if (key == "split-headers") {
            rs.splitHeaders = AstFactory::parseMethods(val.raw);
            warn("'split-headers' is a legacy attribute (deprecated)", keyLoc);
        } else if (key == "join-headers") {
            rs.joinHeaders = AstFactory::parseMethods(val.raw);
            warn("'join-headers' is a legacy attribute (deprecated)", keyLoc);
        } else {
            warn(std::format("ignoring unknown sip-manipulation attribute '{}'",
                             key),
                 keyLoc);
        }
        if (auto r = expect(TokenKind::Newline, "newline after attribute"); !r)
            return std::unexpected(r.error());
    }
    if (auto r = expect(TokenKind::Dedent, "dedent ending sip-manipulation"); !r)
        return std::unexpected(r.error());
    return rs;
}

Result<Ruleset> Parser::parse() {
    if (toks_.empty()) return make_error("empty token stream");
    return parseRuleset();
}

Result<Ruleset> Parser::parseString(std::string_view source) {
    Lexer lexer(source);
    auto tokens = lexer.tokenize();
    if (!tokens) return std::unexpected(tokens.error());
    Parser parser(std::move(*tokens));
    return parser.parse();
}

}  // namespace hmr::parser
