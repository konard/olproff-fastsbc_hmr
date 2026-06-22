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
