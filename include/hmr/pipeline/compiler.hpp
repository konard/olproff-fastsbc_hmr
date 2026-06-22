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

// compiler.hpp — the end-to-end pipeline Facade (GoF Facade).
//
// One call drives the whole chain and hides every subsystem behind a tiny
// surface:
//
//   HMR text → Parser → Optimizer → IrGenerator → Backend → Linker → .so
//
// Each stage is an independent component with its own header; the Facade wires
// them together, threads options through, and converts any stage failure into a
// single hmr::Result. Callers that only want the intermediate IR (tests,
// `hmrc dump-ir`) can stop early with compile_to_ir().

#pragma once

#include <cstddef>
#include <string>

#include "hmr/diagnostics/diagnostics.hpp"

namespace hmr::pipeline {

// Everything tunable about a compilation. Defaults produce an -O3 host module.
struct CompileOptions {
    std::string output_path = "a.so";      // destination shared object
    unsigned opt_level = 3;                // backend optimization level (0..3)
    std::string target_triple;             // empty → host default triple
    std::string cpu = "generic";           // target CPU
    std::string features;                  // target feature string (e.g. "+avx2")
    std::string module_id = "hmr.module";  // LLVM module identifier
    bool emit_comments = true;             // annotate generated IR
    bool run_optimizer = true;             // run the AST-level optimizer passes
    bool strip_debug = true;               // strip the linked .so
    std::string linker_driver;             // override the cc/ld driver
    std::string* ir_out = nullptr;         // if set, receives the generated IR
};

// Facts about a finished compilation.
struct CompileResult {
    std::string output_path;          // where the .so was written
    std::size_t byte_size = 0;        // size of the written .so, in bytes
    std::size_t object_bytes = 0;     // size of the pre-link relocatable object
    std::string module_name;          // ruleset name embedded in the module
    unsigned num_header_rules = 0;    // header rules emitted
    unsigned num_slots = 0;           // store/load slots the runtime allocates
    unsigned num_regexes = 0;         // precompiled regexes in the module
    std::string triple;               // triple the object was built for
};

class Compiler {
public:
    // Full pipeline: compile `source` and write a loadable .so to
    // opts.output_path. Returns module facts (including the written size) or the
    // first stage's error.
    [[nodiscard]] Result<CompileResult> compile_to_file(
        const std::string& source, const CompileOptions& opts = {});

    // Front half only: source → optimized AST → textual LLVM IR. Useful for
    // snapshot tests and `hmrc dump-ir` without invoking the backend/linker.
    [[nodiscard]] Result<std::string> compile_to_ir(
        const std::string& source, const CompileOptions& opts = {});
};

}  // namespace hmr::pipeline
