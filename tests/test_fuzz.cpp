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

// test_fuzz.cpp — randomized robustness tests for the front-end (review #5:
// "Random HMR rulesets").
//
// Two complementary fuzzers, both driven by a fixed-seed PRNG so any failure is
// reproducible from the printed source:
//   * ValidRandomRulesetsParse — assembles syntactically- and semantically-valid
//     rulesets from the real Oracle vocabulary (random rule/element counts,
//     attribute subsets, value shapes and indentation). Every one must parse,
//     proving the keyword-delimited grammar accepts the whole language surface,
//     not just the hand-written sample corpus, and that indentation is cosmetic.
//   * GarbageNeverCrashes — feeds truncated, mutated and pure-noise input. The
//     contract under fuzzing is that parse() always *returns* (never crashes,
//     hangs or throws) and that a rejection carries at least one diagnostic —
//     the error-recovery guarantee from review #5 ("collect all errors").
//
// A third, deterministic case pins the "collect ALL errors" behaviour: a ruleset
// with several independent mistakes must surface several diagnostics at once.

#include <cstdint>
#include <cstdio>
#include <random>
#include <string>
#include <string_view>
#include <vector>

#include "test.hpp"
#include "hmr/parser/parser.hpp"

namespace {

using hmr::parser::Parser;

// A small, deterministic PRNG wrapper so a failing iteration always reproduces.
class Rng {
public:
    explicit Rng(std::uint32_t seed) : eng_(seed) {}
    std::uint32_t next(std::uint32_t n) {  // uniform in [0, n)
        return n == 0
                   ? 0u
                   : std::uniform_int_distribution<std::uint32_t>(0, n - 1)(eng_);
    }
    bool chance(int percent) { return static_cast<int>(next(100)) < percent; }
    template <class T>
    const T& pick(const std::vector<T>& v) {
        return v[next(static_cast<std::uint32_t>(v.size()))];
    }

private:
    std::mt19937 eng_;
};

// Valid Oracle vocabulary (mirrors ast_factory.cpp). Restricting the recognized
// enum keys to these spellings keeps a generated ruleset semantically valid.
const std::vector<std::string> kHeaderActions = {
    "none",          "add", "store",         "manipulate", "replace",
    "find-replace-all", "delete", "delete-element", "delete-header",
    "sip-manip",     "log", "reject"};
const std::vector<std::string> kElementActions = {
    "none",        "add",       "store",   "replace", "delete-element",
    "delete-header", "find-replace-all", "sip-manip", "log", "reject"};
const std::vector<std::string> kElementTypes = {
    "header-value", "header-param-name", "header-param", "uri-display",
    "uri-user",     "uri-user-param",    "uri-host",     "uri-port",
    "uri-param-name", "uri-param",       "uri-header-name", "uri-header",
    "status-code",  "reason-phrase",     "uri-user-only", "uri-phone-number-only"};
const std::vector<std::string> kComparisons = {
    "case-sensitive", "case-insensitive",     "pattern-rule",
    "boolean",        "refer-case-sensitive", "refer-case-insensitive"};
const std::vector<std::string> kMatchValTypes = {"any", "an", "ip", "fqdn"};
const std::vector<std::string> kMsgTypes = {"any", "request", "reply",
                                            "out-of-dialog"};
const std::vector<std::string> kHeaderNames = {
    "From",    "To",         "Via",   "Contact", "P-Asserted-Identity",
    "Diversion", "Server",   "User-Agent", "X-Hop", "Remote-Party-ID",
    "Route",   "Record-Route"};
const std::vector<std::string> kValues = {
    "host.example", "internal\\.local", "10.0.0.0/8",
    "192.168.1.1",  "172.16.0.1-172.16.255.254", "$LOCAL_IP",
    "$REMOTE_IP",   "sip:tg@$REALM",    "anonymous",
    "(\\d+)",       "\\1@example.com",  "", "true"};
// Mixed indentation — none, spaces, tabs — to prove blocks are keyword-delimited,
// not indentation-sensitive.
const std::vector<std::string> kIndents = {"", " ", "  ", "\t", "    ", "\t\t"};

// Append one "key value?" attribute line with random leading whitespace.
void emit_attr(std::string& s, Rng& rng, std::string_view key,
               std::string_view value) {
    s += rng.pick(kIndents);
    s += key;
    if (!value.empty()) {
        s += ' ';
        s += value;
    }
    s += '\n';
}

// A semantically-valid ruleset: a random shape over the real vocabulary. Only
// valid enum spellings are used for recognized enum keys, so it always parses;
// unknown keys (emitted occasionally) become warnings, not errors.
std::string gen_valid(Rng& rng) {
    std::string s =
        "sip-manipulation Fuzz" + std::to_string(rng.next(100000)) + "\n";
    if (rng.chance(50)) emit_attr(s, rng, "description", "\"random fuzz set\"");

    const int n_rules = 1 + static_cast<int>(rng.next(5));
    for (int r = 0; r < n_rules; ++r) {
        s += rng.pick(kIndents);
        s += "header-rule\n";
        if (rng.chance(90)) emit_attr(s, rng, "name", "hr" + std::to_string(r));
        if (rng.chance(80))
            emit_attr(s, rng, "header-name", rng.pick(kHeaderNames));
        if (rng.chance(80))
            emit_attr(s, rng, "action", rng.pick(kHeaderActions));
        if (rng.chance(50))
            emit_attr(s, rng, "comparison-type", rng.pick(kComparisons));
        if (rng.chance(40)) emit_attr(s, rng, "msg-type", rng.pick(kMsgTypes));
        if (rng.chance(30)) emit_attr(s, rng, "methods", "INVITE,BYE");
        if (rng.chance(40))
            emit_attr(s, rng, "match-value", rng.pick(kValues));
        if (rng.chance(40)) emit_attr(s, rng, "new-value", rng.pick(kValues));
        if (rng.chance(30))
            emit_attr(s, rng, "x-future-attr", "ignored");  // -> warning
        if (rng.chance(40)) s += "# a comment line\n";
        if (rng.chance(30)) s += "\n";  // blank line

        const int n_elems = static_cast<int>(rng.next(4));
        for (int e = 0; e < n_elems; ++e) {
            s += rng.pick(kIndents);
            s += "element-rule\n";
            if (rng.chance(90))
                emit_attr(s, rng, "name", "er" + std::to_string(e));
            if (rng.chance(80))
                emit_attr(s, rng, "type", rng.pick(kElementTypes));
            if (rng.chance(80))
                emit_attr(s, rng, "action", rng.pick(kElementActions));
            if (rng.chance(40))
                emit_attr(s, rng, "match-val-type", rng.pick(kMatchValTypes));
            if (rng.chance(40))
                emit_attr(s, rng, "comparison-type", rng.pick(kComparisons));
            if (rng.chance(50))
                emit_attr(s, rng, "match-value", rng.pick(kValues));
            if (rng.chance(50))
                emit_attr(s, rng, "new-value", rng.pick(kValues));
        }
    }
    return s;
}

// A run of "interesting" bytes — the metacharacters and structural punctuation
// the lexer cares about, so noise lands on real lexer/parser edges.
std::string random_noise(Rng& rng, std::size_t max_len) {
    static const std::string alphabet =
        " \t\r\n#\"\\$:/@-.0123456789abcXYZ{}[]()|*+?";
    const std::size_t len = rng.next(static_cast<std::uint32_t>(max_len));
    std::string s;
    s.reserve(len);
    for (std::size_t i = 0; i < len; ++i)
        s.push_back(
            alphabet[rng.next(static_cast<std::uint32_t>(alphabet.size()))]);
    return s;
}

// Malformed input across several flavours: pure noise, a truncated valid set,
// valid skeleton with invalid enum values, keyword soup, and stray quotes.
std::string gen_garbage(Rng& rng) {
    switch (rng.next(5)) {
        case 0:  // pure noise
            return random_noise(rng, 200);
        case 1: {  // a valid ruleset cut at a random offset
            const std::string v = gen_valid(rng);
            return v.substr(
                0, rng.next(static_cast<std::uint32_t>(v.size() + 1)));
        }
        case 2: {  // valid skeleton, garbage enum values
            std::string s = "sip-manipulation Bad\n";
            s += "header-rule\n";
            s += "action " + random_noise(rng, 12) + "\n";
            s += "comparison-type " + random_noise(rng, 12) + "\n";
            return s;
        }
        case 3: {  // keyword soup, no structure
            static const std::vector<std::string> kw = {
                "sip-manipulation", "header-rule", "element-rule",
                "name",             "action",      "match-value",
                "new-value",        "type",        "comparison-type",
                "mime-rule"};
            std::string s;
            const int lines = 1 + static_cast<int>(rng.next(20));
            for (int i = 0; i < lines; ++i) {
                s += rng.pick(kw);
                s += '\n';
            }
            return s;
        }
        default: {  // unterminated string + trailing noise
            std::string s =
                "sip-manipulation X\nheader-rule\nname \"unterminated\n";
            s += random_noise(rng, 40);
            return s;
        }
    }
}

}  // namespace

