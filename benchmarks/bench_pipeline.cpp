// SPDX-License-Identifier: MIT
//
// bench_pipeline.cpp — end-to-end benchmarks over a *real compiled module*, plus
// the two comparisons review #5 requires.
//
//   1. Full pipeline       — front-end + IR + O3 + emit + link, ms/compile and
//                            module size, against the issue's < 80 ms / < 50 KB.
//   2. Compiled apply      — ns/packet through the dlopen'd hmr_apply, against
//                            the 50-150 ns target.
//   3. vs the interpreter  — the same ruleset applied by the tree-walking oracle;
//                            the compiled path's speedup is the payoff of codegen.
//   4. vs the GCC approach — the counterfactual the issue rejects. One optimized
//                            AST is lowered to a .so two ways: directly to LLVM IR
//                            (this project) and to C++ source handed to g++ -O3
//                            -shared (the "generate C++ then invoke a compiler"
//                            design). We compare compile time (the thesis: emitting
//                            IR is far cheaper than spawning a second compiler) and
//                            apply time (the control: identical native speed).
//
// Numbers are reported, not asserted — they vary by host, and the compile figures
// fork external tools (ld for the LLVM path, g++ for the GCC path), so they
// include process-spawn latency. The GCC comparison degrades gracefully: if no
// C++ compiler is found it is skipped with a note.

#include <fcntl.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

#include "bench_common.hpp"
#include "hmr/backend/backend.hpp"
#include "hmr/backend/linker.hpp"
#include "hmr/codegen/cpp_generator.hpp"
#include "hmr/codegen/ir_generator.hpp"
#include "hmr/interp/interpreter.hpp"
#include "hmr/module/module_manager.hpp"
#include "hmr/optimizer/optimizer.hpp"
#include "hmr/parser/parser.hpp"
#include "hmr/pipeline/compiler.hpp"
#include "hmr/runtime/context.hpp"
#include "hmr/runtime/sip_message.hpp"

// Where the generated C++ finds hmr/runtime/hmr_runtime.h. Injected by Meson as
// the absolute in-tree include dir; falls back to a relative path otherwise.
#ifndef HMR_INCLUDE_DIR
#define HMR_INCLUDE_DIR "include"
#endif

extern char** environ;

namespace {

namespace fs = std::filesystem;
using bench::Clock;

// Discarded iterations run before each timed apply loop so the CPU has ramped to
// its steady frequency and the working set is hot — otherwise the first section
// measured after the heavy compile loops reads slow purely from a cold start.
constexpr int kWarmupIters = 5000;

// A per-run-unique scratch path with the given suffix.
fs::path temp_path(const char* suffix) {
    static int counter = 0;
    return fs::temp_directory_path() /
           ("hmr_bench_" + std::to_string(::getpid()) + "_" +
            std::to_string(counter++) + suffix);
}

std::size_t file_size_or_zero(const fs::path& p) {
    std::error_code ec;
    const auto n = fs::file_size(p, ec);
    return ec ? 0 : static_cast<std::size_t>(n);
}

// ---------------------------------------------------------------------------
// The GCC approach: write generated C++ to a temp file and drive a C++ compiler
// to a .so, exactly as a "generate C++ then compile it" design would. Mirrors
// linker.cpp's posix_spawn pattern. The .so leaves every hmr_rt_* undefined
// (resolved at dlopen against this process, which is linked with export_dynamic),
// identical to the LLVM-direct module. Returns false (with `err` set) if the
// compiler is missing or the compile fails.
// ---------------------------------------------------------------------------
bool compile_cpp_to_so(const std::string& cpp_source, const fs::path& out,
                       std::string& err) {
    const fs::path src = temp_path(".cpp");
    {
        std::ofstream o(src, std::ios::binary | std::ios::trunc);
        if (!o) {
            err = "cannot open temp .cpp for writing";
            return false;
        }
        o << cpp_source;
    }
    const fs::path log = temp_path(".log");

    std::string cxx;
    if (const char* e = std::getenv("HMR_CXX"); e && *e) cxx = e;
    if (cxx.empty()) cxx = "g++";

    std::vector<std::string> args = {cxx,
                                     "-std=c++23",
                                     "-O3",
                                     "-shared",
                                     "-fPIC",
                                     "-I",
                                     HMR_INCLUDE_DIR,
                                     src.string(),
                                     "-o",
                                     out.string()};
    std::vector<char*> argv;
    argv.reserve(args.size() + 1);
    for (auto& a : args) argv.push_back(const_cast<char*>(a.c_str()));
    argv.push_back(nullptr);

    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_addopen(&fa, STDOUT_FILENO, log.c_str(),
                                     O_WRONLY | O_CREAT | O_TRUNC, 0644);
    posix_spawn_file_actions_adddup2(&fa, STDOUT_FILENO, STDERR_FILENO);

    pid_t pid = 0;
    const int rc =
        posix_spawnp(&pid, cxx.c_str(), &fa, nullptr, argv.data(), environ);
    posix_spawn_file_actions_destroy(&fa);

    auto cleanup = [&] {
        std::error_code ec;
        fs::remove(src, ec);
        fs::remove(log, ec);
    };

    if (rc != 0) {
        err = "cannot spawn '" + cxx + "': " + std::strerror(rc);
        cleanup();
        return false;
    }

    int status = 0;
    if (::waitpid(pid, &status, 0) < 0) {
        err = std::string("waitpid failed: ") + std::strerror(errno);
        cleanup();
        return false;
    }
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        std::ifstream in(log, std::ios::binary);
        std::ostringstream ss;
        ss << in.rdbuf();
        err = "'" + cxx + "' failed:\n" + ss.str();
        cleanup();
        return false;
    }
    cleanup();
    return true;
}

