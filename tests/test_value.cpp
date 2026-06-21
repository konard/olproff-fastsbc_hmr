// SPDX-License-Identifier: MIT
//
// Unit tests for the HMR value expression parser.

#include "test.hpp"
#include "hmr/ast/value.hpp"

using namespace hmr::ast;

TEST(Value, PureLiteral) {
    Value v = Value::parse("anonymous", ValueMode::NewValue);
    CHECK(v.is_pure_literal());
    CHECK_EQ(v.literal_text(), std::string("anonymous"));
    CHECK(!v.is_single_ref());
}

TEST(Value, InterpolatedNewValue) {
    Value v = Value::parse("sip:$RURI_USER@$LOCAL_IP", ValueMode::NewValue);
    CHECK(!v.is_pure_literal());
    REQUIRE(v.segments().size() == 4);
    CHECK(!v.segments()[0].is_ref);
    CHECK_EQ(v.segments()[0].literal, std::string("sip:"));
    CHECK(v.segments()[1].is_ref);
    CHECK_EQ(v.segments()[1].ref.kind, RefKind::Variable);
    CHECK_EQ(v.segments()[1].ref.name, std::string("RURI_USER"));
    CHECK(!v.segments()[2].is_ref);
    CHECK_EQ(v.segments()[2].literal, std::string("@"));
    CHECK(v.segments()[3].is_ref);
    CHECK_EQ(v.segments()[3].ref.name, std::string("LOCAL_IP"));
}

TEST(Value, ConcatenationOperator) {
    Value v = Value::parse("$rule.$1+edited", ValueMode::NewValue);
    REQUIRE(v.segments().size() == 2);
    CHECK(v.segments()[0].is_ref);
    CHECK_EQ(v.segments()[0].ref.kind, RefKind::RuleRef);
    CHECK_EQ(v.segments()[0].ref.capture_index, 1);
    CHECK(!v.segments()[1].is_ref);
    CHECK_EQ(v.segments()[1].literal, std::string("edited"));
}

TEST(Value, MatchValueCaptureRef) {
    Value v = Value::parse("$1", ValueMode::MatchValue);
    CHECK(v.is_single_ref());
    REQUIRE(v.segments().size() == 1);
    CHECK_EQ(v.segments()[0].ref.kind, RefKind::Capture);
    CHECK_EQ(v.segments()[0].ref.capture_index, 1);
    CHECK(!v.is_regex_literal());
}

TEST(Value, MatchValueNegation) {
    Value v = Value::parse("!$whitelist.$check", ValueMode::MatchValue);
    CHECK(v.negated());
    REQUIRE(v.segments().size() == 1);
    CHECK_EQ(v.segments()[0].ref.kind, RefKind::RuleRef);
}

TEST(Value, RegexLiteralMatchValue) {
    Value v = Value::parse("^sip:.*@example\\.com$", ValueMode::MatchValue);
    CHECK(v.is_regex_literal());
    CHECK(v.is_pure_literal());
    CHECK(!v.negated());
}

TEST(Value, BracedVariable) {
    Value v = Value::parse("${TRUNK_GROUP}-suffix", ValueMode::NewValue);
    REQUIRE(v.segments().size() == 2);
    CHECK(v.segments()[0].is_ref);
    CHECK_EQ(v.segments()[0].ref.kind, RefKind::Variable);
    CHECK_EQ(v.segments()[0].ref.name, std::string("TRUNK_GROUP"));
    CHECK_EQ(v.segments()[1].literal, std::string("-suffix"));
}

TEST(Value, QuotedIsVerbatim) {
    Value v = Value::parse("$keep+literal", ValueMode::NewValue, /*quoted=*/true);
    CHECK(v.is_pure_literal());
    CHECK_EQ(v.literal_text(), std::string("$keep+literal"));
}

TEST_MAIN()
