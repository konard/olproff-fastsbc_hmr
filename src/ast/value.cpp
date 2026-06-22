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

#include "hmr/ast/value.hpp"

#include <cctype>

namespace hmr::ast {

namespace {

bool is_ident_start(char c) {
    return std::isalpha(static_cast<unsigned char>(c)) || c == '_';
}
bool is_ident_cont(char c) {
    return std::isalnum(static_cast<unsigned char>(c)) || c == '_';
}
bool is_all_upper(const std::string& s) {
    bool saw_alpha = false;
    for (char c : s) {
        if (std::isalpha(static_cast<unsigned char>(c))) {
            saw_alpha = true;
            if (std::islower(static_cast<unsigned char>(c))) return false;
        }
    }
    return saw_alpha;
}

// Parse a single `$...` reference starting at text[i] (text[i] == '$').
// Advances i past the reference and returns the ValueRef.
ValueRef parse_ref(const std::string& text, std::size_t& i) {
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
        ref.capture_index = std::stoi(num);
        ref.name = num;
        return ref;
    }

    // Identifier, possibly a dotted back-reference chain: $rule.$elem.$N
    std::string path;
    while (i < text.size() && is_ident_start(text[i])) {
        std::string ident;
        while (i < text.size() && is_ident_cont(text[i]))
            ident.push_back(text[i++]);
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
                ref.capture_index = std::stoi(num);
                path += ".$" + num;
                break;
            }
            continue;  // another named segment follows
        }
        break;
    }

    ref.name = path;
    // All-uppercase single identifier with no dotted chain → built-in variable.
    if (path.find('.') == std::string::npos && is_all_upper(path))
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
            (is_ident_start(text[i + 1]) ||
             std::isdigit(static_cast<unsigned char>(text[i + 1])) ||
             text[i + 1] == '{')) {
            flush();
            ValueRef ref = parse_ref(text, i);
            out.push_back({true, "", ref});
            continue;
        }
        literal.push_back(c);
        ++i;
    }
    flush();
}

// Split a new-value expression on the top-level '+' concatenation operator.
std::vector<std::string> split_concat(const std::string& s) {
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
            v.ref_expression_ = true;
            interpolate(body, v.segments_);
        } else if (!body.empty()) {
            // A regex / plain literal — keep verbatim as a single segment.
            v.segments_.push_back({false, body, {}});
        }
        return v;
    }

    // NewValue: concatenation of operands, each interpolated.
    for (const std::string& part : split_concat(raw)) {
        if (!part.empty()) interpolate(part, v.segments_);
    }
    return v;
}

bool Value::is_pure_literal() const {
    for (const auto& s : segments_)
        if (s.is_ref) return false;
    return true;
}

std::string Value::literal_text() const {
    std::string out;
    for (const auto& s : segments_)
        if (!s.is_ref) out += s.literal;
    return out;
}

bool Value::is_single_ref() const {
    return segments_.size() == 1 && segments_.front().is_ref;
}

bool Value::is_regex_literal() const {
    return !ref_expression_ && is_pure_literal();
}

}  // namespace hmr::ast
