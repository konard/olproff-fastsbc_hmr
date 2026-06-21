// SPDX-License-Identifier: MIT
//
// Unit tests for the CRTP optimizer passes.

#include "test.hpp"
#include "hmr/optimizer/optimizer.hpp"
#include "hmr/parser/parser.hpp"

using namespace hmr::ast;
using namespace hmr::opt;
using namespace hmr::parser;

namespace {
std::size_t changes_for(const OptimizationReport& r, std::string_view pass) {
    for (const auto& p : r.passes)
        if (p.name == pass) return p.changes;
    return 0;
}
}  // namespace

TEST(Optimizer, DeduplicatesIdenticalRules) {
    auto r = Parser::parse_string(
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
    CHECK_EQ(rs.header_rules.size(), std::size_t{1});
    CHECK(changes_for(rep, "deduplication") >= 1);
}

TEST(Optimizer, KeepsDistinctAddRules) {
    auto r = Parser::parse_string(
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
    CHECK_EQ(rs.header_rules.size(), std::size_t{2});
}

TEST(Optimizer, EliminatesDeadRules) {
    auto r = Parser::parse_string(
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
    CHECK_EQ(rs.header_rules.size(), std::size_t{1});
    CHECK(changes_for(rep, "dead-code-elimination") >= 1);
}

TEST(Optimizer, SimplifiesLiteralRegex) {
    auto r = Parser::parse_string(
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
    REQUIRE(rs.header_rules.size() == 1);
    CHECK_EQ(rs.header_rules[0].comparison, ComparisonType::CaseSensitive);
    CHECK_EQ(rs.header_rules[0].match_value.literal_text(), std::string("INVITE"));
    CHECK(changes_for(rep, "pattern-simplification") >= 1);
}

TEST(Optimizer, BuildsDecisionPlan) {
    auto r = Parser::parse_string(
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

TEST_MAIN()
