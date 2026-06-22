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

// Unit tests for the ANTLR-backed parser (front-end visitor).

#include "test.hpp"
#include "hmr/ast/ast_visitor.hpp"
#include "hmr/parser/parser.hpp"

using namespace hmr::ast;
using namespace hmr::parser;

namespace {
constexpr const char* kSample =
    "sip-manipulation\n"
    "        name  TopoHiding\n"
    "        description  \"hide internal topology\"\n"
    "        header-rule\n"
    "                name  fixFrom\n"
    "                header-name  From\n"
    "                action  manipulate\n"
    "                comparison-type  case-sensitive\n"
    "                msg-type  request\n"
    "                methods  INVITE,REGISTER\n"
    "                element-rule\n"
    "                        name  user\n"
    "                        type  uri-user\n"
    "                        action  replace\n"
    "                        match-value  .*\n"
    "                        new-value  anonymous\n"
    "        header-rule\n"
    "                name  dropServer\n"
    "                header-name  Server\n"
    "                action  delete-header\n";
}  // namespace

TEST(Parser, ParsesRulesetStructure) {
    auto r = Parser::parse_string(kSample);
    REQUIRE(r.has_value());
    const Ruleset& rs = *r;
    CHECK_EQ(rs.name, std::string("TopoHiding"));
    CHECK_EQ(rs.description, std::string("hide internal topology"));
    REQUIRE(rs.header_rules.size() == 2);

    const HeaderRule& hr = rs.header_rules[0];
    CHECK_EQ(hr.name, std::string("fixFrom"));
    CHECK_EQ(hr.header_name, std::string("From"));
    CHECK_EQ(hr.action, HeaderAction::Manipulate);
    CHECK_EQ(hr.comparison, ComparisonType::CaseSensitive);
    CHECK_EQ(hr.msg_type, MsgType::Request);
    REQUIRE(hr.methods.size() == 2);
    CHECK_EQ(hr.methods[0], std::string("INVITE"));
    CHECK_EQ(hr.methods[1], std::string("REGISTER"));

    REQUIRE(hr.element_rules.size() == 1);
    const ElementRule& er = hr.element_rules[0];
    CHECK_EQ(er.type, ElementType::UriUser);
    CHECK_EQ(er.action, ElementAction::Replace);
    CHECK_EQ(er.new_value.literal_text(), std::string("anonymous"));

    const HeaderRule& hr2 = rs.header_rules[1];
    CHECK_EQ(hr2.header_name, std::string("Server"));
    CHECK_EQ(hr2.action, HeaderAction::DeleteHeader);
}

TEST(Parser, MissingSipManipulationIsError) {
    auto r = Parser::parse_string("header-rule\n        name  x\n");
    CHECK(!r.has_value());
}

TEST(Parser, UnknownEnumValueIsError) {
    auto r = Parser::parse_string(
        "sip-manipulation\n"
        "        header-rule\n"
        "                action  bogus-action\n");
    CHECK(!r.has_value());
}

TEST(Parser, UnknownAttributeIsWarningNotError) {
    auto r = Parser::parse_string(
        "sip-manipulation\n"
        "        future-attr  somevalue\n"
        "        header-rule\n"
        "                header-name  X\n"
        "                action  delete-header\n");
    REQUIRE(r.has_value());
    CHECK(r->header_rules.size() == 1);
}

TEST(Parser, RoundTripThroughVisitor) {
    auto r = Parser::parse_string(kSample);
    REQUIRE(r.has_value());
    // The pretty-printed text should itself re-parse to an equivalent ruleset.
    std::string text = to_hmr_text(*r);
    auto r2 = Parser::parse_string(text);
    REQUIRE(r2.has_value());
    CHECK_EQ(r2->name, r->name);
    CHECK_EQ(r2->header_rules.size(), r->header_rules.size());
    REQUIRE(r2->header_rules.size() == 2);
    CHECK_EQ(r2->header_rules[0].action, HeaderAction::Manipulate);
    CHECK_EQ(r2->header_rules[0].element_rules.size(), std::size_t{1});
}

// The Oracle 10.1.0 ACLI adds two element `type` values (uri-user-only,
// uri-phone-number-only). The grammar must recognize them (they parse to the
// matching enum) even though the back-ends do not yet lower their manipulation.
TEST(Parser, Recognizes_10_1_0_ElementTypes) {
    auto r = Parser::parse_string(
        "sip-manipulation\n"
        "        header-rule\n"
        "                header-name  From\n"
        "                action  manipulate\n"
        "                element-rule\n"
        "                        name  u\n"
        "                        type  uri-user-only\n"
        "                        action  store\n"
        "                element-rule\n"
        "                        name  p\n"
        "                        type  uri-phone-number-only\n"
        "                        action  store\n");
    REQUIRE(r.has_value());
    REQUIRE(r->header_rules.size() == 1);
    const HeaderRule& hr = r->header_rules[0];
    REQUIRE(hr.element_rules.size() == 2);
    CHECK_EQ(hr.element_rules[0].type, ElementType::UriUserOnly);
    CHECK_EQ(hr.element_rules[1].type, ElementType::UriPhoneNumberOnly);
}

TEST_MAIN()
