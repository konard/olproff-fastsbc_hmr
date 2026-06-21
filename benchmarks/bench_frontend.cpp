// SPDX-License-Identifier: MIT
//
// bench_frontend.cpp — the LLVM-free half of the benchmark suite.
//
// It measures the two stages that need no code generator:
//   * front-end throughput — parse (ANTLR) + optimize, in ms/compile, the part
//     of the issue's "< 80 ms compile" budget that the LLVM backend then adds to,
//   * the interpreter apply baseline — ns/packet for the tree-walking oracle,
//     the reference the compiled module is measured against in bench_pipeline.
//
// Establishing the interpreter baseline here means it is available even in a
// no-LLVM build (the interpreter and front-end have no LLVM dependency), and it
// is the same number bench_pipeline divides the compiled apply into for the
// review-required "comparison with the interpreter".

#include <cstdio>
#include <string>

#include "bench_common.hpp"
#include "hmr/interp/interpreter.hpp"
#include "hmr/optimizer/optimizer.hpp"
#include "hmr/parser/parser.hpp"
#include "hmr/runtime/context.hpp"
#include "hmr/runtime/sip_message.hpp"

namespace {

// Time parse + optimize over `iters` runs, returning ms per front-end compile.
double time_frontend(const std::string& src, int iters) {
    const auto t0 = bench::Clock::now();
    for (int i = 0; i < iters; ++i) {
        auto rs = hmr::parser::Parser::parse_string(src);
        if (!rs) {
            std::fprintf(stderr, "parse failed: %s\n",
                         rs.error().format().c_str());
            return -1;
        }
        hmr::opt::Optimizer opt;
        opt.optimize(*rs);
    }
    const auto t1 = bench::Clock::now();
    return std::chrono::duration<double, std::milli>(t1 - t0).count() / iters;
}

}  // namespace

int main() {
    constexpr int kRules = 20;
    const std::string src = bench::make_ruleset(kRules);

    std::printf("=== HMR front-end benchmarks (%d rules) ===\n", kRules);

    const double fe_ms = time_frontend(src, 200);
    if (fe_ms < 0) return 1;
    std::printf("front-end (parse+optimize) : %8.3f ms/compile  "
                "(part of the < 80 ms total)\n",
                fe_ms);

    // Interpreter apply baseline over the arena SIP model.
    auto rs = hmr::parser::Parser::parse_string(src);
    if (!rs) {
        std::fprintf(stderr, "parse failed: %s\n", rs.error().format().c_str());
        return 1;
    }
    hmr::interp::Interpreter interp(*rs);

    const std::string raw = bench::make_packet(kRules);
    const HmrSipMsg base = HmrSipMsg::parse(raw);
    auto ctx = hmr::runtime::make_context(interp.info());
    bench::set_all_vars(ctx);

    const double interp_ns = bench::time_apply(
        base, ctx,
        [&](HmrSipMsg* m, HmrContext* c) { return interp.apply(m, c); }, 200000);
    std::printf("interpreter apply          : %8.1f ns/packet   "
                "(baseline; compiled is faster)\n",
                interp_ns);
    std::printf("\nThe compiled-module figures and the compiled-vs-interpreter /\n"
                "vs-GCC comparisons are reported by bench_pipeline (LLVM builds).\n");
    return 0;
}
