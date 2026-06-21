// SPDX-License-Identifier: MIT
//
// ast_factory.hpp — Factory for AST enums and nodes.
//
// Centralizes the (case-insensitive) mapping between Oracle keyword spellings
// and the strongly-typed AST enums, plus to_string for IR dumps and
// diagnostics. The parser delegates all keyword recognition here so the
// vocabulary lives in exactly one place.

#pragma once

#include <optional>
#include <string>
#include <string_view>

#include "hmr/ast/ast.hpp"

namespace hmr::ast {

class AstFactory {
public:
    // --- keyword -> enum (case-insensitive) --------------------------------
    static std::optional<HeaderAction> header_action(std::string_view s);
    static std::optional<ElementAction> element_action(std::string_view s);
    static std::optional<ElementType> element_type(std::string_view s);
    static std::optional<ComparisonType> comparison(std::string_view s);
    static std::optional<MatchValType> match_val_type(std::string_view s);
    static std::optional<MsgType> msg_type(std::string_view s);

    // --- enum -> canonical keyword -----------------------------------------
    static std::string_view to_string(HeaderAction a);
    static std::string_view to_string(ElementAction a);
    static std::string_view to_string(ElementType t);
    static std::string_view to_string(ComparisonType c);
    static std::string_view to_string(MatchValType m);
    static std::string_view to_string(MsgType m);

    // True if `name` is a block keyword (sip-manipulation/header-rule/...).
    static bool is_block_keyword(std::string_view s);

    // Split a comma-separated methods list into upper-cased method tokens.
    static std::vector<std::string> parse_methods(std::string_view s);

    // Lowercase copy used for case-insensitive matching.
    static std::string lower(std::string_view s);
};

}  // namespace hmr::ast
