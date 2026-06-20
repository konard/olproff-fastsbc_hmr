// SPDX-License-Identifier: MIT
//
// Backend.cpp — LLVM IR text → optimized native object.

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

}  // namespace

Result<ObjectCode> Backend::compileToObject(const std::string& llvmIR,
                                            const BackendOptions& opts) {
    ensureTargetsInitialized();

    LLVMContext context;
    SMDiagnostic diag;
    std::unique_ptr<Module> module =
        parseAssemblyString(llvmIR, diag, context);
    if (!module) {
        std::string msg;
        raw_string_ostream os(msg);
        diag.print("hmr-ir", os);
        os.flush();
        return make_error("backend: failed to parse generated IR:\n" + msg);
    }

    const std::string triple = opts.targetTriple.empty()
                                   ? sys::getDefaultTargetTriple()
                                   : opts.targetTriple;

    std::string lookupErr;
    const Target* target = TargetRegistry::lookupTarget(triple, lookupErr);
    if (!target)
        return make_error("backend: no target for triple '" + triple +
                          "': " + lookupErr);

    TargetOptions targetOpts;
    auto reloc = std::optional<Reloc::Model>(Reloc::PIC_);  // PIC → shared object
    std::unique_ptr<TargetMachine> tm(target->createTargetMachine(
        triple, opts.cpu, opts.features, targetOpts, reloc));
    if (!tm) return make_error("backend: could not create target machine");

    module->setTargetTriple(triple);
    module->setDataLayout(tm->createDataLayout());

    if (opts.verify) {
        std::string verr;
        raw_string_ostream os(verr);
        if (verifyModule(*module, &os)) {
            os.flush();
            return make_error("backend: module failed verification:\n" + verr);
        }
    }

    // --- optimization pipeline (new pass manager) --------------------------
    LoopAnalysisManager lam;
    FunctionAnalysisManager fam;
    CGSCCAnalysisManager cgam;
    ModuleAnalysisManager mam;

    PassBuilder pb(tm.get());
    pb.registerModuleAnalyses(mam);
    pb.registerCGSCCAnalyses(cgam);
    pb.registerFunctionAnalyses(fam);
    pb.registerLoopAnalyses(lam);
    pb.crossRegisterProxies(lam, fam, cgam, mam);

    // Our custom pass always runs first, then the standard -O<n> pipeline.
    {
        ModulePassManager pre;
        pre.addPass(HmrAttributePass());
        pre.run(*module, mam);
    }
    {
        const OptimizationLevel level = toLevel(opts.optLevel);
        ModulePassManager mpm =
            (opts.optLevel == 0) ? pb.buildO0DefaultPipeline(level)
                                 : pb.buildPerModuleDefaultPipeline(level);
        mpm.run(*module, mam);
    }

    // --- object emission (legacy codegen pass manager) ---------------------
    SmallVector<char, 0> buffer;
    raw_svector_ostream objStream(buffer);
    legacy::PassManager codegenPM;
    if (tm->addPassesToEmitFile(codegenPM, objStream, /*DwoOut=*/nullptr,
                                CodeGenFileType::ObjectFile)) {
        return make_error("backend: target cannot emit object files");
    }
    codegenPM.run(*module);

    ObjectCode out;
    out.triple = triple;
    out.bytes.assign(buffer.begin(), buffer.end());
    return out;
}

}  // namespace hmr::backend
