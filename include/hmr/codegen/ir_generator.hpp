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

// ir_generator.hpp — lowers an (already-optimized) HMR ruleset directly to
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
// walks the AST in Visitor style (ruleset → header-rule → element-rule).

#pragma once

#include <string>

#include "hmr/ast/ast.hpp"
#include "hmr/diagnostics/diagnostics.hpp"
#include "hmr/optimizer/optimizer.hpp"

namespace hmr::codegen {

// Knobs for IR generation. Defaults target the host.
struct IrGenOptions {
    std::string module_id = "hmr.module";  // LLVM module identifier
    std::string target_triple;             // empty → host default triple
    bool emit_comments = true;             // annotate IR with rule names
};

// Side-band facts about the generated module, useful for diagnostics/tests.
struct IrModuleStats {
    std::string module_name;       // ruleset name embedded in hmr_module_info
    unsigned num_slots = 0;        // store/load slots the runtime must allocate
    unsigned num_regexes = 0;      // precompiled regexes in the module's table
    unsigned num_header_rules = 0;
};

class IrGenerator {
public:
    // Generate textual LLVM IR for `rs` (which should already be optimized; the
    // `plan` orders emission by target header). On success returns a verified
    // module as text and, if `stats` is non-null, fills in module facts.
    [[nodiscard]] Result<std::string> generate_ir(const ast::Ruleset& rs,
                                                  const opt::DecisionPlan& plan,
                                                  const IrGenOptions& opts = {},
                                                  IrModuleStats* stats = nullptr);
};

}  // namespace hmr::codegen
