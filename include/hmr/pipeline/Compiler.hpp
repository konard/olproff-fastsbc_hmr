// SPDX-License-Identifier: MIT
//
// Compiler.hpp — the end-to-end pipeline Facade (GoF Facade).
//
// One call drives the whole chain and hides every subsystem behind a tiny
// surface:
//
//   HMR text → Lexer → Parser → Optimizer → IrGenerator → Backend → Linker → .so
//
// Each stage is an independent component with its own header; the Facade wires
// them together, threads options through, and converts any stage failure into a
// single hmr::Result. Callers that only want the intermediate IR (tests,
// `hmrc dump-ir`) can stop early with compileToIR().

#pragma once

#include <cstddef>
#include <string>

#include "hmr/diagnostics/Diagnostics.hpp"

namespace hmr::pipeline {

// Everything tunable about a compilation. Defaults produce an -O3 host module.
struct CompileOptions {
    std::string outputPath = "a.so";      // destination shared object
    unsigned optLevel = 3;                // backend optimization level (0..3)
    std::string targetTriple;             // empty → host default triple
    std::string cpu = "generic";          // target CPU
    std::string features;                 // target feature string (e.g. "+avx2")
    std::string moduleId = "hmr.module";  // LLVM module identifier
    bool emitComments = true;             // annotate generated IR
    bool runOptimizer = true;             // run the AST-level optimizer passes
    bool stripDebug = true;               // strip the linked .so
    std::string linkerDriver;             // override the cc/ld driver
    std::string* irOut = nullptr;         // if set, receives the generated IR
};

// Facts about a finished compilation.
struct CompileResult {
    std::string outputPath;          // where the .so was written
    std::size_t byteSize = 0;        // size of the written .so, in bytes
    std::size_t objectBytes = 0;     // size of the pre-link relocatable object
    std::string moduleName;          // ruleset name embedded in the module
    unsigned numHeaderRules = 0;     // header rules emitted
    unsigned numSlots = 0;           // store/load slots the runtime allocates
    unsigned numRegexes = 0;         // precompiled regexes in the module
    std::string triple;              // triple the object was built for
};

class Compiler {
public:
    // Full pipeline: compile `source` and write a loadable .so to
    // opts.outputPath. Returns module facts (including the written size) or the
    // first stage's error.
    [[nodiscard]] Result<CompileResult> compileToFile(
        const std::string& source, const CompileOptions& opts = {});

    // Front half only: source → optimized AST → textual LLVM IR. Useful for
    // snapshot tests and `hmrc dump-ir` without invoking the backend/linker.
    [[nodiscard]] Result<std::string> compileToIR(
        const std::string& source, const CompileOptions& opts = {});
};

}  // namespace hmr::pipeline
