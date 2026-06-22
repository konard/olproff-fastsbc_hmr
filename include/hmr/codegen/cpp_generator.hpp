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

// cpp_generator.hpp — lowers an (already-optimized) HMR ruleset to a standalone
// C++ translation unit: the "GCC approach" back-end.
//
// This is the counterfactual the issue explicitly rejects ("Direct LLVM IR
// generation from HMR AST — no intermediate C++ code, no Clang frontend, no GCC
// toolchain dependency"). Instead of driving llvm::IRBuilder, it emits C++ source
// and hands it to a C++ compiler (`g++ -O3 -shared -fPIC`). The resulting .so is
// ABI-identical to the LLVM-direct module: it exports the same `hmr_apply` +
// `hmr_module_info` symbols, leaves every `hmr_rt_*` entry point undefined
// (resolved at dlopen against the host, exactly like the LLVM path), and is
// loadable by the same ModuleManager.
//
// benchmarks/bench_pipeline.cpp compiles one ruleset both ways and compares
// compile time and apply time. That is review #5's required "comparison with the
// GCC approach" and the empirical case for the issue's core thesis: emitting LLVM
// IR directly is dramatically faster to compile than spawning a second C++
// compiler, with identical run-time speed.
//
// The generator reuses codegen/lowering.hpp and walks the AST in the same order
// as ir_generator.cpp / interpreter.cpp, so it assigns identical variable ids,
// URI selectors, match-type codes, slot indices and regex ids — the three
// back-ends cannot drift. Unlike the LLVM emitter it never decomposes HmrStr into
// (ptr, len) scalars: in C++ source HmrStr is passed and returned by value and
// the C++ compiler performs the ABI lowering.

#pragma once

#include <string>

#include "hmr/ast/ast.hpp"
#include "hmr/diagnostics/diagnostics.hpp"

namespace hmr::codegen {

// Knobs for C++ source generation. Defaults target the in-tree runtime header.
struct CppGenOptions {
    std::string module_name;  // empty → ruleset name (or "hmr")
    std::string runtime_header =
        "hmr/runtime/hmr_runtime.h";  // #include target for the ABI
    bool emit_comments = true;        // annotate the source with rule names
};

// Side-band facts about the generated module, mirroring IrModuleStats.
struct CppGenStats {
    std::string module_name;
    unsigned num_slots = 0;
    unsigned num_regexes = 0;
    unsigned num_header_rules = 0;
};

// Lowers `rs` to a complete, compilable C++ translation unit. The result is
// self-contained: `g++ -O3 -shared -fPIC -I<include> out.cpp -o out.so` yields a
// module the runtime can dlopen. `rs` should already be optimized (the generator
// emits in source order, like ir_generator.cpp). GoF roles: Builder assembling a
// translation unit, walking the AST in Visitor order.
class CppGenerator {
public:
    [[nodiscard]] Result<std::string> generate_cpp(const ast::Ruleset& rs,
                                                   const CppGenOptions& opts = {},
                                                   CppGenStats* stats = nullptr);
};

}  // namespace hmr::codegen
