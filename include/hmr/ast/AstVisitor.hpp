// SPDX-License-Identifier: MIT
//
// AstVisitor.hpp — GoF Visitor over the HMR AST.
//
// A non-intrusive visitor: the AST nodes stay plain data and `accept` performs
// the traversal, invoking visitor hooks. Used by the AST pretty-printer (round
// trip / `hmrc dump-ast`) and available to any consumer that wants to walk a
// ruleset without depending on the code generator.

#pragma once

#include <string>

#include "hmr/ast/Ast.hpp"

namespace hmr::ast {

class IAstVisitor {
public:
    virtual ~IAstVisitor() = default;

    virtual void enterRuleset(const Ruleset&) {}
    virtual void leaveRuleset(const Ruleset&) {}
    virtual void enterHeaderRule(const HeaderRule&) {}
    virtual void leaveHeaderRule(const HeaderRule&) {}
    virtual void visitElementRule(const ElementRule&) {}
};

inline void accept(const Ruleset& rs, IAstVisitor& v) {
    v.enterRuleset(rs);
    for (const HeaderRule& hr : rs.headerRules) {
        v.enterHeaderRule(hr);
        for (const ElementRule& er : hr.elementRules) v.visitElementRule(er);
        v.leaveHeaderRule(hr);
    }
    v.leaveRuleset(rs);
}

// Renders a Ruleset back into canonical HMR text (used for snapshot tests and
// `hmrc dump-ast`). Demonstrates the Visitor pattern end to end.
std::string toHmrText(const Ruleset& rs);

}  // namespace hmr::ast
