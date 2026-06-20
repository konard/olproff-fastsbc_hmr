// SPDX-License-Identifier: MIT

#include "hmr/parser/Lexer.hpp"

#include <cctype>

namespace hmr::parser {

namespace {
constexpr bool isBlankOrComment(std::string_view content) {
    return content.empty() || content.front() == '#';
}
}  // namespace

int Lexer::measureIndent(std::string_view line, std::size_t& firstNonWs) {
    int width = 0;
    std::size_t i = 0;
    for (; i < line.size(); ++i) {
        char c = line[i];
        if (c == ' ') {
            ++width;
        } else if (c == '\t') {
            width += kTabWidth - (width % kTabWidth);
        } else {
            break;
        }
    }
    firstNonWs = i;
    return width;
}

Result<void> Lexer::lexLine(const PhysLine& line) {
    std::size_t start = 0;
    const int indent = measureIndent(line.text, start);
    std::string_view content = line.text.substr(start);

    // Blank / comment-only lines do not participate in indentation tracking.
    if (isBlankOrComment(content)) return {};

    // --- Indentation handling (emit INDENT / DEDENT as needed) ----------------
    auto loc = [&](std::size_t col) {
        return SourceLocation{line.number, static_cast<std::uint32_t>(col + 1)};
    };

    if (indent > indents_.back()) {
        indents_.push_back(indent);
        tokens_.push_back({TokenKind::Indent, "", loc(start)});
    } else if (indent < indents_.back()) {
        while (indents_.back() > indent) {
            indents_.pop_back();
            tokens_.push_back({TokenKind::Dedent, "", loc(start)});
        }
        if (indents_.back() != indent) {
            return make_error_at(
                loc(start),
                "inconsistent indentation (dedent does not match any outer "
                "level)");
        }
    }

    // --- Content tokenization -------------------------------------------------
    std::size_t i = start;
    const std::string_view text = line.text;
    while (i < text.size()) {
        char c = text[i];
        if (c == ' ' || c == '\t') {
            ++i;
            continue;
        }
        if (c == '#') break;  // trailing comment

        if (c == '"') {
            // Quoted string with simple backslash escapes.
            std::size_t tokCol = i;
            ++i;  // consume opening quote
            std::string value;
            bool closed = false;
            while (i < text.size()) {
                char d = text[i];
                if (d == '\\' && i + 1 < text.size()) {
                    char e = text[i + 1];
                    switch (e) {
                        case 'n': value.push_back('\n'); break;
                        case 't': value.push_back('\t'); break;
                        case 'r': value.push_back('\r'); break;
                        case '"': value.push_back('"'); break;
                        case '\\': value.push_back('\\'); break;
                        default:
                            value.push_back(e);
                            break;
                    }
                    i += 2;
                    continue;
                }
                if (d == '"') {
                    ++i;
                    closed = true;
                    break;
                }
                value.push_back(d);
                ++i;
            }
            if (!closed) {
                return make_error_at(loc(tokCol),
                                     "unterminated string literal");
            }
            tokens_.push_back({TokenKind::String, std::move(value), loc(tokCol)});
            continue;
        }

        // Bareword: run of non-whitespace characters (stops at whitespace).
        std::size_t tokCol = i;
        std::size_t begin = i;
        while (i < text.size() && text[i] != ' ' && text[i] != '\t') ++i;
        tokens_.push_back(
            {TokenKind::Word, std::string(text.substr(begin, i - begin)),
             loc(tokCol)});
    }

    tokens_.push_back({TokenKind::Newline, "",
                       loc(text.size())});
    return {};
}

Result<std::vector<Token>> Lexer::tokenize() {
    tokens_.clear();
    indents_.assign(1, 0);

    std::uint32_t lineNo = 0;
    std::size_t pos = 0;
    while (pos <= src_.size()) {
        std::size_t nl = src_.find('\n', pos);
        std::string_view raw;
        if (nl == std::string_view::npos) {
            raw = src_.substr(pos);
            pos = src_.size() + 1;  // terminate loop after this line
        } else {
            raw = src_.substr(pos, nl - pos);
            pos = nl + 1;
        }
        // Strip a trailing carriage return (CRLF inputs).
        if (!raw.empty() && raw.back() == '\r') raw.remove_suffix(1);

        ++lineNo;
        if (auto r = lexLine({raw, lineNo}); !r) return std::unexpected(r.error());

        if (nl == std::string_view::npos) break;
    }

    // Close any still-open indentation levels at EOF.
    SourceLocation eofLoc{lineNo + 1, 1};
    while (indents_.back() > 0) {
        indents_.pop_back();
        tokens_.push_back({TokenKind::Dedent, "", eofLoc});
    }
    tokens_.push_back({TokenKind::EndOfFile, "", eofLoc});
    return tokens_;
}

}  // namespace hmr::parser