// An optimized program: the AST after the optimizer plus its decision plan.
// Both back-ends consume this, so the vs-GCC comparison measures only code
// generation + toolchain, never the front-end (which is shared and identical).
struct Optimized {
    hmr::ast::Ruleset rs;
    hmr::opt::DecisionPlan plan;
};

std::optional<Optimized> parse_and_optimize(const std::string& src) {
    hmr::parser::Parser parser(src);
    auto rs = parser.parse();
    if (!rs) {
        std::fprintf(stderr, "parse failed: %s\n", rs.error().format().c_str());
        return std::nullopt;
    }
    Optimized o;
    o.rs = std::move(*rs);
    hmr::opt::Optimizer opt;
    auto report = opt.optimize(o.rs);
    o.plan = std::move(report.plan);
    return o;
}

// Full pipeline (source → .so) timing + the size of the written module.
double time_full_compile(const std::string& src, const fs::path& out, int iters,
                         hmr::pipeline::CompileResult* last) {
    hmr::pipeline::Compiler comp;
    hmr::pipeline::CompileOptions opts;
    opts.output_path = out.string();

    const auto t0 = Clock::now();
    for (int i = 0; i < iters; ++i) {
        auto r = comp.compile_to_file(src, opts);
        if (!r) {
            std::fprintf(stderr, "compile failed: %s\n",
                         r.error().format().c_str());
            return -1;
        }
        if (last) *last = *r;
    }
    const auto t1 = Clock::now();
    return std::chrono::duration<double, std::milli>(t1 - t0).count() / iters;
}

// LLVM-direct: optimized AST → IR → object → .so. The front-end is excluded so
// this is comparable to the GCC path below.
double time_llvm_direct(const Optimized& o, const fs::path& out, int iters,
                        std::size_t* bytes) {
    const auto t0 = Clock::now();
    for (int i = 0; i < iters; ++i) {
        hmr::codegen::IrGenerator gen;
        auto ir = gen.generate_ir(o.rs, o.plan);
        if (!ir) {
            std::fprintf(stderr, "ir gen failed: %s\n",
                         ir.error().format().c_str());
            return -1;
        }
        hmr::backend::Backend backend;
        auto obj = backend.compile_to_object(*ir);
        if (!obj) {
            std::fprintf(stderr, "backend failed: %s\n",
                         obj.error().format().c_str());
            return -1;
        }
        hmr::backend::LinkOptions lopts;
        lopts.output_path = out.string();
        hmr::backend::Linker linker;
        auto r = linker.link_shared_object(*obj, lopts);
        if (!r) {
            std::fprintf(stderr, "link failed: %s\n", r.error().format().c_str());
            return -1;
        }
    }
    const auto t1 = Clock::now();
    if (bytes) *bytes = file_size_or_zero(out);
    return std::chrono::duration<double, std::milli>(t1 - t0).count() / iters;
}

