// SPDX-License-Identifier: MIT
//
// Compiler.cpp — wires the pipeline stages together behind one Facade call.

#include "hmr/pipeline/Compiler.hpp"

#include <filesystem>
#include <system_error>
#include <utility>

#include "hmr/ast/Ast.hpp"
#include "hmr/backend/Backend.hpp"
#include "hmr/backend/Linker.hpp"
#include "hmr/codegen/IrGenerator.hpp"
#include "hmr/optimizer/Optimizer.hpp"
#include "hmr/parser/Lexer.hpp"
#include "hmr/parser/Parser.hpp"

namespace hmr::pipeline {

namespace {

// Front-end product: an optimized ruleset plus the decision plan that orders
// emission by target header.
struct ParsedProgram {
    ast::Ruleset ruleset;
    opt::DecisionPlan plan;
};

// Lex → parse → (optional) optimize.
Result<ParsedProgram> frontEnd(const std::string& source,
                               const CompileOptions& opts) {
    auto toks = parser::Lexer(source).tokenize();
    if (!toks) return std::unexpected(std::move(toks.error()));

    parser::Parser parser(std::move(*toks));
    auto rs = parser.parse();
    if (!rs) return std::unexpected(std::move(rs.error()));

    ParsedProgram prog;
    prog.ruleset = std::move(*rs);
    if (opts.runOptimizer) {
        opt::Optimizer optimizer;
        auto report = optimizer.optimize(prog.ruleset);
        prog.plan = std::move(report.plan);
    }
    return prog;
}

// Optimized AST → textual LLVM IR (also fills `stats`).
Result<std::string> emitIR(const ParsedProgram& prog, const CompileOptions& opts,
                           codegen::IrModuleStats& stats) {
    codegen::IrGenerator gen;
    codegen::IrGenOptions gopts;
    gopts.moduleId = opts.moduleId;
    gopts.targetTriple = opts.targetTriple;
    gopts.emitComments = opts.emitComments;
    return gen.generateIR(prog.ruleset, prog.plan, gopts, &stats);
}

}  // namespace

Result<std::string> Compiler::compileToIR(const std::string& source,
                                          const CompileOptions& opts) {
    auto prog = frontEnd(source, opts);
    if (!prog) return std::unexpected(std::move(prog.error()));
    codegen::IrModuleStats stats;
    return emitIR(*prog, opts, stats);
}

Result<CompileResult> Compiler::compileToFile(const std::string& source,
                                              const CompileOptions& opts) {
    auto prog = frontEnd(source, opts);
    if (!prog) return std::unexpected(std::move(prog.error()));

    codegen::IrModuleStats stats;
    auto ir = emitIR(*prog, opts, stats);
    if (!ir) return std::unexpected(std::move(ir.error()));
    if (opts.irOut) *opts.irOut = *ir;

    backend::BackendOptions bopts;
    bopts.optLevel = opts.optLevel;
    bopts.targetTriple = opts.targetTriple;
    bopts.cpu = opts.cpu;
    bopts.features = opts.features;
    backend::Backend backend;
    auto obj = backend.compileToObject(*ir, bopts);
    if (!obj) return std::unexpected(std::move(obj.error()));

    backend::LinkOptions lopts;
    lopts.outputPath = opts.outputPath;
    lopts.driver = opts.linkerDriver;
    lopts.stripDebug = opts.stripDebug;
    backend::Linker linker;
    auto linked = linker.linkSharedObject(*obj, lopts);
    if (!linked) return std::unexpected(std::move(linked.error()));

    std::error_code ec;
    const auto size = std::filesystem::file_size(opts.outputPath, ec);

    CompileResult result;
    result.outputPath = opts.outputPath;
    result.byteSize = ec ? 0 : static_cast<std::size_t>(size);
    result.objectBytes = obj->bytes.size();
    result.moduleName = stats.moduleName;
    result.numHeaderRules = stats.numHeaderRules;
    result.numSlots = stats.numSlots;
    result.numRegexes = stats.numRegexes;
    result.triple = obj->triple;
    return result;
}

}  // namespace hmr::pipeline
