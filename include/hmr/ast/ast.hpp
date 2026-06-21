// SPDX-License-Identifier: MIT
//
// ast.hpp — abstract syntax tree for an HMR ruleset.
//
// The node set mirrors the Oracle sip-manipulation model:
//   Ruleset (sip-manipulation) -> HeaderRule (header-rule) -> ElementRule.
// Enumerations use the exact Oracle keyword spelling (see
// docs/oracle-hmr-reference.md). The AST is plain value-semantic data; the
// Visitor lives in ast_visitor.hpp and the string<->enum mappings + builder in
// ast_factory.hpp.

#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "hmr/ast/value.hpp"
#include "hmr/diagnostics/diagnostics.hpp"

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
    std::string parameter_name;
    ElementType type = ElementType::None;
    ElementAction action = ElementAction::None;
    MatchValType match_val_type = MatchValType::Any;
    ComparisonType comparison = ComparisonType::CaseSensitive;
    Value match_value;
    Value new_value;
    SourceLocation loc;
};

struct HeaderRule {
    std::string name;
    std::string header_name;
    HeaderAction action = HeaderAction::None;
    ComparisonType comparison = ComparisonType::CaseSensitive;
    MsgType msg_type = MsgType::Any;
    std::vector<std::string> methods;  // empty == all methods
    Value match_value;
    Value new_value;
    std::vector<ElementRule> element_rules;
    SourceLocation loc;

    [[nodiscard]] bool has_method_filter() const { return !methods.empty(); }
};

struct Ruleset {
    std::string name;
    std::string description;
    std::vector<HeaderRule> header_rules;

    // Legacy / accepted-but-deprecated attributes.
    std::vector<std::string> split_headers;
    std::vector<std::string> join_headers;
    std::string import_file;
    std::string export_file;

    SourceLocation loc;
};

// True when the action removes the whole header (delete / delete-header).
[[nodiscard]] constexpr bool removes_header(HeaderAction a) {
    return a == HeaderAction::Delete || a == HeaderAction::DeleteHeader;
}

}  // namespace hmr::ast
