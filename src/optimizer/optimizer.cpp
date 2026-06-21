// SPDX-License-Identifier: MIT

#include "hmr/optimizer/optimizer.hpp"

#include <algorithm>
#include <string>
#include <unordered_set>

#include "hmr/ast/ast_factory.hpp"

namespace hmr::opt {

using namespace hmr::ast;

namespace {

// Case-insensitive header-name comparison (RFC 3261 §7.3).
bool iequals_header(std::string_view a, std::string_view b) {
    return AstFactory::lower(a) == AstFactory::lower(b);
}

// Additive actions must not be deduplicated/merged: two identical `add` rules
// legitimately insert two headers.
bool is_idempotent(HeaderAction a) { return a != HeaderAction::Add; }
bool is_idempotent(ElementAction a) { return a != ElementAction::Add; }

// Actions that operate on a named header and are meaningless without one.
bool needs_header(HeaderAction a) {
    switch (a) {
        case HeaderAction::Add:
        case HeaderAction::Store:
        case HeaderAction::Manipulate:
        case HeaderAction::Replace:
        case HeaderAction::FindReplaceAll:
        case HeaderAction::Delete:
        case HeaderAction::DeleteElement:
        case HeaderAction::DeleteHeader:
            return true;
        case HeaderAction::None:
        case HeaderAction::SipManip:
        case HeaderAction::Log:
        case HeaderAction::Reject:
            return false;
    }
    return false;
}

std::string sig(const ElementRule& er) {
    std::string s;
    s += AstFactory::to_string(er.type);
    s += '|';
    s += AstFactory::to_string(er.action);
    s += '|';
    s += AstFactory::to_string(er.comparison);
    s += '|';
    s += er.match_value.raw();
    s += '|';
    s += er.new_value.raw();
    s += '|';
    s += er.parameter_name;
    return s;
}

std::string sig(const HeaderRule& hr) {
    std::string s = AstFactory::lower(hr.header_name);
    s += '#';
    s += AstFactory::to_string(hr.action);
    s += '|';
    s += AstFactory::to_string(hr.comparison);
    s += '|';
    s += AstFactory::to_string(hr.msg_type);
    s += '|';
    for (const auto& m : hr.methods) {
        s += m;
        s += ',';
    }
    s += '|';
    s += hr.match_value.raw();
    s += '|';
    s += hr.new_value.raw();
    s += '|';
    for (const auto& er : hr.element_rules) {
        s += sig(er);
        s += ';';
    }
    return s;
}

// Strip a single pair of ^...$ anchors, reporting whether the body is a plain
// literal (no regex metacharacters).
bool is_plain_literal_regex(std::string_view body, std::string& literal_out) {
    if (body.size() >= 1 && body.front() == '^') body.remove_prefix(1);
    if (body.size() >= 1 && body.back() == '$') body.remove_suffix(1);
    static constexpr std::string_view meta = ".[]()*+?{}|\\^$";
    for (char c : body)
        if (meta.find(c) != std::string_view::npos) return false;
    literal_out.assign(body);
    return true;
}

}  // namespace

// ===========================================================================
// 1. Deduplication
// ===========================================================================
std::size_t DeduplicationPass::apply(Ruleset& rs) {
    std::unordered_set<std::string> seen;
    std::size_t removed = 0;
    std::vector<HeaderRule> kept;
    kept.reserve(rs.header_rules.size());
    for (auto& hr : rs.header_rules) {
        if (is_idempotent(hr.action)) {
            std::string key = sig(hr);
            if (!seen.insert(key).second) {
                ++removed;
                continue;
            }
        }
        kept.push_back(std::move(hr));
    }
    rs.header_rules = std::move(kept);
    return removed;
}

// ===========================================================================
// 2. Dead-code elimination
// ===========================================================================
std::size_t DeadCodeEliminationPass::apply(Ruleset& rs) {
    std::size_t removed = 0;
    std::vector<HeaderRule> kept;
    kept.reserve(rs.header_rules.size());
    for (auto& hr : rs.header_rules) {
        // A rule that does nothing, or needs a header but names none, can never
        // have an observable effect.
        if (hr.action == HeaderAction::None ||
            (needs_header(hr.action) && hr.header_name.empty())) {
            ++removed;
            continue;
        }
        // Drop no-op element-rules inside surviving header-rules.
        auto before = hr.element_rules.size();
        std::erase_if(hr.element_rules, [](const ElementRule& er) {
            return er.action == ElementAction::None &&
                   er.type == ElementType::None;
        });
        removed += before - hr.element_rules.size();
        kept.push_back(std::move(hr));
    }
    rs.header_rules = std::move(kept);
    return removed;
}

// ===========================================================================
// 3. Pattern simplification (regex -> equality where possible)
// ===========================================================================
std::size_t PatternSimplificationPass::apply(Ruleset& rs) {
    std::size_t changed = 0;
    auto simplify = [&](ComparisonType& cmp, Value& mv) {
        if (cmp != ComparisonType::PatternRule) return;
        if (!mv.is_regex_literal() || mv.empty()) return;
        std::string literal;
        if (is_plain_literal_regex(mv.raw(), literal) && !literal.empty()) {
            mv = Value::parse(literal, ValueMode::Literal);
            cmp = ComparisonType::CaseSensitive;
            ++changed;
        }
    };
    for (auto& hr : rs.header_rules) {
        simplify(hr.comparison, hr.match_value);
        for (auto& er : hr.element_rules) simplify(er.comparison, er.match_value);
    }
    return changed;
}

// ===========================================================================
// 4. Rule merging
// ===========================================================================
std::size_t RuleMergingPass::apply(Ruleset& rs) {
    std::size_t merged = 0;

    // (a) Within each header-rule, drop duplicate idempotent element-rules.
    for (auto& hr : rs.header_rules) {
        std::unordered_set<std::string> seen;
        auto before = hr.element_rules.size();
        std::vector<ElementRule> kept;
        kept.reserve(hr.element_rules.size());
        for (auto& er : hr.element_rules) {
            if (is_idempotent(er.action)) {
                if (!seen.insert(sig(er)).second) continue;
            }
            kept.push_back(std::move(er));
        }
        merged += before - kept.size();
        hr.element_rules = std::move(kept);
    }

    // (b) Fuse two adjacent `manipulate` rules on the same header with the same
    // guard by concatenating their element-rule lists.
    std::vector<HeaderRule> fused;
    fused.reserve(rs.header_rules.size());
    for (auto& hr : rs.header_rules) {
        if (!fused.empty() && hr.action == HeaderAction::Manipulate &&
            fused.back().action == HeaderAction::Manipulate &&
            iequals_header(fused.back().header_name, hr.header_name) &&
            fused.back().comparison == hr.comparison &&
            fused.back().msg_type == hr.msg_type &&
            fused.back().methods == hr.methods &&
            fused.back().match_value.raw() == hr.match_value.raw()) {
            for (auto& er : hr.element_rules)
                fused.back().element_rules.push_back(std::move(er));
            ++merged;
            continue;
        }
        fused.push_back(std::move(hr));
    }
    rs.header_rules = std::move(fused);
    return merged;
}

// ===========================================================================
// 5. Decision-tree analysis (non-destructive grouping by target header)
// ===========================================================================
std::size_t DecisionTreeAnalysisPass::apply(Ruleset& rs) {
    plan_ = DecisionPlan{};
    for (std::size_t i = 0; i < rs.header_rules.size(); ++i) {
        std::string key = AstFactory::lower(rs.header_rules[i].header_name);
        auto it = std::find_if(plan_.groups.begin(), plan_.groups.end(),
                               [&](const DecisionGroup& g) {
                                   return g.header_name == key;
                               });
        if (it == plan_.groups.end()) {
            plan_.groups.push_back({key, {i}});
        } else {
            it->rule_idx.push_back(i);
        }
    }
    return plan_.groups.size();
}

// ===========================================================================
// Optimizer pipeline (Strategy + chain, run to fixpoint)
// ===========================================================================
OptimizationReport Optimizer::optimize(Ruleset& rs) {
    OptimizationReport report;
    DeduplicationPass dedup;
    DeadCodeEliminationPass dce;
    PatternSimplificationPass pattern;
    RuleMergingPass merge;

    std::size_t dedup_total = 0, dce_total = 0, pat_total = 0, merge_total = 0;
    unsigned iter = 0;
    for (; iter < max_iterations_; ++iter) {
        std::size_t before = dedup_total + dce_total + pat_total + merge_total;
        dedup_total += dedup.run(rs).changes;
        dce_total += dce.run(rs).changes;
        pat_total += pattern.run(rs).changes;
        merge_total += merge.run(rs).changes;
        std::size_t after = dedup_total + dce_total + pat_total + merge_total;
        if (after == before) {  // fixpoint reached
            ++iter;
            break;
        }
    }

    DecisionTreeAnalysisPass tree;
    std::size_t groups = tree.run(rs).changes;

    report.iterations = iter;
    report.passes.push_back({DeduplicationPass::kName, dedup_total});
    report.passes.push_back({DeadCodeEliminationPass::kName, dce_total});
    report.passes.push_back({PatternSimplificationPass::kName, pat_total});
    report.passes.push_back({RuleMergingPass::kName, merge_total});
    report.passes.push_back({DecisionTreeAnalysisPass::kName, groups});
    report.plan = tree.plan();
    return report;
}

}  // namespace hmr::opt
