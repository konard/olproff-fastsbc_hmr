// SPDX-License-Identifier: MIT
//
// bench_frontend.cpp — dependency-free micro-benchmarks for the LLVM-free parts
// of the pipeline: front-end (lex + parse + optimize) compilation throughput and
// the per-packet runtime apply path (the callbacks generated code invokes).
//
// These establish a baseline for the targets in the issue (<80ms compile,
// <200ns/packet) without needing the code generator. The end-to-end benchmark
// over a real compiled .so lives in bench_pipeline.cpp (LLVM builds only).

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <string>

#include "hmr/optimizer/Optimizer.hpp"
#include "hmr/parser/Parser.hpp"
#include "hmr/runtime/Runtime.hpp"
#include "hmr/runtime/SipMessage.hpp"
#include "hmr/runtime/hmr_runtime.h"

namespace {

using Clock = std::chrono::steady_clock;

HmrStr S(std::string_view s) {
    return HmrStr{s.data(), static_cast<std::uint32_t>(s.size())};
}

// A representative ruleset (~20 header rules) for the compile benchmark.
std::string makeRuleset(int rules) {
    std::string s = "sip-manipulation HeavyManip\n";
    for (int i = 0; i < rules; ++i) {
        s += "        header-rule\n";
        s += "                name  r" + std::to_string(i) + "\n";
        s += "                header-name  X-Custom-" + std::to_string(i) + "\n";
        s += "                action  manipulate\n";
        s += "                comparison-type  pattern-rule\n";
        s += "                match-value  ^sip:.*@.*$\n";
        s += "                element-rule\n";
        s += "                        type  uri-user\n";
        s += "                        action  replace\n";
        s += "                        new-value  anon$" + std::to_string(i) + "\n";
    }
    return s;
}

double timeCompile(const std::string& src, int iters) {
    auto t0 = Clock::now();
    for (int i = 0; i < iters; ++i) {
        auto rs = hmr::parser::Parser::parseString(src);
        if (!rs) {
            std::fprintf(stderr, "parse failed: %s\n", rs.error().message().c_str());
            return -1;
        }
        hmr::opt::Optimizer opt;
        opt.optimize(*rs);
    }
    auto t1 = Clock::now();
    return std::chrono::duration<double, std::milli>(t1 - t0).count() / iters;
}

// Simulate what a compiled ~20-rule module does per packet, using the same
// runtime callbacks the generated code calls. Measures steady-state ns/packet.
double timeApply(int iters) {
    HmrRegexEntry entries[1] = {{"sip:([^@]+)@([^:;>]+)", 0}};
    HmrModuleInfo info{HMR_ABI_VERSION, "bench", /*slots=*/4, 1, entries};
    auto ctx = hmr::runtime::makeContext(info);
    ctx.setVar(HMR_VAR_LOCAL_IP, "10.0.0.1");

    HmrSipMsg base;
    base.isRequest = true;
    base.method = "INVITE";
    base.requestUri = "sip:bob@example.com";
    base.addHeader("Via", "SIP/2.0/UDP 1.2.3.4:5060;branch=z9hG4bK1");
    base.addHeader("From", "\"Alice\" <sip:alice@internal.local>;tag=12345");
    base.addHeader("To", "<sip:bob@example.com>");
    base.addHeader("Call-ID", "abc@host");
    base.addHeader("CSeq", "1 INVITE");
    base.addHeader("Server", "Internal-SBC/2.1");

    volatile std::uint32_t sink = 0;
    auto t0 = Clock::now();
    for (int i = 0; i < iters; ++i) {
        HmrSipMsg msg = base;  // copy so each packet starts identical
        ctx.resetForApply();

        // ~20 header-rule body: read From, rewrite its user to a built value,
        // topology-hide by deleting Server, tag a custom header.
        HmrStr from = hmr_rt_get_header(&msg, S("From"));
        if (hmr_rt_regex_match(&ctx, 0, from)) {
            hmr_rt_val_reset(&ctx);
            hmr_rt_val_append_lit(&ctx, S("anonymous"));
            HmrStr user = hmr_rt_val_finish(&ctx);
            HmrStr rebuilt = hmr_rt_uri_set(&ctx, from, HMR_URI_USER, user);
            hmr_rt_set_header(&msg, S("From"), rebuilt);
        }
        hmr_rt_delete_header(&msg, S("Server"));
        hmr_rt_val_reset(&ctx);
        hmr_rt_val_append_lit(&ctx, S("realm="));
        hmr_rt_val_append_var(&ctx, HMR_VAR_LOCAL_IP);
        hmr_rt_set_header(&msg, S("X-Realm"), hmr_rt_val_finish(&ctx));

        sink += static_cast<std::uint32_t>(hmr_rt_get_header(&msg, S("From")).len);
    }
    auto t1 = Clock::now();
    (void)sink;
    return std::chrono::duration<double, std::nano>(t1 - t0).count() / iters;
}

}  // namespace

int main() {
    std::string rs20 = makeRuleset(20);
    double compileMs = timeCompile(rs20, 200);
    std::printf("compile  (20 rules)  : %8.3f ms/compile  (target < 80 ms)\n",
                compileMs);

    double applyNs = timeApply(200000);
    std::printf("apply    (per packet): %8.1f ns/packet   (target < 200 ns*)\n",
                applyNs);
    std::printf("  * apply uses std::string SIP model + std::regex; the compiled\n"
                "    .so path with arena buffers is measured by bench_pipeline.\n");
    return 0;
}
