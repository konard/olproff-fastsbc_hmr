// SPDX-License-Identifier: MIT
//
// IrGenerator.hpp — lowers an (already-optimized) HMR ruleset directly to
// LLVM IR. No intermediate C++/Clang frontend is involved: the generator drives
// llvm::IRBuilder itself, emitting the module's single entry point `hmr_apply`
// plus the `hmr_module_info` descriptor consumed by the runtime.
//
// The public surface is intentionally LLVM-free: generation produces *textual*
// LLVM IR (a std::string). That keeps this header dependency-light (tests and
// the CLI can inspect IR without the LLVM headers on their include path) and
// makes the codegen↔backend boundary a clean, snapshot-testable artifact. The
// backend re-parses the text — negligible next to optimization/codegen — and
// turns it into a native object.
//
// GoF roles: the generator is a Builder (assembles a Module piece by piece) and
// walks the AST in Visitor style (enterRuleset → header-rule → element-rule).

#pragma once

#include <string>

#include "hmr/ast/Ast.hpp"
#include "hmr/diagnostics/Diagnostics.hpp"
#include "hmr/optimizer/Optimizer.hpp"

namespace hmr::codegen {

// Knobs for IR generation. Defaults target the host.
struct IrGenOptions {
    std::string moduleId = "hmr.module";  // LLVM module identifier
    std::string targetTriple;             // empty → host default triple
    bool emitComments = true;             // annotate IR with rule names
};

// Side-band facts about the generated module, useful for diagnostics/tests.
struct IrModuleStats {
    std::string moduleName;     // ruleset name embedded in hmr_module_info
    unsigned numSlots = 0;      // store/load slots the runtime must allocate
    unsigned numRegexes = 0;    // precompiled regexes in the module's table
    unsigned numHeaderRules = 0;
};

class IrGenerator {
public:
    // Generate textual LLVM IR for `rs` (which should already be optimized; the
    // `plan` orders emission by target header). On success returns a verified
    // module as text and, if `stats` is non-null, fills in module facts.
    [[nodiscard]] Result<std::string> generateIR(const ast::Ruleset& rs,
                                                 const opt::DecisionPlan& plan,
                                                 const IrGenOptions& opts = {},
                                                 IrModuleStats* stats = nullptr);
};

}  // namespace hmr::codegen
