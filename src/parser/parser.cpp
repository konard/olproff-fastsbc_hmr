// SPDX-License-Identifier: MIT
//
// parser.cpp — visitor over the ANTLR4-generated HMR parse tree.
//
// The generated HmrLexer/HmrParser (from grammar/Hmr.g4) do the tokenizing and
// keyword-delimited block parsing; this file walks the resulting tree and
// builds the strongly-typed AST, mapping keywords through ast::AstFactory and
// folding values through ast::Value. Recognized-but-unsupported blocks
// (mime-*-rule) and unknown attribute keys become warnings; unknown enum values
// become errors. All diagnostics are accumulated (error recovery) so a single
// parse reports every problem rather than aborting at the first.

#include "hmr/parser/parser.hpp"

#include <cstdint>
#include <format>

#include <antlr4-runtime.h>

#include "HmrLexer.h"
#include "HmrParser.h"
#include "hmr/ast/ast_factory.hpp"

namespace hmr::parser {

using namespace hmr::ast;

namespace {

// --- source locations -------------------------------------------------------
SourceLocation loc_of(antlr4::Token* t) {
    if (t == nullptr) return {};
    return {static_cast<std::uint32_t>(t->getLine()),
            static_cast<std::uint32_t>(t->getCharPositionInLine() + 1)};
}
SourceLocation loc_of(antlr4::ParserRuleContext* c) {
    return c != nullptr ? loc_of(c->getStart()) : SourceLocation{};
}

// Collects every lexer/parser syntax error into a diagnostics list instead of
// printing to stderr, so the parser can report all of them at once.
class CollectingErrorListener final : public antlr4::BaseErrorListener {
public:
    explicit CollectingErrorListener(std::vector<Diagnostic>& out) : out_(out) {}