// GCC approach: optimized AST → C++ source → g++ -O3 -shared → .so. Returns -1
// and sets *available=false if no C++ compiler is present (skip, don't fail).
double time_gcc_approach(const Optimized& o, const fs::path& out, int iters,
                         std::size_t* bytes, bool* available) {
    *available = true;
    const auto t0 = Clock::now();
    for (int i = 0; i < iters; ++i) {
        hmr::codegen::CppGenerator gen;
        auto cpp = gen.generate_cpp(o.rs);
        if (!cpp) {
            std::fprintf(stderr, "cpp gen failed: %s\n",
                         cpp.error().format().c_str());
            return -1;
        }
        std::string err;
        if (!compile_cpp_to_so(*cpp, out, err)) {
            *available = false;
            std::fprintf(stderr, "  (GCC approach unavailable: %s)\n",
                         err.c_str());
            return -1;
        }
    }
    const auto t1 = Clock::now();
    if (bytes) *bytes = file_size_or_zero(out);
    return std::chrono::duration<double, std::milli>(t1 - t0).count() / iters;
}

// Load a module and measure its apply over the standard packet; -1 on failure.
double measure_apply(const fs::path& so, const HmrSipMsg& base, int iters) {
    hmr::module::ModuleManager mgr;
    auto mod = mgr.load(so.string());
    if (!mod) {
        std::fprintf(stderr, "load failed: %s\n", mod.error().format().c_str());
        return -1;
    }
    auto ctx = hmr::runtime::make_context((*mod)->info());
    bench::set_all_vars(ctx);
    auto apply = [&](HmrSipMsg* m, HmrContext* c) { return (*mod)->apply(m, c); };
    bench::time_apply(base, ctx, apply, kWarmupIters);  // ramp CPU/caches; discard
    return bench::time_apply(base, ctx, apply, iters);
}

// One matcher variant of the same workload, compiled and applied end-to-end. The
// ruleset and packet are built for `kind` (exact strcmp, std::regex, or the
// specialized IP-range test) but the logical work is identical across all three.
// Reports the compiled regex count (0 for exact/ip, one per rule for regex) and
// the apply ns/packet — the figure that shows std::regex is the slow path.
struct MatcherResult {
    double apply_ns;       // ns/packet, or < 0 on failure
    double compile_ms;
    unsigned num_regexes;
    std::size_t byte_size;
};

MatcherResult measure_matcher(bench::Match kind, int rules, int iters) {
    MatcherResult out{-1.0, 0.0, 0, 0};
    const std::string src = bench::make_ruleset(rules, kind);
    const std::string raw = bench::make_packet(rules, kind);
    const HmrSipMsg base = HmrSipMsg::parse(raw);

    const fs::path so = temp_path(".so");
    hmr::pipeline::CompileResult cr;
    const double ms = time_full_compile(src, so, 5, &cr);
    if (ms < 0) return out;
    out.compile_ms = ms;
    out.num_regexes = cr.num_regexes;
    out.byte_size = cr.byte_size;
    out.apply_ns = measure_apply(so, base, iters);

    std::error_code ec;
    fs::remove(so, ec);
    return out;
}

}  // namespace