// Every well-formed random ruleset must parse — the grammar accepts the full
// language surface, with indentation insignificant.
TEST(Fuzz, ValidRandomRulesetsParse) {
    Rng rng(0xC0FFEEu);
    constexpr int kIters = 2000;
    int ok = 0;
    for (int i = 0; i < kIters; ++i) {
        const std::string src = gen_valid(rng);
        auto rs = Parser::parse_string(src);
        if (rs) {
            ++ok;
        } else {
            std::fprintf(stderr,
                         "    valid ruleset #%d failed to parse:\n%s\n    -> %s\n",
                         i, src.c_str(), rs.error().format().c_str());
            CHECK(rs.has_value());
            break;
        }
    }
    CHECK_EQ(ok, kIters);
}

// Malformed input must never crash, hang or throw — parse() always returns, and
// a rejection always carries at least one diagnostic.
TEST(Fuzz, GarbageNeverCrashes) {
    Rng rng(0xBADF00Du);
    constexpr int kIters = 4000;
    int rejected = 0;
    int accepted = 0;
    for (int i = 0; i < kIters; ++i) {
        const std::string src = gen_garbage(rng);
        auto rs = Parser::parse_string(src);  // contract: simply RETURNS
        if (rs) {
            ++accepted;
        } else {
            ++rejected;
            CHECK(!rs.error().diagnostics().empty());
        }
    }
    CHECK_EQ(rejected + accepted, kIters);  // reached the end => no crash
    CHECK(rejected > 0);  // sanity: the noise really does exercise rejection
}

// Error recovery collects ALL errors, not just the first (review #5). Three
// header-rules each carry a distinct unknown enum value; all three are reported.
TEST(Fuzz, AllErrorsCollectedNotJustFirst) {
    const char* src =
        "sip-manipulation MultiError\n"
        "        header-rule\n"
        "                name a\n"
        "                action frobnicate\n"        // unknown header action
        "        header-rule\n"
        "                name b\n"
        "                comparison-type sideways\n"  // unknown comparison-type
        "        header-rule\n"
        "                name c\n"
        "                msg-type telepathy\n";       // unknown msg-type
    auto rs = Parser::parse_string(src);
    REQUIRE(!rs.has_value());
    CHECK(rs.error().diagnostics().size() >= 3);
}

TEST_MAIN()
