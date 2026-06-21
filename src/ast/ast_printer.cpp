// SPDX-License-Identifier: MIT

#include <format>
#include <string>

#include "hmr/ast/ast_factory.hpp"
#include "hmr/ast/ast_visitor.hpp"

namespace hmr::ast {

namespace {

class HmrTextPrinter final : public IAstVisitor {
public:
    std::string out;

    void enter_ruleset(const Ruleset& rs) override {
        out += "sip-manipulation\n";
        attr(1, "name", rs.name);
        if (!rs.description.empty()) attr(1, "description", rs.description);
    }

    void enter_header_rule(const HeaderRule& hr) override {
        out += "        header-rule\n";
        attr(2, "name", hr.name);
        attr(2, "header-name", hr.header_name);
        attr(2, "action", std::string(AstFactory::to_string(hr.action)));
        attr(2, "comparison-type",
             std::string(AstFactory::to_string(hr.comparison)));
        attr(2, "match-value", hr.match_value.raw());
        attr(2, "msg-type", std::string(AstFactory::to_string(hr.msg_type)));
        attr(2, "new-value", hr.new_value.raw());
        attr(2, "methods", join_methods(hr.methods));
    }

    void visit_element_rule(const ElementRule& er) override {
        out += "                element-rule\n";
        attr(3, "name", er.name);
        attr(3, "parameter-name", er.parameter_name);
        attr(3, "type", std::string(AstFactory::to_string(er.type)));
        attr(3, "action", std::string(AstFactory::to_string(er.action)));
        attr(3, "match-val-type",
             std::string(AstFactory::to_string(er.match_val_type)));
        attr(3, "comparison-type",
             std::string(AstFactory::to_string(er.comparison)));
        attr(3, "match-value", er.match_value.raw());
        attr(3, "new-value", er.new_value.raw());
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

    static std::string join_methods(const std::vector<std::string>& m) {
        std::string s;
        for (std::size_t i = 0; i < m.size(); ++i) {
            if (i) s.push_back(',');
            s += m[i];
        }
        return s;
    }
};

}  // namespace

std::string to_hmr_text(const Ruleset& rs) {
    HmrTextPrinter printer;
    accept(rs, printer);
    return printer.out;
}

}  // namespace hmr::ast
