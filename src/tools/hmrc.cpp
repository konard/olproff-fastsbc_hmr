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

// hmrc — the HMR compiler driver / CLI (Facade entry point).
//
// Subcommands:
//   parse        <file>                  parse, report diagnostics
//   dump-ast     <file>                  parse and pretty-print the AST
//   optimize     <file>                  parse + run the optimizer, show report
//   check-samples <dir>                  parse every *.hmr (invalid_* must fail)
//   dump-ir      <file> [--opt]          emit LLVM IR (before / after -O3) (LLVM)
//   compile      <file> [-o out.so]      full pipeline to a native module (LLVM)
//
// The `dump-ir` and `compile` subcommands require the LLVM-backed code generator
// and are compiled in only when HMR_HAVE_LLVM is defined. The front-end
// subcommands always work, so the tool is useful even in a no-LLVM build.

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

#include "hmr/ast/ast_visitor.hpp"
#include "hmr/optimizer/optimizer.hpp"
#include "hmr/parser/parser.hpp"

#if HMR_HAVE_LLVM
#include "hmr/backend/backend.hpp"
#include "hmr/pipeline/compiler.hpp"
#endif

namespace {

namespace fs = std::filesystem;

int usage() {
    std::fprintf(stderr,
                 "usage: hmrc <command> [args]\n"
                 "  parse <file>\n"
                 "  dump-ast <file>\n"
                 "  optimize <file>\n"
                 "  check-samples <dir>\n"
#if HMR_HAVE_LLVM
                 "  dump-ir <file> [--opt]\n"
                 "  compile <file> [-o out.so]\n"
#endif
    );
    return 2;
}

std::optional<std::string> read_file(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return std::nullopt;
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

// Parse a file, printing diagnostics (and warnings). Returns the ruleset, or
// nullopt on a parse error.
std::optional<hmr::ast::Ruleset> parse_file(const std::string& path,
                                            bool quiet = false) {
    auto src = read_file(path);
    if (!src) {
        std::fprintf(stderr, "error: cannot read '%s'\n", path.c_str());
        return std::nullopt;
    }
    hmr::parser::Parser parser(*src);
    auto rs = parser.parse();
    if (!rs) {
        if (!quiet)
            std::fprintf(stderr, "%s: %s\n", path.c_str(),
                         rs.error().format().c_str());
        return std::nullopt;
    }
    if (!quiet)
        for (const auto& w : parser.warnings())
            std::fprintf(stderr, "%s: %s\n", path.c_str(), w.format().c_str());
    return std::move(*rs);
}

int cmd_parse(const std::string& path) {
    auto rs = parse_file(path);
    if (!rs) return 1;
    std::printf("%s: ok — %zu header-rule(s)\n", path.c_str(),
                rs->header_rules.size());
    return 0;
}

int cmd_dump_ast(const std::string& path) {
    auto rs = parse_file(path);
    if (!rs) return 1;
    std::fputs(hmr::ast::to_hmr_text(*rs).c_str(), stdout);
    return 0;
}

int cmd_optimize(const std::string& path) {
    auto rs = parse_file(path);
    if (!rs) return 1;
    hmr::opt::Optimizer opt;
    auto report = opt.optimize(*rs);
    std::printf("optimization report (%u iteration(s)):\n", report.iterations);
    for (const auto& p : report.passes)
        std::printf("  %-24.*s %zu change(s)\n",
                    static_cast<int>(p.name.size()), p.name.data(), p.changes);
    std::printf("  decision groups: %zu\n", report.plan.size());
    std::puts("--- optimized AST ---");
    std::fputs(hmr::ast::to_hmr_text(*rs).c_str(), stdout);
    return 0;
}

// ctest helper: every *.hmr in <dir> must parse, except files named invalid_*
// which must fail. Exit nonzero if any expectation is violated.
int cmd_check_samples(const std::string& dir) {
    if (!fs::is_directory(dir)) {
        std::fprintf(stderr, "error: '%s' is not a directory\n", dir.c_str());
        return 2;
    }
    std::vector<fs::path> paths;
    for (const auto& e : fs::directory_iterator(dir))
        if (e.is_regular_file() && e.path().extension() == ".hmr")
            paths.push_back(e.path());
    std::sort(paths.begin(), paths.end());

    int failures = 0;
    for (const auto& p : paths) {
        bool expect_fail = p.filename().string().rfind("invalid_", 0) == 0;
        bool ok = parse_file(p.string(), /*quiet=*/true).has_value();
        if (ok == expect_fail) {
            std::printf("  FAIL %s (expected %s, got %s)\n",
                        p.filename().c_str(), expect_fail ? "parse-error" : "success",
                        ok ? "success" : "parse-error");
            ++failures;
        } else {
            std::printf("  ok   %s (%s)\n", p.filename().c_str(),
                        expect_fail ? "rejected" : "parsed");
        }
    }
    std::printf("%zu sample(s) checked, %d failure(s)\n", paths.size(), failures);
    return failures == 0 ? 0 : 1;
}

#if HMR_HAVE_LLVM
// Emit the textual LLVM IR for a ruleset. Without --opt this is the IR straight
// from the generator (before optimization); with --opt the same IR is run
// through the backend's HMR + -O3 pipeline (after optimization). The pair is the
// "LLVM IR examples (before/after optimization)" documentation artifact.
int cmd_dump_ir(const std::string& path, bool optimize) {
    auto src = read_file(path);
    if (!src) {
        std::fprintf(stderr, "error: cannot read '%s'\n", path.c_str());
        return 1;
    }
    hmr::pipeline::Compiler compiler;
    auto ir = compiler.compile_to_ir(*src);
    if (!ir) {
        std::fprintf(stderr, "%s: %s\n", path.c_str(),
                     ir.error().format().c_str());
        return 1;
    }
    if (!optimize) {
        std::fputs(ir->c_str(), stdout);
        return 0;
    }
    hmr::backend::Backend backend;
    auto opt = backend.optimize_ir(*ir);
    if (!opt) {
        std::fprintf(stderr, "%s: %s\n", path.c_str(),
                     opt.error().format().c_str());
        return 1;
    }
    std::fputs(opt->c_str(), stdout);
    return 0;
}

int cmd_compile(const std::string& path, const std::string& out) {
    auto src = read_file(path);
    if (!src) {
        std::fprintf(stderr, "error: cannot read '%s'\n", path.c_str());
        return 1;
    }
    hmr::pipeline::Compiler compiler;
    hmr::pipeline::CompileOptions opts;
    opts.output_path = out;
    auto result = compiler.compile_to_file(*src, opts);
    if (!result) {
        std::fprintf(stderr, "%s: %s\n", path.c_str(),
                     result.error().format().c_str());
        return 1;
    }
    std::printf("wrote %s (%zu bytes)\n", out.c_str(), result->byte_size);
    return 0;
}
#endif

}  // namespace

int main(int argc, char** argv) {
    std::vector<std::string> args(argv + 1, argv + argc);
    if (args.empty()) return usage();
    const std::string& cmd = args[0];

    if (cmd == "parse" && args.size() == 2) return cmd_parse(args[1]);
    if (cmd == "dump-ast" && args.size() == 2) return cmd_dump_ast(args[1]);
    if (cmd == "optimize" && args.size() == 2) return cmd_optimize(args[1]);
    if (cmd == "check-samples" && args.size() == 2) return cmd_check_samples(args[1]);

#if HMR_HAVE_LLVM
    if (cmd == "dump-ir" && args.size() >= 2) {
        bool optimize = false;
        for (std::size_t i = 2; i < args.size(); ++i)
            if (args[i] == "--opt" || args[i] == "-O") optimize = true;
        return cmd_dump_ir(args[1], optimize);
    }
    if (cmd == "compile" && args.size() >= 2) {
        std::string out = "a.so";
        for (std::size_t i = 2; i + 1 < args.size(); ++i)
            if (args[i] == "-o") out = args[i + 1];
        return cmd_compile(args[1], out);
    }
#endif

    return usage();
}
