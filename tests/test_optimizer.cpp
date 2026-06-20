// SPDX-License-Identifier: MIT
//
// Unit tests for the CRTP optimizer passes.

#include "Test.hpp"
#include "hmr/optimizer/Optimizer.hpp"
#include "hmr/parser/Parser.hpp"

using namespace hmr::ast;
using namespace hmr::opt;
using namespace hmr::parser;

namespace {
std::size_t changesFor(const OptimizationReport& r, std::string_view pass) {
    for (const auto& p : r.passes)
        if (p.name == pass) return p.changes;
    return 0;
}
}  // namespace

TEST(Optimizer, DeduplicatesIdenticalRules) {
    auto r = Parser::parseString(
        "sip-manipulation\n"
        "        header-rule\n"
        "                header-name  Server\n"
        "                action  delete-header\n"
        "        header-rule\n"
        "                header-name  Server\n"
        "                action  delete-header\n");
    REQUIRE(r.has_value());
    Ruleset rs = *r;
    Optimizer opt;
    OptimizationReport rep = opt.optimize(rs);
    CHECK_EQ(rs.headerRules.size(), std::size_t{1});
    CHECK(changesFor(rep, "deduplication") >= 1);
}

TEST(Optimizer, KeepsDistinctAddRules) {
    auto r = Parser::parseString(
        "sip-manipulation\n"
        "        header-rule\n"
        "                header-name  X-Tag\n"
        "                action  add\n"
        "                new-value  a\n"
        "        header-rule\n"
        "                header-name  X-Tag\n"
        "                action  add\n"
        "                new-value  a\n");
    REQUIRE(r.has_value());
    Ruleset rs = *r;
    Optimizer opt;
    opt.optimize(rs);
    // Two identical `add` rules legitimately add two headers; do NOT dedup.
    CHECK_EQ(rs.headerRules.size(), std::size_t{2});
}

TEST(Optimizer, EliminatesDeadRules) {
    auto r = Parser::parseString(
        "sip-manipulation\n"
        "        header-rule\n"
        "                header-name  X\n"
        "                action  none\n"
        "        header-rule\n"
        "                header-name  Y\n"
        "                action  delete-header\n");
    REQUIRE(r.has_value());
    Ruleset rs = *r;
    Optimizer opt;
    OptimizationReport rep = opt.optimize(rs);
    CHECK_EQ(rs.headerRules.size(), std::size_t{1});
    CHECK(changesFor(rep, "dead-code-elimination") >= 1);
}

TEST(Optimizer, SimplifiesLiteralRegex) {
    auto r = Parser::parseString(
        "sip-manipulation\n"
        "        header-rule\n"
        "                header-name  From\n"
        "                action  store\n"
        "                comparison-type  pattern-rule\n"
        "                match-value  ^INVITE$\n");
    REQUIRE(r.has_value());
    Ruleset rs = *r;
    Optimizer opt;
    OptimizationReport rep = opt.optimize(rs);
    REQUIRE(rs.headerRules.size() == 1);
    CHECK_EQ(rs.headerRules[0].comparison, ComparisonType::CaseSensitive);
    CHECK_EQ(rs.headerRules[0].matchValue.literalText(), std::string("INVITE"));
    CHECK(changesFor(rep, "pattern-simplification") >= 1);
}

TEST(Optimizer, BuildsDecisionPlan) {
    auto r = Parser::parseString(
        "sip-manipulation\n"
        "        header-rule\n"
        "                header-name  From\n"
        "                action  store\n"
        "        header-rule\n"
        "                header-name  from\n"  // same header, different case
        "                action  manipulate\n"
        "        header-rule\n"
        "                header-name  To\n"
        "                action  store\n");
    REQUIRE(r.has_value());
    Ruleset rs = *r;
    Optimizer opt;
    OptimizationReport rep = opt.optimize(rs);
    // From/from collapse to one group; To is another => 2 groups.
    CHECK_EQ(rep.plan.size(), std::size_t{2});
}