    void syntaxError(antlr4::Recognizer* /*recognizer*/,
                     antlr4::Token* /*offendingSymbol*/, std::size_t line,
                     std::size_t col, const std::string& msg,
                     std::exception_ptr /*e*/) override {
        out_.push_back({DiagSeverity::Error, msg,
                        SourceLocation{static_cast<std::uint32_t>(line),
                                       static_cast<std::uint32_t>(col + 1)}});
    }

private:
    std::vector<Diagnostic>& out_;
};

// Decode a STRING lexeme (which still carries its surrounding double quotes)
// into its textual value, applying the simple backslash escapes the grammar
// allows (\n \t \r \" \\); any other escaped char is taken literally.
std::string unescape_string(std::string_view lexeme) {
    std::string out;
    if (lexeme.size() < 2) return out;
    std::string_view body = lexeme.substr(1, lexeme.size() - 2);
    for (std::size_t i = 0; i < body.size(); ++i) {
        char c = body[i];
        if (c == '\\' && i + 1 < body.size()) {
            char e = body[++i];
            switch (e) {
                case 'n': out.push_back('\n'); break;
                case 't': out.push_back('\t'); break;
                case 'r': out.push_back('\r'); break;
                case '"': out.push_back('"'); break;
                case '\\': out.push_back('\\'); break;
                default: out.push_back(e); break;
            }
        } else {
            out.push_back(c);
        }
    }
    return out;
}

struct AttrValue {
    std::string raw;
    bool quoted = false;       // exactly one atom, and it is a quoted string
    std::size_t count = 0;     // number of value atoms
    SourceLocation loc;
};

// Join a value's atoms with single spaces, replicating the legacy lexer: a
// quoted string contributes its decoded content; a single string atom sets the
// `quoted` flag (verbatim / Literal handling downstream).
AttrValue read_value(HmrParser::ValueContext* v, SourceLocation fallback) {
    AttrValue a;
    a.loc = fallback;
    if (v == nullptr) return a;
    a.loc = loc_of(v);

    const auto atoms = v->valueAtom();
    a.count = atoms.size();
    bool first_string = false;
    std::string raw;
    for (std::size_t k = 0; k < atoms.size(); ++k) {
        auto* atom = atoms[k];
        std::string text;
        if (auto* str = atom->STRING(); str != nullptr) {
            if (k == 0) first_string = true;
            text = unescape_string(str->getText());
        } else {
            text = atom->getText();
        }
        if (!raw.empty()) raw.push_back(' ');
        raw += text;
    }
    a.raw = std::move(raw);
    a.quoted = (a.count == 1 && first_string);
    return a;
}

void unknown_enum(std::vector<Diagnostic>& errors, std::string_view what,
                  const AttrValue& val) {
    errors.push_back({DiagSeverity::Error,
                      std::format("unknown {} '{}'", what, val.raw), val.loc});
}

// --- attribute application (per block) --------------------------------------

void apply_element_attr(ElementRule& er, HmrParser::AttributeContext* attr,
                        std::vector<Diagnostic>& errors,
                        std::vector<Diagnostic>& warnings) {
    const SourceLocation key_loc = loc_of(attr->key());
    const std::string key = AstFactory::lower(attr->key()->getText());
    const AttrValue val = read_value(attr->value(), key_loc);

    if (key == "name") {
        er.name = val.raw;
    } else if (key == "parameter-name") {
        er.parameter_name = val.raw;
    } else if (key == "type") {
        if (val.count) {
            if (auto t = AstFactory::element_type(val.raw)) er.type = *t;
            else unknown_enum(errors, "element type", val);
        }
    } else if (key == "action") {
        if (val.count) {
            if (auto a = AstFactory::element_action(val.raw)) er.action = *a;
            else unknown_enum(errors, "element action", val);
        }
    } else if (key == "match-val-type") {
        if (val.count) {
            if (auto m = AstFactory::match_val_type(val.raw))
                er.match_val_type = *m;
            else unknown_enum(errors, "match-val-type", val);
        }
    } else if (key == "comparison-type") {
        if (val.count) {
            if (auto c = AstFactory::comparison(val.raw)) er.comparison = *c;
            else unknown_enum(errors, "comparison-type", val);
        }
    } else if (key == "match-value") {
        er.match_value = Value::parse(val.raw, ValueMode::MatchValue, val.quoted);
    } else if (key == "new-value") {
        er.new_value = Value::parse(val.raw, ValueMode::NewValue, val.quoted);
    } else {
        warnings.push_back(
            {DiagSeverity::Warning,
             std::format("ignoring unknown element-rule attribute '{}'", key),
             key_loc});
    }
}

void apply_header_attr(HeaderRule& hr, HmrParser::AttributeContext* attr,
                       std::vector<Diagnostic>& errors,
                       std::vector<Diagnostic>& warnings) {
    const SourceLocation key_loc = loc_of(attr->key());
    const std::string key = AstFactory::lower(attr->key()->getText());
    const AttrValue val = read_value(attr->value(), key_loc);

    if (key == "name") {
        hr.name = val.raw;
    } else if (key == "header-name") {
        hr.header_name = val.raw;
    } else if (key == "action") {
        if (val.count) {
            if (auto a = AstFactory::header_action(val.raw)) hr.action = *a;
            else unknown_enum(errors, "header action", val);
        }
    } else if (key == "comparison-type") {
        if (val.count) {
            if (auto c = AstFactory::comparison(val.raw)) hr.comparison = *c;
            else unknown_enum(errors, "comparison-type", val);
        }
    } else if (key == "msg-type") {
        if (val.count) {
            if (auto m = AstFactory::msg_type(val.raw)) hr.msg_type = *m;
            else unknown_enum(errors, "msg-type", val);
        }
    } else if (key == "methods") {
        hr.methods = AstFactory::parse_methods(val.raw);
    } else if (key == "match-value") {
        hr.match_value = Value::parse(val.raw, ValueMode::MatchValue, val.quoted);
    } else if (key == "new-value") {
        hr.new_value = Value::parse(val.raw, ValueMode::NewValue, val.quoted);
    } else {
        warnings.push_back(
            {DiagSeverity::Warning,
             std::format("ignoring unknown header-rule attribute '{}'", key),
             key_loc});
    }
}

void apply_ruleset_attr(Ruleset& rs, HmrParser::AttributeContext* attr,
                        std::vector<Diagnostic>& /*errors*/,
                        std::vector<Diagnostic>& warnings) {
    const SourceLocation key_loc = loc_of(attr->key());
    const std::string key = AstFactory::lower(attr->key()->getText());
    const AttrValue val = read_value(attr->value(), key_loc);

    if (key == "name") {
        rs.name = val.raw;
    } else if (key == "description") {
        rs.description = val.raw;
    } else if (key == "import") {
        rs.import_file = val.raw;
    } else if (key == "export") {
        rs.export_file = val.raw;
    } else if (key == "split-headers") {
        rs.split_headers = AstFactory::parse_methods(val.raw);
        warnings.push_back({DiagSeverity::Warning,
                            "'split-headers' is a legacy attribute (deprecated)",
                            key_loc});
    } else if (key == "join-headers") {
        rs.join_headers = AstFactory::parse_methods(val.raw);
        warnings.push_back({DiagSeverity::Warning,
                            "'join-headers' is a legacy attribute (deprecated)",
                            key_loc});
    } else {
        warnings.push_back(
            {DiagSeverity::Warning,
             std::format("ignoring unknown sip-manipulation attribute '{}'", key),
             key_loc});
    }
}

// --- tree -> AST ------------------------------------------------------------

ElementRule build_element_rule(HmrParser::ElementRuleContext* ctx,
                               std::vector<Diagnostic>& errors,
                               std::vector<Diagnostic>& warnings) {
    ElementRule er;
    er.loc = loc_of(ctx);
    if (auto* v = ctx->value()) er.name = read_value(v, er.loc).raw;
    for (auto* item : ctx->elementRuleItem()) {
        if (auto* attr = item->attribute())
            apply_element_attr(er, attr, errors, warnings);
    }
    return er;
}

HeaderRule build_header_rule(HmrParser::HeaderRuleContext* ctx,
                             std::vector<Diagnostic>& errors,
                             std::vector<Diagnostic>& warnings) {
    HeaderRule hr;
    hr.loc = loc_of(ctx);
    if (auto* v = ctx->value()) hr.name = read_value(v, hr.loc).raw;
    for (auto* item : ctx->headerRuleItem()) {
        if (auto* erc = item->elementRule())
            hr.element_rules.push_back(
                build_element_rule(erc, errors, warnings));
        else if (auto* attr = item->attribute())
            apply_header_attr(hr, attr, errors, warnings);
    }
    return hr;
}

Ruleset build_ruleset(HmrParser::ManipulationContext* ctx,
                      std::vector<Diagnostic>& errors,
                      std::vector<Diagnostic>& warnings) {
    Ruleset rs;
    rs.loc = loc_of(ctx);
    if (auto* v = ctx->value()) rs.name = read_value(v, rs.loc).raw;
    for (auto* item : ctx->manipItem()) {
        if (auto* hrc = item->headerRule())
            rs.header_rules.push_back(build_header_rule(hrc, errors, warnings));
        else if (auto* mrc = item->mimeRule())
            warnings.push_back(
                {DiagSeverity::Warning,
                 std::format("skipping unsupported block '{}'",
                             mrc->getStart()->getText()),
                 loc_of(mrc)});
        else if (auto* attr = item->attribute())
            apply_ruleset_attr(rs, attr, errors, warnings);
    }
    return rs;
}

}  // namespace

Result<ast::Ruleset> Parser::parse() {
    // ANTLR's NEWLINE terminates every attribute line, so guarantee the input
    // ends with one even when the file has no trailing newline.
    std::string text = source_;
    if (text.empty() || text.back() != '\n') text.push_back('\n');

    antlr4::ANTLRInputStream input(text);
    HmrLexer lexer(&input);
    antlr4::CommonTokenStream tokens(&lexer);
    HmrParser parser(&tokens);

    std::vector<Diagnostic> errors;
    CollectingErrorListener listener(errors);
    lexer.removeErrorListeners();
    lexer.addErrorListener(&listener);
    parser.removeErrorListeners();
    parser.addErrorListener(&listener);

    HmrParser::UnitContext* unit = parser.unit();

    ast::Ruleset rs;
    const auto manips = unit->manipulation();
    if (manips.empty()) {
        if (errors.empty())
            errors.push_back(
                {DiagSeverity::Error,
                 "expected 'sip-manipulation' at start of ruleset", {}});
    } else {
        rs = build_ruleset(manips[0], errors, warnings_);
        for (std::size_t i = 1; i < manips.size(); ++i)
            warnings_.push_back(
                {DiagSeverity::Warning,
                 "ignoring additional sip-manipulation block (only the first is "
                 "compiled)",
                 loc_of(manips[i])});
    }

    if (!errors.empty()) return std::unexpected(Error{std::move(errors)});
    return rs;
}

Result<ast::Ruleset> Parser::parse_string(std::string_view source) {
    Parser p(source);
    return p.parse();
}

}  // namespace hmr::parser
