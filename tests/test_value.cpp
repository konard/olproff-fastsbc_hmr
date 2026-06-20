// SPDX-License-Identifier: MIT
//
// Unit tests for the HMR value expression parser.

#include "Test.hpp"
#include "hmr/ast/Value.hpp"

using namespace hmr::ast;

TEST(Value, PureLiteral) {
    Value v = Value::parse("anonymous", ValueMode::NewValue);
    CHECK(v.isPureLiteral());
    CHECK_EQ(v.literalText(), std::string("anonymous"));
    CHECK(!v.isSingleRef());
}

TEST(Value, InterpolatedNewValue) {
    Value v = Value::parse("sip:$RURI_USER@$LOCAL_IP", ValueMode::NewValue);
    CHECK(!v.isPureLiteral());
    REQUIRE(v.segments().size() == 4);
    CHECK(!v.segments()[0].isRef);
    CHECK_EQ(v.segments()[0].literal, std::string("sip:"));
    CHECK(v.segments()[1].isRef);
    CHECK_EQ(v.segments()[1].ref.kind, RefKind::Variable);
    CHECK_EQ(v.segments()[1].ref.name, std::string("RURI_USER"));
    CHECK(!v.segments()[2].isRef);
    CHECK_EQ(v.segments()[2].literal, std::string("@"));
    CHECK(v.segments()[3].isRef);
    CHECK_EQ(v.segments()[3].ref.name, std::string("LOCAL_IP"));
}

TEST(Value, ConcatenationOperator) {
    Value v = Value::parse("$rule.$1+edited", ValueMode::NewValue);
    REQUIRE(v.segments().size() == 2);
    CHECK(v.segments()[0].isRef);
    CHECK_EQ(v.segments()[0].ref.kind, RefKind::RuleRef);
    CHECK_EQ(v.segments()[0].ref.captureIndex, 1);
    CHECK(!v.segments()[1].isRef);
    CHECK_EQ(v.segments()[1].literal, std::string("edited"));
}

TEST(Value, MatchValueCaptureRef) {
    Value v = Value::parse("$1", ValueMode::MatchValue);
    CHECK(v.isSingleRef());
    REQUIRE(v.segments().size() == 1);
    CHECK_EQ(v.segments()[0].ref.kind, RefKind::Capture);
    CHECK_EQ(v.segments()[0].ref.captureIndex, 1);
    CHECK(!v.isRegexLiteral());
}

TEST(Value, MatchValueNegation) {
    Value v = Value::parse("!$whitelist.$check", ValueMode::MatchValue);
    CHECK(v.negated());
    REQUIRE(v.segments().size() == 1);
    CHECK_EQ(v.segments()[0].ref.kind, RefKind::RuleRef);
}

TEST(Value, RegexLiteralMatchValue) {
    Value v = Value::parse("^sip:.*@example\\.com$", ValueMode::MatchValue);
    CHECK(v.isRegexLiteral());
    CHECK(v.isPureLiteral());
    CHECK(!v.negated());
}

TEST(Value, BracedVariable) {
    Value v = Value::parse("${TRUNK_GROUP}-suffix", ValueMode::NewValue);
    REQUIRE(v.segments().size() == 2);
    CHECK(v.segments()[0].isRef);
    CHECK_EQ(v.segments()[0].ref.kind, RefKind::Variable);
    CHECK_EQ(v.segments()[0].ref.name, std::string("TRUNK_GROUP"));
    CHECK_EQ(v.segments()[1].literal, std::string("-suffix"));
}

TEST(Value, QuotedIsVerbatim) {
    Value v = Value::parse("$keep+literal", ValueMode::NewValue, /*quoted=*/true);
    CHECK(v.isPureLiteral());
    CHECK_EQ(v.literalText(), std::string("$keep+literal"));
}
