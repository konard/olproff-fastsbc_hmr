// SPDX-License-Identifier: MIT
//
// AstFactory.hpp — Factory for AST enums and nodes.
//
// Centralizes the (case-insensitive) mapping between Oracle keyword spellings
// and the strongly-typed AST enums, plus to_string for IR dumps and
// diagnostics. The recursive-descent parser delegates all keyword recognition
// here so the vocabulary lives in exactly one place.

#pragma once

#include <optional>
#include <string>
#include <string_view>

#include "hmr/ast/Ast.hpp"

namespace hmr::ast {

class AstFactory {
public:
    // --- keyword -> enum (case-insensitive) --------------------------------
    static std::optional<HeaderAction> headerAction(std::string_view s);
    static std::optional<ElementAction> elementAction(std::string_view s);
    static std::optional<ElementType> elementType(std::string_view s);
    static std::optional<ComparisonType> comparison(std::string_view s);
    static std::optional<MatchValType> matchValType(std::string_view s);
    static std::optional<MsgType> msgType(std::string_view s);

    // --- enum -> canonical keyword -----------------------------------------
    static std::string_view toString(HeaderAction a);
    static std::string_view toString(ElementAction a);
    static std::string_view toString(ElementType t);
    static std::string_view toString(ComparisonType c);
    static std::string_view toString(MatchValType m);
    static std::string_view toString(MsgType m);

    // True if `name` is a block keyword (sip-manipulation/header-rule/...).
    static bool isBlockKeyword(std::string_view s);

    // Split a comma-separated methods list into upper-cased method tokens.
    static std::vector<std::string> parseMethods(std::string_view s);

    // Lowercase copy used for case-insensitive matching.
    static std::string lower(std::string_view s);
};

}  // namespace hmr::ast