int main() {
    constexpr int kRules = 20;
    constexpr int kApplyIters = 200000;
    const std::string src = bench::make_ruleset(kRules);
    const std::string raw = bench::make_packet(kRules);
    const HmrSipMsg base = HmrSipMsg::parse(raw);

    std::printf("=== HMR pipeline benchmarks (%d rules, 1 packet) ===\n\n",
                kRules);

    // ---- 1. Full pipeline compile + module size --------------------------
    const fs::path so = temp_path(".so");
    hmr::pipeline::CompileResult cr;
    const double full_ms = time_full_compile(src, so, 10, &cr);
    if (full_ms < 0) return 1;
    std::printf("[1] full pipeline\n");
    std::printf("    compile (src->.so)     : %8.3f ms/compile  (target < 80 ms)\n",
                full_ms);
    std::printf("    module size            : %8.2f KB          (target < 50 KB)\n",
                static_cast<double>(cr.byte_size) / 1024.0);
    std::printf("    header-rules/slots/rgx : %u / %u / %u\n\n", cr.num_header_rules,
                cr.num_slots, cr.num_regexes);

    // ---- 2. Compiled apply ----------------------------------------------
    const double compiled_ns = measure_apply(so, base, kApplyIters);
    if (compiled_ns < 0) {
        std::error_code ec;
        fs::remove(so, ec);
        return 1;
    }
    std::printf("[2] compiled apply         : %8.1f ns/packet   (target 50-150 ns)\n\n",
                compiled_ns);

    // ---- 3. vs the interpreter ------------------------------------------
    auto rs = hmr::parser::Parser::parse_string(src);
    if (!rs) {
        std::fprintf(stderr, "parse failed: %s\n", rs.error().format().c_str());
        std::error_code ec;
        fs::remove(so, ec);
        return 1;
    }
    hmr::interp::Interpreter interp(*rs);
    auto ictx = hmr::runtime::make_context(interp.info());
    bench::set_all_vars(ictx);
    auto interp_apply = [&](HmrSipMsg* m, HmrContext* c) {
        return interp.apply(m, c);
    };
    bench::time_apply(base, ictx, interp_apply, kWarmupIters);  // discard
    const double interp_ns =
        bench::time_apply(base, ictx, interp_apply, kApplyIters);
    std::printf("[3] comparison with the interpreter\n");
    std::printf("    interpreter apply      : %8.1f ns/packet\n", interp_ns);
    std::printf("    compiled apply         : %8.1f ns/packet\n", compiled_ns);
    if (compiled_ns > 0)
        std::printf("    speedup (compiled)     : %8.2fx\n\n",
                    interp_ns / compiled_ns);

    // ---- 4. vs the GCC approach -----------------------------------------
    std::printf("[4] comparison with the GCC approach"
                " (one optimized AST -> .so, two back-ends)\n");
    auto opt = parse_and_optimize(src);
    if (!opt) {
        std::error_code ec;
        fs::remove(so, ec);
        return 1;
    }
    const fs::path llvm_so = temp_path(".so");
    const fs::path gcc_so = temp_path(".so");

    std::size_t llvm_bytes = 0;
    const double llvm_ms = time_llvm_direct(*opt, llvm_so, 10, &llvm_bytes);

    std::size_t gcc_bytes = 0;
    bool gcc_available = false;
    const double gcc_ms =
        time_gcc_approach(*opt, gcc_so, 5, &gcc_bytes, &gcc_available);

    if (llvm_ms >= 0) {
        const double llvm_apply = measure_apply(llvm_so, base, kApplyIters);
        std::printf("    LLVM-direct  compile   : %8.3f ms   size %6.2f KB"
                    "   apply %7.1f ns/packet\n",
                    llvm_ms, static_cast<double>(llvm_bytes) / 1024.0,
                    llvm_apply);
    }
    if (gcc_available && gcc_ms >= 0) {
        const double gcc_apply = measure_apply(gcc_so, base, kApplyIters);
        std::printf("    GCC (g++)    compile   : %8.3f ms   size %6.2f KB"
                    "   apply %7.1f ns/packet\n",
                    gcc_ms, static_cast<double>(gcc_bytes) / 1024.0, gcc_apply);
        if (llvm_ms > 0)
            std::printf("    => emitting IR directly compiles %.1fx faster than"
                        " spawning g++, at parity run-time speed.\n",
                        gcc_ms / llvm_ms);
    } else {
        std::printf("    GCC (g++)    compile   :   skipped (no C++ compiler on"
                    " PATH / $HMR_CXX)\n");
    }
    std::printf("\n");

    // ---- 5. specialized matchers vs std::regex (review #2) --------------
    // The identical topology-hiding workload, compiled three ways: an exact
    // strcmp, a std::regex, and a specialized IP-range test. Same logical work
    // (rewrite the host when it matches); only the element-rule's comparison
    // engine differs. Routing every match-val-type through std::regex — what the
    // first cut did — is the slow path review #2 called out; exact and ip should
    // beat it without compiling any regex at all.
    std::printf("[5] match-val-type engines (same %d-rule workload, three ways)\n",
                kRules);
    const MatcherResult exact = measure_matcher(bench::Match::Exact, kRules,
                                                kApplyIters);
    const MatcherResult regex = measure_matcher(bench::Match::Regex, kRules,
                                                kApplyIters);
    const MatcherResult ip = measure_matcher(bench::Match::Ip, kRules,
                                             kApplyIters);
    std::printf("    exact (strcmp)         : %8.1f ns/packet   %u regex(es)\n",
                exact.apply_ns, exact.num_regexes);
    std::printf("    ip-range (specialized) : %8.1f ns/packet   %u regex(es)\n",
                ip.apply_ns, ip.num_regexes);
    std::printf("    regex (std::regex)     : %8.1f ns/packet   %u regex(es)\n",
                regex.apply_ns, regex.num_regexes);
    if (exact.apply_ns > 0 && regex.apply_ns > 0)
        std::printf("    => exact is %.1fx faster than std::regex; ip is %.1fx"
                    " faster.\n",
                    regex.apply_ns / exact.apply_ns,
                    regex.apply_ns / ip.apply_ns);

    std::error_code ec;
    fs::remove(so, ec);
    fs::remove(llvm_so, ec);
    fs::remove(gcc_so, ec);
    return 0;
}
