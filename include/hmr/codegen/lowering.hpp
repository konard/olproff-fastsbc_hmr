// SPDX-License-Identifier: MIT
//
// lowering.hpp — pure AST → runtime-immediate mappings shared by the two
// back-ends that lower an HMR ruleset onto the hmr_rt_* ABI:
//
//   * src/codegen/ir_generator.cpp  — emits LLVM IR (the compiled module),
//   * src/interp/interpreter.cpp    — walks the AST directly (the oracle).
//
// Both must pick the *same* variable id, URI selector, and match-type code for a
// given AST node, otherwise the compiled .so and the interpreter would disagree
// and the differential fuzzer would (rightly) flag it. Keeping these decisions
// in one header makes that agreement structural rather than a coincidence two
// copies happen to maintain. The header is deliberately free of any LLVM
// dependency so the interpreter (which has none) can include it.

#pragma once

#include <cctype>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>

#include "hmr/ast/ast.hpp"
#include "hmr/runtime/hmr_runtime.h"

namespace hmr::codegen {

// Map a built-in variable name ($LOCAL_IP, ...) to its stable HmrVarId. Unknown
// names resolve to HMR_VAR_NONE, which the runtime reports as the empty string.
inline HmrVarId variable_id(std::string_view name) {
    std::string up;
    up.reserve(name.size());
    for (char c : name)
        up.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(c))));
    static const std::map<std::string, HmrVarId> kMap = {
        {"LOCAL_IP", HMR_VAR_LOCAL_IP},       {"REMOTE_IP", HMR_VAR_REMOTE_IP},
        {"LOCAL_PORT", HMR_VAR_LOCAL_PORT},   {"REMOTE_PORT", HMR_VAR_REMOTE_PORT},
        {"TRUNK_GROUP", HMR_VAR_TRUNK_GROUP}, {"REALM", HMR_VAR_REALM},
        {"INTERFACE", HMR_VAR_INTERFACE},     {"METHOD", HMR_VAR_METHOD},
        {"RURI_USER", HMR_VAR_RURI_USER},     {"RURI_HOST", HMR_VAR_RURI_HOST},
        {"TO_USER", HMR_VAR_TO_USER},         {"TO_HOST", HMR_VAR_TO_HOST},
        {"FROM_USER", HMR_VAR_FROM_USER},     {"FROM_HOST", HMR_VAR_FROM_HOST},
    };
    auto it = kMap.find(up);
    return it == kMap.end() ? HMR_VAR_NONE : it->second;
}

// Map an element-rule type to a URI element selector. Returns nullopt for
// element kinds the v1 back-ends do not yet manipulate (params, etc.).
inline std::optional<std::uint32_t> uri_element(ast::ElementType t) {
    switch (t) {
        case ast::ElementType::HeaderValue: return HMR_URI_WHOLE;
        case ast::ElementType::UriDisplay:  return HMR_URI_DISPLAY;
        case ast::ElementType::UriUser:     return HMR_URI_USER;
        case ast::ElementType::UriHost:     return HMR_URI_HOST;
        case ast::ElementType::UriPort:     return HMR_URI_PORT;
        default:                            return std::nullopt;
    }
}

inline bool is_case_insensitive(ast::ComparisonType c) {
    return c == ast::ComparisonType::CaseInsensitive ||
           c == ast::ComparisonType::ReferCaseInsensitive;
}
inline bool is_pattern(ast::ComparisonType c) {
    return c == ast::ComparisonType::PatternRule;
}

// Choose the IP matcher variant from the literal's shape: a '/' means a CIDR /
// dotted-netmask subnet, a '-' (with no '/') means an inclusive low-high range,
// otherwise a single address. Mirrors the runtime matchers (matchers.hpp).
inline std::uint32_t ip_match_type(std::string_view pat) {
    if (pat.find('/') != std::string_view::npos) return HMR_MATCH_IP_MASK;
    if (pat.find('-') != std::string_view::npos) return HMR_MATCH_IP_RANGE;
    return HMR_MATCH_IP;
}

// Count the capturing groups in an ECMAScript regex, so a pattern-rule `store`
// knows how many sub-captures a later `$rule.$N` back-reference might name. We
// deliberately *over*-count on ambiguity rather than risk under-counting: a
// surplus slot just holds the empty string, whereas a missing one would drop a
// real capture. Escaped metacharacters (`\(`) and character classes (`[(]`) are
// skipped so a literal '(' is never mistaken for a group; `(?:` / `(?=` / `(?!`
// / `(?<=` / `(?<!` are non-capturing, while a named group `(?<name>...)` does
// capture and is counted.
inline unsigned count_capturing_groups(std::string_view re) {
    unsigned n = 0;
    bool in_class = false;
    for (std::size_t i = 0; i < re.size(); ++i) {
        char c = re[i];
        if (c == '\\') { ++i; continue; }  // skip the escaped character
        if (in_class) {
            if (c == ']') in_class = false;
            continue;
        }
        if (c == '[') { in_class = true; continue; }
        if (c == '(') {
            if (i + 1 < re.size() && re[i + 1] == '?') {
                // (?<name> ...) captures; (?: (?= (?! (?<= (?<! do not.
                if (i + 2 < re.size() && re[i + 2] == '<' && i + 3 < re.size() &&
                    re[i + 3] != '=' && re[i + 3] != '!')
                    ++n;
                continue;
            }
            ++n;  // ordinary capturing group
        }
    }
    return n;
}

// Canonical slot key for capture group `group` of a `store` rule named
// `rule_name`. Group 0 (the whole match) keeps the bare rule name, so a plain
// `$rule` back-reference and the pre-existing single-value `store` slots are
// unchanged; groups >= 1 get a ".$N" suffix. Both back-ends key their slot table
// with this, so a `$rule.$N` load resolves to the very slot the store wrote.
inline std::string capture_slot_key(std::string_view rule_name, unsigned group) {
    if (group == 0) return std::string(rule_name);
    return std::string(rule_name) + ".$" + std::to_string(group);
}

// The base rule/element name of a back-reference path: everything up to the
// first '.' ($rule.$1 -> "rule", $rule.el -> "rule").
inline std::string ref_base_name(std::string_view path) {
    return std::string(path.substr(0, path.find('.')));
}

}  // namespace hmr::codegen
