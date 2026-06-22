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

// ast_visitor.hpp — GoF Visitor over the HMR AST.
//
// A non-intrusive visitor: the AST nodes stay plain data and `accept` performs
// the traversal, invoking visitor hooks. Used by the AST pretty-printer (round
// trip / `hmrc dump-ast`) and available to any consumer that wants to walk a
// ruleset without depending on the code generator.

#pragma once

#include <string>

#include "hmr/ast/ast.hpp"

namespace hmr::ast {

class IAstVisitor {
public:
    virtual ~IAstVisitor() = default;

    virtual void enter_ruleset(const Ruleset&) {}
    virtual void leave_ruleset(const Ruleset&) {}
    virtual void enter_header_rule(const HeaderRule&) {}
    virtual void leave_header_rule(const HeaderRule&) {}
    virtual void visit_element_rule(const ElementRule&) {}
};

inline void accept(const Ruleset& rs, IAstVisitor& v) {
    v.enter_ruleset(rs);
    for (const HeaderRule& hr : rs.header_rules) {
        v.enter_header_rule(hr);
        for (const ElementRule& er : hr.element_rules) v.visit_element_rule(er);
        v.leave_header_rule(hr);
    }
    v.leave_ruleset(rs);
}

// Renders a Ruleset back into canonical HMR text (used for snapshot tests and
// `hmrc dump-ast`). Demonstrates the Visitor pattern end to end.
std::string to_hmr_text(const Ruleset& rs);

}  // namespace hmr::ast
