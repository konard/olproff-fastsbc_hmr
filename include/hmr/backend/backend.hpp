// SPDX-License-Identifier: MIT
//
// backend.hpp — turns textual LLVM IR into a native relocatable object.
//
// The backend is the second half of the code generator: it parses the IR text
// produced by IrGenerator, runs the optimization pipeline (a custom HMR pass
// followed by LLVM's -O<n> module pipeline), and lowers the result to an
// in-memory ELF object via the target's code generator. Linking that object
// into a loadable .so is the Linker's job.
//
// The public surface is intentionally LLVM-free (raw bytes in, raw bytes out),
// so consumers — including the test suite — need no LLVM headers on their path.

#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "hmr/diagnostics/diagnostics.hpp"

namespace hmr::backend {

// Tunables for object generation. Defaults target the host at -O3.
struct BackendOptions {
    unsigned opt_level = 3;          // 0..3 → -O0../-O3
    std::string target_triple;       // empty → host default triple
    std::string cpu = "generic";     // target CPU (e.g. "x86-64", "native")
    std::string features;            // target feature string (e.g. "+avx2")
    bool verify = true;              // run the IR verifier before codegen
};

// A freshly generated relocatable object plus the triple it targets.
struct ObjectCode {
    std::vector<std::uint8_t> bytes;  // ELF .o image
    std::string triple;               // triple the object was built for
};

// Strategy object: IR text → optimized native object. Stateless; safe to reuse.
class Backend {
public:
    [[nodiscard]] Result<ObjectCode> compile_to_object(
        const std::string& llvm_ir, const BackendOptions& opts = {});

    // Run the same HMR + -O<n> pipeline as compile_to_object() but stop before
    // codegen and return the optimized IR as text. Pairs with the unoptimized
    // IR from Compiler::compile_to_ir() to show before/after optimization — see
    // `hmrc dump-ir --opt` and docs/llvm-ir-examples.md.
    [[nodiscard]] Result<std::string> optimize_ir(
        const std::string& llvm_ir, const BackendOptions& opts = {});
};

}  // namespace hmr::backend
