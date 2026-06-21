// SPDX-License-Identifier: MIT

#include "hmr/ast/ast_factory.hpp"

#include <array>
#include <cctype>
#include <utility>

namespace hmr::ast {

std::string AstFactory::lower(std::string_view s) {
    std::string out(s);
    for (char& c : out)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

namespace {

template <class Enum, std::size_t N>
std::optional<Enum> lookup(
    const std::array<std::pair<std::string_view, Enum>, N>& table,
    std::string_view s) {
    std::string key = AstFactory::lower(s);
    for (const auto& [name, value] : table)
        if (name == key) return value;
    return std::nullopt;
}

}  // namespace

std::optional<HeaderAction> AstFactory::header_action(std::string_view s) {
    static constexpr std::array table{
        std::pair{std::string_view{"none"}, HeaderAction::None},
        std::pair{std::string_view{"add"}, HeaderAction::Add},
        std::pair{std::string_view{"store"}, HeaderAction::Store},
        std::pair{std::string_view{"manipulate"}, HeaderAction::Manipulate},
        std::pair{std::string_view{"replace"}, HeaderAction::Replace},
        std::pair{std::string_view{"find-replace-all"},
                  HeaderAction::FindReplaceAll},
        std::pair{std::string_view{"delete"}, HeaderAction::Delete},
        std::pair{std::string_view{"delete-element"},
                  HeaderAction::DeleteElement},
        std::pair{std::string_view{"delete-header"}, HeaderAction::DeleteHeader},
        std::pair{std::string_view{"sip-manip"}, HeaderAction::SipManip},
        std::pair{std::string_view{"log"}, HeaderAction::Log},
        std::pair{std::string_view{"reject"}, HeaderAction::Reject},
    };
    return lookup(table, s);
}

std::optional<ElementAction> AstFactory::element_action(std::string_view s) {
    static constexpr std::array table{
        std::pair{std::string_view{"none"}, ElementAction::None},
        std::pair{std::string_view{"add"}, ElementAction::Add},
        std::pair{std::string_view{"store"}, ElementAction::Store},
        std::pair{std::string_view{"replace"}, ElementAction::Replace},
        std::pair{std::string_view{"delete-element"},
                  ElementAction::DeleteElement},
        std::pair{std::string_view{"delete-header"},
                  ElementAction::DeleteHeader},
        std::pair{std::string_view{"find-replace-all"},
                  ElementAction::FindReplaceAll},
        std::pair{std::string_view{"sip-manip"}, ElementAction::SipManip},
        std::pair{std::string_view{"log"}, ElementAction::Log},
        std::pair{std::string_view{"reject"}, ElementAction::Reject},
    };
    return lookup(table, s);
}

std::optional<ElementType> AstFactory::element_type(std::string_view s) {
    static constexpr std::array table{
        std::pair{std::string_view{"header-value"}, ElementType::HeaderValue},
        std::pair{std::string_view{"header-param-name"},
                  ElementType::HeaderParamName},
        std::pair{std::string_view{"header-param"}, ElementType::HeaderParam},
        std::pair{std::string_view{"uri-display"}, ElementType::UriDisplay},
        std::pair{std::string_view{"uri-user"}, ElementType::UriUser},
        std::pair{std::string_view{"uri-user-param"}, ElementType::UriUserParam},
        std::pair{std::string_view{"uri-host"}, ElementType::UriHost},
        std::pair{std::string_view{"uri-port"}, ElementType::UriPort},
        std::pair{std::string_view{"uri-param-name"}, ElementType::UriParamName},
        std::pair{std::string_view{"uri-param"}, ElementType::UriParam},
        std::pair{std::string_view{"uri-header-name"},
                  ElementType::UriHeaderName},
        std::pair{std::string_view{"uri-header"}, ElementType::UriHeader},
        std::pair{std::string_view{"status-code"}, ElementType::StatusCode},
        std::pair{std::string_view{"reason-phrase"}, ElementType::ReasonPhrase},
        std::pair{std::string_view{"uri-user-only"}, ElementType::UriUserOnly},
        std::pair{std::string_view{"uri-phone-number-only"},
                  ElementType::UriPhoneNumberOnly},
    };
    return lookup(table, s);
}

std::optional<ComparisonType> AstFactory::comparison(std::string_view s) {
    static constexpr std::array table{
        std::pair{std::string_view{"case-sensitive"},
                  ComparisonType::CaseSensitive},
        std::pair{std::string_view{"case-insensitive"},
                  ComparisonType::CaseInsensitive},
        std::pair{std::string_view{"pattern-rule"}, ComparisonType::PatternRule},
        std::pair{std::string_view{"boolean"}, ComparisonType::Boolean},
        std::pair{std::string_view{"refer-case-sensitive"},
                  ComparisonType::ReferCaseSensitive},
        std::pair{std::string_view{"refer-case-insensitive"},
                  ComparisonType::ReferCaseInsensitive},
    };
    return lookup(table, s);
}

std::optional<MatchValType> AstFactory::match_val_type(std::string_view s) {
    static constexpr std::array table{
        std::pair{std::string_view{"any"}, MatchValType::Any},
        std::pair{std::string_view{"an"}, MatchValType::Any},  // doc truncation
        std::pair{std::string_view{"ip"}, MatchValType::Ip},
        std::pair{std::string_view{"fqdn"}, MatchValType::Fqdn},
    };
    return lookup(table, s);
}

std::optional<MsgType> AstFactory::msg_type(std::string_view s) {
    static constexpr std::array table{
        std::pair{std::string_view{"any"}, MsgType::Any},
        std::pair{std::string_view{"request"}, MsgType::Request},
        std::pair{std::string_view{"reply"}, MsgType::Reply},
        std::pair{std::string_view{"out-of-dialog"}, MsgType::OutOfDialog},
    };
    return lookup(table, s);
}

std::string_view AstFactory::to_string(HeaderAction a) {
    switch (a) {
        case HeaderAction::None: return "none";
        case HeaderAction::Add: return "add";
        case HeaderAction::Store: return "store";
        case HeaderAction::Manipulate: return "manipulate";
        case HeaderAction::Replace: return "replace";
        case HeaderAction::FindReplaceAll: return "find-replace-all";
        case HeaderAction::Delete: return "delete";
        case HeaderAction::DeleteElement: return "delete-element";
        case HeaderAction::DeleteHeader: return "delete-header";
        case HeaderAction::SipManip: return "sip-manip";
        case HeaderAction::Log: return "log";
        case HeaderAction::Reject: return "reject";
    }
    return "?";
}

std::string_view AstFactory::to_string(ElementAction a) {
    switch (a) {
        case ElementAction::None: return "none";
        case ElementAction::Add: return "add";
        case ElementAction::Store: return "store";
        case ElementAction::Replace: return "replace";
        case ElementAction::DeleteElement: return "delete-element";
        case ElementAction::DeleteHeader: return "delete-header";
        case ElementAction::FindReplaceAll: return "find-replace-all";
        case ElementAction::SipManip: return "sip-manip";
        case ElementAction::Log: return "log";
        case ElementAction::Reject: return "reject";
    }
    return "?";
}

std::string_view AstFactory::to_string(ElementType t) {
    switch (t) {
        case ElementType::None: return "none";
        case ElementType::HeaderValue: return "header-value";
        case ElementType::HeaderParamName: return "header-param-name";
        case ElementType::HeaderParam: return "header-param";
        case ElementType::UriDisplay: return "uri-display";
        case ElementType::UriUser: return "uri-user";
        case ElementType::UriUserParam: return "uri-user-param";
        case ElementType::UriHost: return "uri-host";
        case ElementType::UriPort: return "uri-port";
        case ElementType::UriParamName: return "uri-param-name";
        case ElementType::UriParam: return "uri-param";
        case ElementType::UriHeaderName: return "uri-header-name";
        case ElementType::UriHeader: return "uri-header";
        case ElementType::StatusCode: return "status-code";
        case ElementType::ReasonPhrase: return "reason-phrase";
        case ElementType::UriUserOnly: return "uri-user-only";
        case ElementType::UriPhoneNumberOnly: return "uri-phone-number-only";
    }
    return "?";
}

std::string_view AstFactory::to_string(ComparisonType c) {
    switch (c) {
        case ComparisonType::CaseSensitive: return "case-sensitive";
        case ComparisonType::CaseInsensitive: return "case-insensitive";
        case ComparisonType::PatternRule: return "pattern-rule";
        case ComparisonType::Boolean: return "boolean";
        case ComparisonType::ReferCaseSensitive: return "refer-case-sensitive";
        case ComparisonType::ReferCaseInsensitive:
            return "refer-case-insensitive";
    }
    return "?";
}

std::string_view AstFactory::to_string(MatchValType m) {
    switch (m) {
        case MatchValType::Any: return "any";
        case MatchValType::Ip: return "ip";
        case MatchValType::Fqdn: return "fqdn";
    }
    return "?";
}

std::string_view AstFactory::to_string(MsgType m) {
    switch (m) {
        case MsgType::Any: return "any";
        case MsgType::Request: return "request";
        case MsgType::Reply: return "reply";
        case MsgType::OutOfDialog: return "out-of-dialog";
    }
    return "?";
}

bool AstFactory::is_block_keyword(std::string_view s) {
    std::string k = lower(s);
    return k == "sip-manipulation" || k == "header-rule" ||
           k == "header-rules" || k == "element-rule" || k == "element-rules";
}

std::vector<std::string> AstFactory::parse_methods(std::string_view s) {
    std::vector<std::string> out;
    std::string cur;
    auto flush = [&] {
        if (!cur.empty()) {
            for (char& c : cur)
                c = static_cast<char>(
                    std::toupper(static_cast<unsigned char>(c)));
            out.push_back(cur);
            cur.clear();
        }
    };
    for (char c : s) {
        if (c == ',' || c == ' ' || c == '\t')
            flush();
        else
            cur.push_back(c);
    }
    flush();
    return out;
}

}  // namespace hmr::ast
