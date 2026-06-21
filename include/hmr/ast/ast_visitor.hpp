// SPDX-License-Identifier: MIT
//
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
