// SPDX-License-Identifier: MIT
//
// Backend.cpp — LLVM IR text → optimized native object (or optimized IR text).

#include "hmr/backend/Backend.hpp"

#include <memory>
#include <mutex>
#include <optional>

#include "llvm/AsmParser/Parser.h"
#include "llvm/IR/Attributes.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/LegacyPassManager.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/PassManager.h"
#include "llvm/IR/Verifier.h"
#include "llvm/MC/TargetRegistry.h"
#include "llvm/Passes/PassBuilder.h"
#include "llvm/Support/CodeGen.h"
#include "llvm/Support/SourceMgr.h"
#include "llvm/Support/TargetSelect.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Target/TargetMachine.h"
#include "llvm/Target/TargetOptions.h"
#include "llvm/TargetParser/Host.h"

namespace hmr::backend {

namespace {

using namespace llvm;

// Initialize the native target exactly once, regardless of how many times the
// backend runs (idempotent + thread-safe).
void ensureTargetsInitialized() {
    static std::once_flag once;
    std::call_once(once, [] {
        InitializeNativeTarget();
        InitializeNativeTargetAsmPrinter();
        InitializeNativeTargetAsmParser();
    });
}

// Custom HMR pass (new pass manager): annotate the entry point and the runtime
// callbacks as `nounwind`. Our C ABI never propagates exceptions across module
// boundaries, so this is always sound — and it lets the optimizer drop unwind
// edges and landing pads from hmr_apply, shrinking the fast path.
struct HmrAttributePass : PassInfoMixin<HmrAttributePass> {
    PreservedAnalyses run(Module& m, ModuleAnalysisManager&) {
        bool changed = false;
        for (Function& f : m) {
            StringRef n = f.getName();
            if (n == "hmr_apply" || n.starts_with("hmr_rt_")) {
                if (!f.hasFnAttribute(Attribute::NoUnwind)) {
                    f.addFnAttr(Attribute::NoUnwind);
                    changed = true;
                }
            }
        }
        return changed ? PreservedAnalyses::none() : PreservedAnalyses::all();
    }
};

OptimizationLevel toLevel(unsigned o) {
    switch (o) {
        case 0: return OptimizationLevel::O0;
        case 1: return OptimizationLevel::O1;
        case 2: return OptimizationLevel::O2;
        default: return OptimizationLevel::O3;
    }
}

// A parsed, target-configured, fully optimized module bundled with everything
// that must outlive it. Members are declared context-first so destruction runs
// in reverse — module before its target machine and LLVMContext.
struct OptimizedModule {
    std::unique_ptr<LLVMContext> context;
    std::unique_ptr<TargetMachine> tm;
    std::unique_ptr<Module> module;
    std::string triple;
};

// Shared front half of both public entry points: parse the IR text, configure
// the host (or requested) target machine, verify, then run the HMR pass plus
// the standard -O<n> module pipeline. On return the module is fully optimized;
// callers either emit an object from it or print it back to text.
Result<OptimizedModule> parseAndOptimize(const std::string& llvmIR,
                                         const BackendOptions& opts) {
    ensureTargetsInitialized();

    OptimizedModule out;
    out.context = std::make_unique<LLVMContext>();

    SMDiagnostic diag;
    out.module = parseAssemblyString(llvmIR, diag, *out.context);
    if (!out.module) {
        std::string msg;
        raw_string_ostream os(msg);
        diag.print("hmr-ir", os);
        os.flush();
        return make_error("backend: failed to parse generated IR:\n" + msg);
    }
    // parseAssemblyString names the module after its (anonymous) buffer; restore
    // the original identifier so optimized-IR dumps match the generator's header.
    if (!out.module->getSourceFileName().empty())
        out.module->setModuleIdentifier(out.module->getSourceFileName());

    out.triple = opts.targetTriple.empty() ? sys::getDefaultTargetTriple()
                                           : opts.targetTriple;

    std::string lookupErr;
    const Target* target = TargetRegistry::lookupTarget(out.triple, lookupErr);
    if (!target)
        return make_error("backend: no target for triple '" + out.triple +
                          "': " + lookupErr);

    TargetOptions targetOpts;
    auto reloc = std::optional<Reloc::Model>(Reloc::PIC_);  // PIC → shared object
    out.tm.reset(target->createTargetMachine(out.triple, opts.cpu, opts.features,
                                             targetOpts, reloc));
    if (!out.tm) return make_error("backend: could not create target machine");

    out.module->setTargetTriple(out.triple);
    out.module->setDataLayout(out.tm->createDataLayout());

    if (opts.verify) {
        std::string verr;
        raw_string_ostream os(verr);
        if (verifyModule(*out.module, &os)) {
            os.flush();
            return make_error("backend: module failed verification:\n" + verr);
        }
    }

    // --- optimization pipeline (new pass manager) --------------------------
    LoopAnalysisManager lam;
    FunctionAnalysisManager fam;
    CGSCCAnalysisManager cgam;
    ModuleAnalysisManager mam;

    PassBuilder pb(out.tm.get());
    pb.registerModuleAnalyses(mam);
    pb.registerCGSCCAnalyses(cgam);
    pb.registerFunctionAnalyses(fam);
    pb.registerLoopAnalyses(lam);
    pb.crossRegisterProxies(lam, fam, cgam, mam);

    // Our custom pass always runs first, then the standard -O<n> pipeline.
    {
        ModulePassManager pre;
        pre.addPass(HmrAttributePass());
        pre.run(*out.module, mam);
    }
    {
        const OptimizationLevel level = toLevel(opts.optLevel);
        ModulePassManager mpm =
            (opts.optLevel == 0) ? pb.buildO0DefaultPipeline(level)
                                 : pb.buildPerModuleDefaultPipeline(level);
        mpm.run(*out.module, mam);
    }

    return out;
}

}  // namespace

Result<ObjectCode> Backend::compileToObject(const std::string& llvmIR,
                                            const BackendOptions& opts) {
    auto opt = parseAndOptimize(llvmIR, opts);
    if (!opt) return std::unexpected(opt.error());

    // --- object emission (legacy codegen pass manager) ---------------------
    SmallVector<char, 0> buffer;
    raw_svector_ostream objStream(buffer);
    legacy::PassManager codegenPM;
    if (opt->tm->addPassesToEmitFile(codegenPM, objStream, /*DwoOut=*/nullptr,
                                     CodeGenFileType::ObjectFile)) {
        return make_error("backend: target cannot emit object files");
    }
    codegenPM.run(*opt->module);

    ObjectCode out;
    out.triple = opt->triple;
    out.bytes.assign(buffer.begin(), buffer.end());
    return out;
}

Result<std::string> Backend::optimizeIR(const std::string& llvmIR,
                                        const BackendOptions& opts) {
    auto opt = parseAndOptimize(llvmIR, opts);
    if (!opt) return std::unexpected(opt.error());

    std::string out;
    raw_string_ostream os(out);
    opt->module->print(os, /*AAW=*/nullptr);
    os.flush();
    return out;
}

}  // namespace hmr::backend
