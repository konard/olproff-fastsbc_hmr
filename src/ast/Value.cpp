// SPDX-License-Identifier: MIT

#include "hmr/ast/Value.hpp"

#include <cctype>

namespace hmr::ast {

namespace {

bool isIdentStart(char c) {
    return std::isalpha(static_cast<unsigned char>(c)) || c == '_';
}
bool isIdentCont(char c) {
    return std::isalnum(static_cast<unsigned char>(c)) || c == '_';
}
bool isAllUpper(const std::string& s) {
    bool sawAlpha = false;
    for (char c : s) {
        if (std::isalpha(static_cast<unsigned char>(c))) {
            sawAlpha = true;
            if (std::islower(static_cast<unsigned char>(c))) return false;
        }
    }
    return sawAlpha;
}

// Parse a single `$...` reference starting at text[i] (text[i] == '$').
// Advances i past the reference and returns the ValueRef.
ValueRef parseRef(const std::string& text, std::size_t& i) {
    ValueRef ref;
    ++i;  // consume '$'

    // Braced form: ${NAME}
    if (i < text.size() && text[i] == '{') {
        ++i;
        std::string name;
        while (i < text.size() && text[i] != '}') name.push_back(text[i++]);
        if (i < text.size()) ++i;  // consume '}'
        ref.kind = RefKind::Variable;
        ref.name = name;
        return ref;
    }

    // Pure capture index: $0 .. $9...
    if (i < text.size() && std::isdigit(static_cast<unsigned char>(text[i]))) {
        std::string num;
        while (i < text.size() &&
               std::isdigit(static_cast<unsigned char>(text[i])))
            num.push_back(text[i++]);
        ref.kind = RefKind::Capture;
        ref.captureIndex = std::stoi(num);
        ref.name = num;
        return ref;
    }

    // Identifier, possibly a dotted back-reference chain: $rule.$elem.$N
    std::string path;
    while (i < text.size() && isIdentStart(text[i])) {
        std::string ident;
        while (i < text.size() && isIdentCont(text[i])) ident.push_back(text[i++]);
        if (!path.empty()) path.push_back('.');
        path += ident;
        // Continue the chain only on a ".$" sequence.
        if (i + 1 < text.size() && text[i] == '.' && text[i + 1] == '$') {
            i += 2;  // consume ".$"
            // A trailing numeric segment is the capture index.
            if (i < text.size() &&
                std::isdigit(static_cast<unsigned char>(text[i]))) {
                std::string num;
                while (i < text.size() &&
                       std::isdigit(static_cast<unsigned char>(text[i])))
                    num.push_back(text[i++]);
                ref.captureIndex = std::stoi(num);
                path += ".$" + num;
                break;
            }
            continue;  // another named segment follows
        }
        break;
    }

    ref.name = path;
    // All-uppercase single identifier with no dotted chain → built-in variable.
    if (path.find('.') == std::string::npos && isAllUpper(path))
        ref.kind = RefKind::Variable;
    else
        ref.kind = RefKind::RuleRef;
    return ref;
}

// Scan `text` for embedded references, appending literal/ref segments.
void interpolate(const std::string& text, std::vector<ValueSegment>& out) {
    std::string literal;
    auto flush = [&] {
        if (!literal.empty()) {
            out.push_back({false, literal, {}});
            literal.clear();
        }
    };
    std::size_t i = 0;
    while (i < text.size()) {
        char c = text[i];
        if (c == '$' && i + 1 < text.size() &&
            (isIdentStart(text[i + 1]) ||
             std::isdigit(static_cast<unsigned char>(text[i + 1])) ||
             text[i + 1] == '{')) {
            flush();
            ValueRef ref = parseRef(text, i);
            out.push_back({true, "", ref});
            continue;
        }
        literal.push_back(c);
        ++i;
    }
    flush();
}

// Split a new-value expression on the top-level '+' concatenation operator.
std::vector<std::string> splitConcat(const std::string& s) {
    std::vector<std::string> parts;
    std::string cur;
    for (char c : s) {
        if (c == '+') {
            parts.push_back(cur);
            cur.clear();
        } else {
            cur.push_back(c);
        }
    }
    parts.push_back(cur);
    return parts;
}

}  // namespace

Value Value::parse(std::string raw, ValueMode mode, bool quoted) {
    Value v;
    v.raw_ = raw;

    if (quoted || mode == ValueMode::Literal) {
        if (!raw.empty()) v.segments_.push_back({false, raw, {}});
        return v;
    }

    if (mode == ValueMode::MatchValue) {
        std::string body = raw;
        if (!body.empty() && body.front() == '!') {
            v.negated_ = true;
            body.erase(body.begin());
        }
        if (!body.empty() && body.front() == '$') {
            // A $reference expression (boolean/back-reference), interpolated.
            v.refExpression_ = true;
            interpolate(body, v.segments_);
        } else if (!body.empty()) {
            // A regex / plain literal — keep verbatim as a single segment.
            v.segments_.push_back({false, body, {}});
        }
        return v;
    }

    // NewValue: concatenation of operands, each interpolated.
    for (const std::string& part : splitConcat(raw)) {
        if (!part.empty()) interpolate(part, v.segments_);
    }
    return v;
}

bool Value::isPureLiteral() const {
    for (const auto& s : segments_)
        if (s.isRef) return false;
    return true;
}

std::string Value::literalText() const {
    std::string out;
    for (const auto& s : segments_)
        if (!s.isRef) out += s.literal;
    return out;
}

bool Value::isSingleRef() const {
    return segments_.size() == 1 && segments_.front().isRef;
}

bool Value::isRegexLiteral() const {
    return !refExpression_ && isPureLiteral();
}

}  // namespace hmr::ast
