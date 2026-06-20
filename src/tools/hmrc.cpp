// SPDX-License-Identifier: MIT
//
// hmrc — the HMR compiler driver / CLI (Facade entry point).
//
// Subcommands:
//   parse        <file>                  lex + parse, report diagnostics
//   dump-ast     <file>                  parse and pretty-print the AST
//   optimize     <file>                  parse + run the optimizer, show report
//   check-samples <dir>                  parse every *.hmr (invalid_* must fail)
//   compile      <file> [-o out.so]      full pipeline to a native module (LLVM)
//
// The `compile` subcommand requires the LLVM-backed code generator and is
// compiled in only when HMR_HAVE_LLVM is defined. The front-end subcommands
// always work, so the tool is useful even in a no-LLVM build.

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

#include "hmr/ast/AstVisitor.hpp"
#include "hmr/optimizer/Optimizer.hpp"
#include "hmr/parser/Lexer.hpp"
#include "hmr/parser/Parser.hpp"

#if HMR_HAVE_LLVM
#include "hmr/pipeline/Compiler.hpp"
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
                 "  compile <file> [-o out.so]\n"
#endif
    );
    return 2;
}

std::optional<std::string> readFile(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return std::nullopt;
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

// Lex + parse a file, printing diagnostics (and warnings). Returns the ruleset,
// or nullopt on a lex/parse error.
std::optional<hmr::ast::Ruleset> parseFile(const std::string& path,
                                           bool quiet = false) {
    auto src = readFile(path);
    if (!src) {
        std::fprintf(stderr, "error: cannot read '%s'\n", path.c_str());
        return std::nullopt;
    }
    auto toks = hmr::parser::Lexer(*src).tokenize();
    if (!toks) {
        if (!quiet)
            std::fprintf(stderr, "%s: %s\n", path.c_str(),
                         toks.error().format().c_str());
        return std::nullopt;
    }
    hmr::parser::Parser parser(std::move(*toks));
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

int cmdParse(const std::string& path) {
    auto rs = parseFile(path);
    if (!rs) return 1;
    std::printf("%s: ok — %zu header-rule(s)\n", path.c_str(),
                rs->headerRules.size());
    return 0;
}

int cmdDumpAst(const std::string& path) {
    auto rs = parseFile(path);
    if (!rs) return 1;
    std::fputs(hmr::ast::toHmrText(*rs).c_str(), stdout);
    return 0;
}

int cmdOptimize(const std::string& path) {
    auto rs = parseFile(path);
    if (!rs) return 1;
    hmr::opt::Optimizer opt;
    auto report = opt.optimize(*rs);
    std::printf("optimization report (%u iteration(s)):\n", report.iterations);
    for (const auto& p : report.passes)
        std::printf("  %-24.*s %zu change(s)\n",
                    static_cast<int>(p.name.size()), p.name.data(), p.changes);
    std::printf("  decision groups: %zu\n", report.plan.size());
    std::puts("--- optimized AST ---");
    std::fputs(hmr::ast::toHmrText(*rs).c_str(), stdout);
    return 0;
}

// ctest helper: every *.hmr in <dir> must parse, except files named invalid_*
// which must fail. Exit nonzero if any expectation is violated.
int cmdCheckSamples(const std::string& dir) {
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
        bool expectFail = p.filename().string().rfind("invalid_", 0) == 0;
        bool ok = parseFile(p.string(), /*quiet=*/true).has_value();
        if (ok == expectFail) {
            std::printf("  FAIL %s (expected %s, got %s)\n",
                        p.filename().c_str(), expectFail ? "parse-error" : "success",
                        ok ? "success" : "parse-error");
            ++failures;
        } else {
            std::printf("  ok   %s (%s)\n", p.filename().c_str(),
                        expectFail ? "rejected" : "parsed");
        }
    }
    std::printf("%zu sample(s) checked, %d failure(s)\n", paths.size(), failures);
    return failures == 0 ? 0 : 1;
}

#if HMR_HAVE_LLVM
int cmdCompile(const std::string& path, const std::string& out) {
    auto src = readFile(path);
    if (!src) {
        std::fprintf(stderr, "error: cannot read '%s'\n", path.c_str());
        return 1;
    }
    hmr::pipeline::Compiler compiler;
    hmr::pipeline::CompileOptions opts;
    opts.outputPath = out;
    auto result = compiler.compileToFile(*src, opts);
    if (!result) {
        std::fprintf(stderr, "%s: %s\n", path.c_str(),
                     result.error().format().c_str());
        return 1;
    }
    std::printf("wrote %s (%zu bytes)\n", out.c_str(), result->byteSize);
    return 0;
}
#endif

}  // namespace

int main(int argc, char** argv) {
    std::vector<std::string> args(argv + 1, argv + argc);
    if (args.empty()) return usage();
    const std::string& cmd = args[0];

    if (cmd == "parse" && args.size() == 2) return cmdParse(args[1]);
    if (cmd == "dump-ast" && args.size() == 2) return cmdDumpAst(args[1]);
    if (cmd == "optimize" && args.size() == 2) return cmdOptimize(args[1]);
    if (cmd == "check-samples" && args.size() == 2) return cmdCheckSamples(args[1]);

#if HMR_HAVE_LLVM
    if (cmd == "compile" && args.size() >= 2) {
        std::string out = "a.so";
        for (std::size_t i = 2; i + 1 < args.size(); ++i)
            if (args[i] == "-o") out = args[i + 1];
        return cmdCompile(args[1], out);
    }
#endif

    return usage();
}
