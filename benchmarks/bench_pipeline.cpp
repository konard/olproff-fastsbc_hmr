// SPDX-License-Identifier: MIT
//
// bench_pipeline.cpp — end-to-end benchmarks over a *real compiled module*.
//
// Unlike bench_frontend (which times the LLVM-free stages and a hand-written
// apply loop), this drives the whole pipeline: it compiles a ~20-rule ruleset to
// a native .so, dlopen's it through the ModuleManager, and measures both the
// one-shot compilation cost and the steady-state per-packet cost of calling the
// generated hmr_apply. These map directly onto the issue's targets:
//
//   * compilation   < 80 ms      (front-end + IR + O3 + emit + link)
//   * per packet    < 200 ns     (20 rules, zero heap traffic in steady state)
//   * module size   < 50 KB
//
// Numbers are reported, not asserted: they vary by host, and the linker step
// forks `cc`, so the compile figure includes process-spawn latency.

#include <unistd.h>  // getpid

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <string>

#include "hmr/module/ModuleManager.hpp"
#include "hmr/pipeline/Compiler.hpp"
#include "hmr/runtime/Runtime.hpp"
#include "hmr/runtime/SipMessage.hpp"
#include "hmr/runtime/hmr_runtime.h"

namespace {

namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;

// A representative ~N-rule ruleset: each rule reads a distinct header, matches
// its URI host against a regex, and rewrites the host to $LOCAL_IP — the same
// get_header → uri_get → regex_match → uri_set → set_header chain a real
// topology-hiding/normalization module runs per packet.
std::string makeRuleset(int rules) {
    std::string s = "sip-manipulation BenchPipeline\n";
    for (int i = 0; i < rules; ++i) {
        const std::string n = std::to_string(i);
        s += "        header-rule\n";
        s += "                name  rewrite" + n + "\n";
        s += "                header-name  X-Hop-" + n + "\n";
        s += "                action  manipulate\n";
        s += "                comparison-type  case-sensitive\n";
        s += "                element-rule\n";
        s += "                        type  uri-host\n";
        s += "                        action  replace\n";
        s += "                        match-value  host\\.example\n";
        s += "                        new-value  $LOCAL_IP\n";
    }
    return s;
}

fs::path tempSoPath() {
    return fs::temp_directory_path() /
           ("hmr_bench_" + std::to_string(::getpid()) + ".so");
}

double timeCompile(const std::string& src, const std::string& out, int iters,
                   std::size_t* soBytes) {
    hmr::pipeline::Compiler comp;
    hmr::pipeline::CompileOptions opts;
    opts.outputPath = out;

    auto t0 = Clock::now();
    for (int i = 0; i < iters; ++i) {
        auto r = comp.compileToFile(src, opts);
        if (!r) {
            std::fprintf(stderr, "compile failed: %s\n",
                         r.error().format().c_str());
            return -1;
        }
        if (soBytes) *soBytes = r->byteSize;
    }
    auto t1 = Clock::now();
    return std::chrono::duration<double, std::milli>(t1 - t0).count() / iters;
}

double timeApply(const hmr::module::LoadedModule& mod, int rules, int iters) {
    HmrSipMsg base;
    base.isRequest = true;
    base.method = "INVITE";
    base.requestUri = "sip:bob@example.com";
    base.addHeader("Via", "SIP/2.0/UDP 1.2.3.4:5060;branch=z9hG4bK1");
    base.addHeader("From", "\"Alice\" <sip:alice@host.example>;tag=12345");
    base.addHeader("To", "<sip:bob@example.com>");
    for (int i = 0; i < rules; ++i)
        base.addHeader("X-Hop-" + std::to_string(i),
                       "<sip:hop@host.example:5060>");

    auto ctx = hmr::runtime::makeContext(mod.info());
    ctx.setVar(HMR_VAR_LOCAL_IP, "203.0.113.5");

    volatile std::uint32_t sink = 0;
    auto t0 = Clock::now();
    for (int i = 0; i < iters; ++i) {
        HmrSipMsg msg = base;  // each packet starts identical
        ctx.resetForApply();
        sink += static_cast<std::uint32_t>(mod.apply(&msg, &ctx));
        sink += static_cast<std::uint32_t>(msg.getHeader("X-Hop-0").size());
    }
    auto t1 = Clock::now();
    (void)sink;
    return std::chrono::duration<double, std::nano>(t1 - t0).count() / iters;
}

}  // namespace

int main() {
    constexpr int kRules = 20;
    const std::string src = makeRuleset(kRules);
    const fs::path so = tempSoPath();

    std::size_t soBytes = 0;
    double compileMs = timeCompile(src, so.string(), 10, &soBytes);
    if (compileMs < 0) return 1;

    std::printf("compile  (%d rules)   : %8.3f ms/compile  (target < 80 ms)\n",
                kRules, compileMs);
    std::printf("module size           : %8.2f KB          (target < 50 KB)\n",
                static_cast<double>(soBytes) / 1024.0);

    hmr::module::ModuleManager mgr;
    auto mod = mgr.load(so.string());
    if (!mod) {
        std::fprintf(stderr, "load failed: %s\n", mod.error().format().c_str());
        std::error_code ec;
        fs::remove(so, ec);
        return 1;
    }

    double applyNs = timeApply(**mod, kRules, 200000);
    std::printf("apply    (per packet) : %8.1f ns/packet   (target < 200 ns*)\n",
                applyNs);
    std::printf("  * compiled .so over the std::string SIP model + std::regex;\n"
                "    a packet rewrites %d URI hosts via the generated hmr_apply.\n",
                kRules);

    std::error_code ec;
    fs::remove(so, ec);
    return 0;
}
