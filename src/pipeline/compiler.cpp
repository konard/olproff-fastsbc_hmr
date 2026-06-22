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

// compiler.cpp — wires the pipeline stages together behind one Facade call.

#include "hmr/pipeline/compiler.hpp"

#include <filesystem>
#include <system_error>
#include <utility>

#include "hmr/ast/ast.hpp"
#include "hmr/backend/backend.hpp"
#include "hmr/backend/linker.hpp"
#include "hmr/codegen/ir_generator.hpp"
#include "hmr/optimizer/optimizer.hpp"
#include "hmr/parser/parser.hpp"

namespace hmr::pipeline {

namespace {

// Front-end product: an optimized ruleset plus the decision plan that orders
// emission by target header.
struct ParsedProgram {
    ast::Ruleset ruleset;
    opt::DecisionPlan plan;
};

// Parse (ANTLR) → (optional) optimize. The ANTLR-generated lexer is internal to
// the Parser, so the Facade only drives parse() directly.
Result<ParsedProgram> front_end(const std::string& source,
                                const CompileOptions& opts) {
    parser::Parser parser(source);
    auto rs = parser.parse();
    if (!rs) return std::unexpected(std::move(rs.error()));

    ParsedProgram prog;
    prog.ruleset = std::move(*rs);
    if (opts.run_optimizer) {
        opt::Optimizer optimizer;
        auto report = optimizer.optimize(prog.ruleset);
        prog.plan = std::move(report.plan);
    }
    return prog;
}

// Optimized AST → textual LLVM IR (also fills `stats`).
Result<std::string> emit_ir(const ParsedProgram& prog, const CompileOptions& opts,
                            codegen::IrModuleStats& stats) {
    codegen::IrGenerator gen;
    codegen::IrGenOptions gopts;
    gopts.module_id = opts.module_id;
    gopts.target_triple = opts.target_triple;
    gopts.emit_comments = opts.emit_comments;
    return gen.generate_ir(prog.ruleset, prog.plan, gopts, &stats);
}

}  // namespace

Result<std::string> Compiler::compile_to_ir(const std::string& source,
                                            const CompileOptions& opts) {
    auto prog = front_end(source, opts);
    if (!prog) return std::unexpected(std::move(prog.error()));
    codegen::IrModuleStats stats;
    return emit_ir(*prog, opts, stats);
}

Result<CompileResult> Compiler::compile_to_file(const std::string& source,
                                                const CompileOptions& opts) {
    auto prog = front_end(source, opts);
    if (!prog) return std::unexpected(std::move(prog.error()));

    codegen::IrModuleStats stats;
    auto ir = emit_ir(*prog, opts, stats);
    if (!ir) return std::unexpected(std::move(ir.error()));
    if (opts.ir_out) *opts.ir_out = *ir;

    backend::BackendOptions bopts;
    bopts.opt_level = opts.opt_level;
    bopts.target_triple = opts.target_triple;
    bopts.cpu = opts.cpu;
    bopts.features = opts.features;
    backend::Backend backend;
    auto obj = backend.compile_to_object(*ir, bopts);
    if (!obj) return std::unexpected(std::move(obj.error()));

    backend::LinkOptions lopts;
    lopts.output_path = opts.output_path;
    lopts.driver = opts.linker_driver;
    lopts.strip_debug = opts.strip_debug;
    backend::Linker linker;
    auto linked = linker.link_shared_object(*obj, lopts);
    if (!linked) return std::unexpected(std::move(linked.error()));

    std::error_code ec;
    const auto size = std::filesystem::file_size(opts.output_path, ec);

    CompileResult result;
    result.output_path = opts.output_path;
    result.byte_size = ec ? 0 : static_cast<std::size_t>(size);
    result.object_bytes = obj->bytes.size();
    result.module_name = stats.module_name;
    result.num_header_rules = stats.num_header_rules;
    result.num_slots = stats.num_slots;
    result.num_regexes = stats.num_regexes;
    result.triple = obj->triple;
    return result;
}

}  // namespace hmr::pipeline
