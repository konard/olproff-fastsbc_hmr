// SPDX-License-Identifier: MIT
//
// interpreter.hpp — a tree-walking interpreter over an HMR Ruleset.
//
// This is the reference implementation the compiler is measured against: it
// applies a ruleset by walking the AST at packet time and driving the *same*
// `hmr_rt_*` runtime entry points the generated module calls, in the same order
// with the same immediate operands. Because both paths bottom out in the one
// runtime (matchers, URI rewrite, value builder, arena), the interpreter is a
// faithful oracle:
//
//   * benchmarks/ uses it for the review-required "compiled vs interpreter"
//     comparison — same work, the only delta is per-packet AST traversal +
//     switch dispatch + un-inlined runtime calls vs. straight-line native code,
//   * the fuzzer uses it as a differential oracle — a random ruleset applied by
//     the interpreter and by the compiled .so must produce byte-identical output.
//
// Construction pre-scans the ruleset to assign store/load slot indices and build
// the precompiled-regex table exactly as src/codegen/ir_generator.cpp does, then
// publishes a synthetic HmrModuleInfo so a host can size an HmrContext via
// hmr::runtime::make_context(). The scan and the apply walk are the same code
// (Interpreter::walk) run in two modes, so they cannot drift apart.

#pragma once

#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "hmr/ast/ast.hpp"
#include "hmr/runtime/hmr_runtime.h"

struct HmrSipMsg;   // concrete in runtime/sip_message.hpp
struct HmrContext;  // concrete in runtime/context.hpp

namespace hmr::interp {

class Interpreter {
public:
    explicit Interpreter(const ast::Ruleset& rs);

    // Synthetic module descriptor (ABI version + name + slot count + regex
    // table). Stable for the interpreter's lifetime; pass to make_context().
    [[nodiscard]] const HmrModuleInfo& info() const noexcept { return info_; }

    // Apply the ruleset to `msg` using a per-worker `ctx` prepared from info().
    // Returns an HmrVerdict (HMR_OK / HMR_REJECTED), matching hmr_apply.
    [[nodiscard]] int apply(HmrSipMsg* msg, HmrContext* ctx) const;

private:
    // Control flow signalled up the walk (a reject terminates the whole apply).
    enum class Flow { Continue, Rejected };

    // The single walk, run in two modes. scan==true (msg/ctx null) only assigns
    // the slot/regex tables and never short-circuits on a runtime guard, so it
    // visits every construct the code generator would emit. scan==false is the
    // real application: guards short-circuit and the hmr_rt_* calls mutate.
    int walk(HmrSipMsg* msg, HmrContext* ctx, bool scan) const;
    Flow run_header_rule(const ast::HeaderRule& hr, std::size_t idx,
                         HmrSipMsg* msg, HmrContext* ctx, bool scan) const;
    // Returns true if the action terminated the rule (reject).
    bool run_header_action(const ast::HeaderRule& hr, std::size_t idx, HmrStr hv,
                           bool have_hv, HmrSipMsg* msg, HmrContext* ctx) const;
    Flow run_element_rules(const ast::HeaderRule& hr, HmrSipMsg* msg,
                           HmrContext* ctx, bool scan) const;

    // Mirror of ir_generator's emit_value / emit_match_guard.
    HmrStr eval_value(const ast::Value& v, HmrContext* ctx) const;
    // Returns whether the guard lets the rule fire (always true in scan mode,
    // where the result is ignored).
    bool eval_match_guard(ast::ComparisonType cmp, ast::MatchValType mvt,
                          const ast::Value& mv, HmrStr subject,
                          HmrContext* ctx) const;

    // Memoized tables (mutable: populated once during the constructor scan, then
    // frozen). When frozen_ they are pure lookups, so apply() stays const.
    unsigned add_regex(const std::string& pattern, bool ci) const;
    unsigned slot_for(const std::string& name) const;
    unsigned slot_for_ref(const std::string& path, int capture_index) const;

    // Stable storage for literal HmrStr views handed to the runtime. The code
    // generator interns string literals into the module's .rodata; the
    // interpreter interns them here so a returned view outlives the call that
    // consumes it (the runtime copies synchronously, but a value built from a
    // temporary std::string would dangle before the copy). Deduplicated, so it
    // does not grow across apply() calls.
    HmrStr intern(std::string_view s) const;

    const ast::Ruleset& rs_;
    std::string name_;  // module name (backing info_.name)

    mutable std::vector<std::pair<std::string, std::uint32_t>> regexes_;
    mutable std::map<std::string, unsigned> slots_;
    mutable std::set<std::string> literal_pool_;
    bool frozen_ = false;

    std::vector<HmrRegexEntry> regex_entries_;  // backs info_.regexes
    HmrModuleInfo info_{};
};

}  // namespace hmr::interp
