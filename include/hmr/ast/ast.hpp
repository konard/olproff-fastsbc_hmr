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
    // Oracle 10.1.0 additions: the user part stripped of user-parameters, and the
    // bare phone number. Recognized by the grammar; manipulation is not yet
    // lowered (they join the skipped element kinds — see
    // docs/compatibility-matrix.md).
    UriUserOnly,
    UriPhoneNumberOnly,
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
