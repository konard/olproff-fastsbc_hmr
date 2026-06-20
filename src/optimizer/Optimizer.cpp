// SPDX-License-Identifier: MIT

#include "hmr/optimizer/Optimizer.hpp"

#include <algorithm>
#include <string>
#include <unordered_set>

#include "hmr/ast/AstFactory.hpp"

namespace hmr::opt {

using namespace hmr::ast;

namespace {

// Case-insensitive header-name comparison (RFC 3261 §7.3).
bool iequalsHeader(std::string_view a, std::string_view b) {
    return AstFactory::lower(a) == AstFactory::lower(b);
}

// Additive actions must not be deduplicated/merged: two identical `add` rules
// legitimately insert two headers.
bool isIdempotent(HeaderAction a) { return a != HeaderAction::Add; }
bool isIdempotent(ElementAction a) { return a != ElementAction::Add; }

// Actions that operate on a named header and are meaningless without one.
bool needsHeader(HeaderAction a) {
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
    s += AstFactory::toString(er.type);
    s += '|';
    s += AstFactory::toString(er.action);
    s += '|';
    s += AstFactory::toString(er.comparison);
    s += '|';
    s += er.matchValue.raw();
    s += '|';
    s += er.newValue.raw();
    s += '|';
    s += er.parameterName;
    return s;
}

std::string sig(const HeaderRule& hr) {
    std::string s = AstFactory::lower(hr.headerName);
    s += '#';
    s += AstFactory::toString(hr.action);
    s += '|';
    s += AstFactory::toString(hr.comparison);
    s += '|';
    s += AstFactory::toString(hr.msgType);
    s += '|';
    for (const auto& m : hr.methods) {
        s += m;
        s += ',';
    }
    s += '|';
    s += hr.matchValue.raw();
    s += '|';
    s += hr.newValue.raw();
    s += '|';
    for (const auto& er : hr.elementRules) {
        s += sig(er);
        s += ';';
    }
    return s;
}

// Strip a single pair of ^...$ anchors, reporting whether the body is a plain
// literal (no regex metacharacters).
bool isPlainLiteralRegex(std::string_view body, std::string& literalOut) {
    if (body.size() >= 1 && body.front() == '^') body.remove_prefix(1);
    if (body.size() >= 1 && body.back() == '$') body.remove_suffix(1);
    static constexpr std::string_view meta = ".[]()*+?{}|\\^$";
    for (char c : body)
        if (meta.find(c) != std::string_view::npos) return false;
    literalOut.assign(body);
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
    kept.reserve(rs.headerRules.size());
    for (auto& hr : rs.headerRules) {
        if (isIdempotent(hr.action)) {
            std::string key = sig(hr);
            if (!seen.insert(key).second) {
                ++removed;
                continue;
            }
        }
        kept.push_back(std::move(hr));
    }
    rs.headerRules = std::move(kept);
    return removed;
}

// ===========================================================================
// 2. Dead-code elimination
// ===========================================================================
std::size_t DeadCodeEliminationPass::apply(Ruleset& rs) {
    std::size_t removed = 0;
    std::vector<HeaderRule> kept;
    kept.reserve(rs.headerRules.size());
    for (auto& hr : rs.headerRules) {
        // A rule that does nothing, or needs a header but names none, can never
        // have an observable effect.
        if (hr.action == HeaderAction::None ||
            (needsHeader(hr.action) && hr.headerName.empty())) {
            ++removed;
            continue;
        }
        // Drop no-op element-rules inside surviving header-rules.
        auto before = hr.elementRules.size();
        std::erase_if(hr.elementRules, [](const ElementRule& er) {
            return er.action == ElementAction::None &&
                   er.type == ElementType::None;
        });
        removed += before - hr.elementRules.size();
        kept.push_back(std::move(hr));
    }
    rs.headerRules = std::move(kept);
    return removed;
}

// ===========================================================================
// 3. Pattern simplification (regex -> equality where possible)
// ===========================================================================
std::size_t PatternSimplificationPass::apply(Ruleset& rs) {
    std::size_t changed = 0;
    auto simplify = [&](ComparisonType& cmp, Value& mv) {
        if (cmp != ComparisonType::PatternRule) return;
        if (!mv.isRegexLiteral() || mv.empty()) return;
        std::string literal;
        if (isPlainLiteralRegex(mv.raw(), literal) && !literal.empty()) {
            mv = Value::parse(literal, ValueMode::Literal);
            cmp = ComparisonType::CaseSensitive;
            ++changed;
        }
    };
    for (auto& hr : rs.headerRules) {
        simplify(hr.comparison, hr.matchValue);
        for (auto& er : hr.elementRules) simplify(er.comparison, er.matchValue);
    }
    return changed;
}

// ===========================================================================
// 4. Rule merging
// ===========================================================================
std::size_t RuleMergingPass::apply(Ruleset& rs) {
    std::size_t merged = 0;

    // (a) Within each header-rule, drop duplicate idempotent element-rules.
    for (auto& hr : rs.headerRules) {
        std::unordered_set<std::string> seen;
        auto before = hr.elementRules.size();
        std::vector<ElementRule> kept;
        kept.reserve(hr.elementRules.size());
        for (auto& er : hr.elementRules) {
            if (isIdempotent(er.action)) {
                if (!seen.insert(sig(er)).second) continue;
            }
            kept.push_back(std::move(er));
        }
        merged += before - kept.size();
        hr.elementRules = std::move(kept);
    }

    // (b) Fuse two adjacent `manipulate` rules on the same header with the same
    // guard by concatenating their element-rule lists.
    std::vector<HeaderRule> fused;
    fused.reserve(rs.headerRules.size());
    for (auto& hr : rs.headerRules) {
        if (!fused.empty() && hr.action == HeaderAction::Manipulate &&
            fused.back().action == HeaderAction::Manipulate &&
            iequalsHeader(fused.back().headerName, hr.headerName) &&
            fused.back().comparison == hr.comparison &&
            fused.back().msgType == hr.msgType &&
            fused.back().methods == hr.methods &&
            fused.back().matchValue.raw() == hr.matchValue.raw()) {
            for (auto& er : hr.elementRules)
                fused.back().elementRules.push_back(std::move(er));
            ++merged;
            continue;
        }
        fused.push_back(std::move(hr));
    }
    rs.headerRules = std::move(fused);
    return merged;
}

// ===========================================================================
// 5. Decision-tree analysis (non-destructive grouping by target header)
// ===========================================================================
std::size_t DecisionTreeAnalysisPass::apply(Ruleset& rs) {
    plan_ = DecisionPlan{};
    for (std::size_t i = 0; i < rs.headerRules.size(); ++i) {
        std::string key = AstFactory::lower(rs.headerRules[i].headerName);
        auto it = std::find_if(plan_.groups.begin(), plan_.groups.end(),
                               [&](const DecisionGroup& g) {
                                   return g.headerName == key;
                               });
        if (it == plan_.groups.end()) {
            plan_.groups.push_back({key, {i}});
        } else {
            it->ruleIdx.push_back(i);
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

    std::size_t dedupTotal = 0, dceTotal = 0, patTotal = 0, mergeTotal = 0;
    unsigned iter = 0;
    for (; iter < maxIterations_; ++iter) {
        std::size_t before = dedupTotal + dceTotal + patTotal + mergeTotal;
        dedupTotal += dedup.run(rs).changes;
        dceTotal += dce.run(rs).changes;
        patTotal += pattern.run(rs).changes;
        mergeTotal += merge.run(rs).changes;
        std::size_t after = dedupTotal + dceTotal + patTotal + mergeTotal;
        if (after == before) {  // fixpoint reached
            ++iter;
            break;
        }
    }

    DecisionTreeAnalysisPass tree;
    std::size_t groups = tree.run(rs).changes;

    report.iterations = iter;
    report.passes.push_back({DeduplicationPass::kName, dedupTotal});
    report.passes.push_back({DeadCodeEliminationPass::kName, dceTotal});
    report.passes.push_back({PatternSimplificationPass::kName, patTotal});
    report.passes.push_back({RuleMergingPass::kName, mergeTotal});
    report.passes.push_back({DecisionTreeAnalysisPass::kName, groups});
    report.plan = tree.plan();
    return report;
}

}  // namespace hmr::opt
