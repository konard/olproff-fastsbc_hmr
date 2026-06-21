// SPDX-License-Identifier: MIT
//
// probe.cpp — per-stage micro-breakdown of the interpreter apply, used to find
// where the per-packet time goes. It dumps the ABI struct sizes, then times the
// message copy, a single get_header, a full interpreter apply, and the O(1)
// context reset in isolation. The finding it documents: get_header (the linear
// header scan) dominates apply, so the data-path cost tracks the header-lookup
// loop, not the rule logic — see the iequals hot-path fix in
// src/runtime/sip_message.cpp.
//
// Not part of the Meson build (a standalone probe, like the other experiments).
// It reuses the benchmark fixtures, so put benchmarks/ on the include path:
//
//   g++ -std=c++23 -O2 -Iinclude -Ibenchmarks experiments/probe.cpp \
//       $(meson introspect builddir --targets | ...) -o experiments/probe
//
// In practice it is easiest to link it against the same objects bench_frontend
// uses (front-end + interpreter + runtime).

#include <cstdio>
#include <chrono>
#include "bench_common.hpp"
#include "hmr/interp/interpreter.hpp"
#include "hmr/parser/parser.hpp"
#include "hmr/runtime/context.hpp"
#include "hmr/runtime/sip_message.hpp"
using Clock = std::chrono::steady_clock;
int main(){
  std::printf("HMR_MAX_HEADERS=%d\n", (int)HMR_MAX_HEADERS);
  std::printf("sizeof(HmrSipMsg)=%zu  sizeof(HmrContext)=%zu  sizeof(HmrArena)=%zu\n",
              sizeof(HmrSipMsg), sizeof(HmrContext), sizeof(HmrArena));
  const std::string raw = bench::make_packet(20);
  const HmrSipMsg base = HmrSipMsg::parse(raw);
  const int N=200000;
  volatile uint32_t sink=0;
  // (1) just the copy
  { auto t0=Clock::now();
    for(int i=0;i<N;++i){ HmrSipMsg m=base; sink+=m.num_headers; }
    auto t1=Clock::now();
    std::printf("copy msg only      : %8.1f ns\n", std::chrono::duration<double,std::nano>(t1-t0).count()/N); }
  // (2) copy + get_header
  { auto t0=Clock::now();
    for(int i=0;i<N;++i){ HmrSipMsg m=base; sink+=m.get_header("X-Hop-19").len; }
    auto t1=Clock::now();
    std::printf("copy + get_header  : %8.1f ns\n", std::chrono::duration<double,std::nano>(t1-t0).count()/N); }
  // (3) full interpreter apply via time_apply
  const std::string src = bench::make_ruleset(20);
  auto rs = hmr::parser::Parser::parse_string(src);
  hmr::interp::Interpreter interp(*rs);
  auto ctx = hmr::runtime::make_context(interp.info());
  bench::set_all_vars(ctx);
  double ns = bench::time_apply(base, ctx, [&](HmrSipMsg*m,HmrContext*c){return interp.apply(m,c);}, N);
  std::printf("interp apply (full): %8.1f ns\n", ns);
  // (4) reset_for_apply only
  { auto t0=Clock::now();
    for(int i=0;i<N;++i){ ctx.reset_for_apply(); sink+=ctx.arena.used; }
    auto t1=Clock::now();
    std::printf("reset_for_apply    : %8.1f ns\n", std::chrono::duration<double,std::nano>(t1-t0).count()/N); }
  (void)sink; return 0;
}
