// SPDX-License-Identifier: MIT
//
// Ast.hpp — abstract syntax tree for an HMR ruleset.
//
// The node set mirrors the Oracle sip-manipulation model:
//   Ruleset (sip-manipulation) -> HeaderRule (header-rule) -> ElementRule.
// Enumerations use the exact Oracle keyword spelling (see
// docs/oracle-hmr-reference.md). The AST is plain value-semantic data; the
// Visitor lives in AstVisitor.hpp and the string<->enum mappings + builder in
// AstFactory.hpp.

#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "hmr/ast/Value.hpp"
#include "hmr/diagnostics/Diagnostics.hpp"

namespace hmr::ast {

// --- header-rule action ----------------------------------------------------
enum class HeaderAction : std::uint8_t {
    None,
    Add,
    Store,
    Manipulate,
    Replace,
    FindReplaceAll,
    Delete,         // alias of DeleteHeader
    DeleteElement,
    DeleteHeader,
    SipManip,
    Log,
    Reject,
};

// --- element-rule action ---------------------------------------------------
enum class ElementAction : std::uint8_t {
    None,
    Add,
    Store,
    Replace,
    DeleteElement,
    DeleteHeader,
    FindReplaceAll,
    SipManip,
    Log,
    Reject,
};

// --- element-rule type -----------------------------------------------------
enum class ElementType : std::uint8_t {
    None,
    HeaderValue,
    HeaderParamName,
    HeaderParam,
    UriDisplay,
    UriUser,
    UriUserParam,
    UriHost,
    UriPort,
    UriParamName,
    UriParam,
    UriHeaderName,
    UriHeader,
    StatusCode,
    ReasonPhrase,
};

// --- comparison-type -------------------------------------------------------
enum class ComparisonType : std::uint8_t {
    CaseSensitive,
    CaseInsensitive,
    PatternRule,
    Boolean,
    ReferCaseSensitive,
    ReferCaseInsensitive,
};

// --- element-rule match-val-type -------------------------------------------
enum class MatchValType : std::uint8_t {
    Any,
    Ip,
    Fqdn,
};

// --- header-rule msg-type --------------------------------------------------
enum class MsgType : std::uint8_t {
    Any,
    Request,
    Reply,
    OutOfDialog,
};

struct ElementRule {
    std::string name;
    std::string parameterName;
    ElementType type = ElementType::None;
    ElementAction action = ElementAction::None;
    MatchValType matchValType = MatchValType::Any;
    ComparisonType comparison = ComparisonType::CaseSensitive;
    Value matchValue;
    Value newValue;
    SourceLocation loc;
};

struct HeaderRule {
    std::string name;
    std::string headerName;
    HeaderAction action = HeaderAction::None;
    ComparisonType comparison = ComparisonType::CaseSensitive;
    MsgType msgType = MsgType::Any;
    std::vector<std::string> methods;  // empty == all methods
    Value matchValue;
    Value newValue;
    std::vector<ElementRule> elementRules;
    SourceLocation loc;

    [[nodiscard]] bool hasMethodFilter() const { return !methods.empty(); }
};

struct Ruleset {
    std::string name;
    std::string description;
    std::vector<HeaderRule> headerRules;

    // Legacy / accepted-but-deprecated attributes.
    std::vector<std::string> splitHeaders;
    std::vector<std::string> joinHeaders;
    std::string importFile;
    std::string exportFile;

    SourceLocation loc;
};

// True when the action removes the whole header (delete / delete-header).
[[nodiscard]] constexpr bool removesHeader(HeaderAction a) {
    return a == HeaderAction::Delete || a == HeaderAction::DeleteHeader;
}

}  // namespace hmr::ast
