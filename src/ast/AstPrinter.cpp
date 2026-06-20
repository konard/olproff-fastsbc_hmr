// SPDX-License-Identifier: MIT

#include <format>
#include <string>

#include "hmr/ast/AstFactory.hpp"
#include "hmr/ast/AstVisitor.hpp"

namespace hmr::ast {

namespace {

class HmrTextPrinter final : public IAstVisitor {
public:
    std::string out;

    void enterRuleset(const Ruleset& rs) override {
        out += "sip-manipulation\n";
        attr(1, "name", rs.name);
        if (!rs.description.empty()) attr(1, "description", rs.description);
    }

    void enterHeaderRule(const HeaderRule& hr) override {
        out += "        header-rule\n";
        attr(2, "name", hr.name);
        attr(2, "header-name", hr.headerName);
        attr(2, "action", std::string(AstFactory::toString(hr.action)));
        attr(2, "comparison-type",
             std::string(AstFactory::toString(hr.comparison)));
        attr(2, "match-value", hr.matchValue.raw());
        attr(2, "msg-type", std::string(AstFactory::toString(hr.msgType)));
        attr(2, "new-value", hr.newValue.raw());
        attr(2, "methods", joinMethods(hr.methods));
    }

    void visitElementRule(const ElementRule& er) override {
        out += "                element-rule\n";
        attr(3, "name", er.name);
        attr(3, "parameter-name", er.parameterName);
        attr(3, "type", std::string(AstFactory::toString(er.type)));
        attr(3, "action", std::string(AstFactory::toString(er.action)));
        attr(3, "match-val-type",
             std::string(AstFactory::toString(er.matchValType)));
        attr(3, "comparison-type",
             std::string(AstFactory::toString(er.comparison)));
        attr(3, "match-value", er.matchValue.raw());
        attr(3, "new-value", er.newValue.raw());
    }

private:
    void attr(int depth, std::string_view key, const std::string& value) {
        out.append(static_cast<std::size_t>(depth) * 8, ' ');
        out += key;
        if (!value.empty()) {
            out += "                    ";
            out += value;
        }
        out.push_back('\n');
    }

    static std::string joinMethods(const std::vector<std::string>& m) {
        std::string s;
        for (std::size_t i = 0; i < m.size(); ++i) {
            if (i) s.push_back(',');
            s += m[i];
        }
        return s;
    }
};

}  // namespace

std::string toHmrText(const Ruleset& rs) {
    HmrTextPrinter printer;
    accept(rs, printer);
    return printer.out;
}

}  // namespace hmr::ast
