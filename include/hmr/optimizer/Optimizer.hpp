// SPDX-License-Identifier: MIT
//
// Optimizer.hpp — the rule-level optimizer and its CRTP pass set.
//
// The optimizer runs a chain of transform passes to a fixpoint, then a final
// analysis pass that derives the decision plan consumed by the code generator.
// Passes:
//   1. DeduplicationPass        — drop header-rules identical to an earlier one
//   2. DeadCodeEliminationPass  — drop rules that can never fire / do nothing
//   3. PatternSimplificationPass— lower trivial regexes to plain comparisons
//   4. RuleMergingPass          — fuse compatible adjacent rules / element-rules
//   5. DecisionTreeAnalysisPass — group rules by target header (analysis only)
//
// The first four mutate the AST; the fifth produces a DecisionPlan without
// changing rule semantics.

#pragma once

#include <cstddef>
#include <string>
#include <vector>

#include "hmr/ast/Ast.hpp"
#include "hmr/optimizer/Pass.hpp"

namespace hmr::opt {

// --- Transform passes ------------------------------------------------------
class DeduplicationPass final : public PassBase<DeduplicationPass> {
public:
    static constexpr std::string_view kName = "deduplication";
    std::size_t apply(ast::Ruleset& rs);
};

class DeadCodeEliminationPass final : public PassBase<DeadCodeEliminationPass> {
public:
    static constexpr std::string_view kName = "dead-code-elimination";
    std::size_t apply(ast::Ruleset& rs);
};

class PatternSimplificationPass final
    : public PassBase<PatternSimplificationPass> {
public:
    static constexpr std::string_view kName = "pattern-simplification";
    std::size_t apply(ast::Ruleset& rs);
};

class RuleMergingPass final : public PassBase<RuleMergingPass> {
public:
    static constexpr std::string_view kName = "rule-merging";
    std::size_t apply(ast::Ruleset& rs);
};

// --- Analysis pass: decision plan ------------------------------------------
struct DecisionGroup {
    std::string headerName;            // target header (lower-cased)
    std::vector<std::size_t> ruleIdx;  // indices into Ruleset::headerRules
};

struct DecisionPlan {
    std::vector<DecisionGroup> groups;  // one per distinct target header
    [[nodiscard]] std::size_t size() const { return groups.size(); }
};

class DecisionTreeAnalysisPass final
    : public PassBase<DecisionTreeAnalysisPass> {
public:
    static constexpr std::string_view kName = "decision-tree";
    std::size_t apply(ast::Ruleset& rs);  // populates plan(); returns group count
    [[nodiscard]] const DecisionPlan& plan() const { return plan_; }

private:
    DecisionPlan plan_;
};

// --- Optimizer facade ------------------------------------------------------
struct OptimizationReport {
    std::vector<PassResult> passes;  // per-pass change counts (in run order)
    DecisionPlan plan;
    unsigned iterations = 0;

    [[nodiscard]] std::size_t totalChanges() const {
        std::size_t n = 0;
        for (const auto& p : passes) n += p.changes;
        return n;
    }
};

class Optimizer {
public:
    explicit Optimizer(unsigned maxIterations = 4)
        : maxIterations_(maxIterations) {}

    // Run the transform passes to a fixpoint (or maxIterations), then the
    // decision-tree analysis. Mutates `rs` in place.
    OptimizationReport optimize(ast::Ruleset& rs);

private:
    unsigned maxIterations_;
};

}  // namespace hmr::opt
