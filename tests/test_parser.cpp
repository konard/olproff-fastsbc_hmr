// SPDX-License-Identifier: MIT
//
// Unit tests for the recursive-descent parser.

#include "Test.hpp"
#include "hmr/ast/AstVisitor.hpp"
#include "hmr/parser/Parser.hpp"

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
    auto r = Parser::parseString(kSample);
    REQUIRE(r.has_value());
    const Ruleset& rs = *r;
    CHECK_EQ(rs.name, std::string("TopoHiding"));
    CHECK_EQ(rs.description, std::string("hide internal topology"));
    REQUIRE(rs.headerRules.size() == 2);

    const HeaderRule& hr = rs.headerRules[0];
    CHECK_EQ(hr.name, std::string("fixFrom"));
    CHECK_EQ(hr.headerName, std::string("From"));
    CHECK_EQ(hr.action, HeaderAction::Manipulate);
    CHECK_EQ(hr.comparison, ComparisonType::CaseSensitive);
    CHECK_EQ(hr.msgType, MsgType::Request);
    REQUIRE(hr.methods.size() == 2);
    CHECK_EQ(hr.methods[0], std::string("INVITE"));
    CHECK_EQ(hr.methods[1], std::string("REGISTER"));

    REQUIRE(hr.elementRules.size() == 1);
    const ElementRule& er = hr.elementRules[0];
    CHECK_EQ(er.type, ElementType::UriUser);
    CHECK_EQ(er.action, ElementAction::Replace);
    CHECK_EQ(er.newValue.literalText(), std::string("anonymous"));

    const HeaderRule& hr2 = rs.headerRules[1];
    CHECK_EQ(hr2.headerName, std::string("Server"));
    CHECK_EQ(hr2.action, HeaderAction::DeleteHeader);
}

TEST(Parser, MissingSipManipulationIsError) {
    auto r = Parser::parseString("header-rule\n        name  x\n");
    CHECK(!r.has_value());
}

TEST(Parser, UnknownEnumValueIsError) {
    auto r = Parser::parseString(
        "sip-manipulation\n"
        "        header-rule\n"
        "                action  bogus-action\n");
    CHECK(!r.has_value());
}

TEST(Parser, UnknownAttributeIsWarningNotError) {
    auto r = Parser::parseString(
        "sip-manipulation\n"
        "        future-attr  somevalue\n"
        "        header-rule\n"
        "                header-name  X\n"
        "                action  delete-header\n");
    REQUIRE(r.has_value());
    CHECK(r->headerRules.size() == 1);
}

TEST(Parser, RoundTripThroughVisitor) {
    auto r = Parser::parseString(kSample);
    REQUIRE(r.has_value());
    // The pretty-printed text should itself re-parse to an equivalent ruleset.
    std::string text = toHmrText(*r);
    auto r2 = Parser::parseString(text);
    REQUIRE(r2.has_value());
    CHECK_EQ(r2->name, r->name);
    CHECK_EQ(r2->headerRules.size(), r->headerRules.size());
    REQUIRE(r2->headerRules.size() == 2);
    CHECK_EQ(r2->headerRules[0].action, HeaderAction::Manipulate);
    CHECK_EQ(r2->headerRules[0].elementRules.size(), std::size_t{1});
}
