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

// host_integration — a complete, self-contained example of embedding the HMR
// compiler in a host (the role a real SBC data-plane plays).
//
// It exercises every layer of the pipeline in one program:
//   1. compile an embedded HMR ruleset to a native .so   (pipeline::Compiler)
//   2. observe load events                                (module::ModuleObserver)
//   3. dlopen + ABI-check the module                      (module::ModuleManager)
//   4. build a per-thread runtime context                 (runtime::make_context)
//   5. apply the module to a SIP message, with before/after + verdict
//   6. hot-swap the module for a new ruleset and re-apply  (RCU reload)
//
// The generated module resolves its hmr_rt_* callbacks against THIS process, so
// the example is linked with exported dynamic symbols (see meson.build).

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <string>

#include "hmr/module/module_manager.hpp"
#include "hmr/pipeline/compiler.hpp"
#include "hmr/runtime/arena.hpp"
#include "hmr/runtime/context.hpp"
#include "hmr/runtime/hmr_runtime.h"
#include "hmr/runtime/sip_message.hpp"

namespace {

namespace fs = std::filesystem;

// A topology-hiding ruleset: mask the internal host in From, and strip headers
// that leak the internal platform. Uses only fully-lowered HMR features.
constexpr const char* kRulesetV1 = R"hmr(
sip-manipulation TopologyHiding
        header-rule
                name            maskFromHost
                header-name     From
                action          manipulate
                comparison-type case-sensitive
                element-rule
                        name         rewriteHost
                        type         uri-host
                        action       replace
                        match-value  internal.local
                        new-value    $LOCAL_IP
        header-rule
                name            dropServer
                header-name     Server
                action          delete-header
        header-rule
                name            dropUserAgent
                header-name     User-Agent
                action          delete-header
        header-rule
                name            dropInternalRoute
                header-name     X-Internal-Route
                action          delete-header
)hmr";

// V2 adds a marker header — used to demonstrate a behaviour change across a
// hot reload, without restarting the process.
constexpr const char* kRulesetV2 = R"hmr(
sip-manipulation TopologyHiding
        header-rule
                name            maskFromHost
                header-name     From
                action          manipulate
                comparison-type case-sensitive
                element-rule
                        name         rewriteHost
                        type         uri-host
                        action       replace
                        match-value  internal.local
                        new-value    $LOCAL_IP
        header-rule
                name            dropServer
                header-name     Server
                action          delete-header
        header-rule
                name            tagAnonymized
                header-name     X-Anonymized
                action          add
                new-value       true
)hmr";

// GoF Observer: log module lifecycle events as a real host would (to re-prepare
// per-thread contexts on a swap, emit metrics, etc.).
class LoggingObserver final : public hmr::module::ModuleObserver {
public:
    void on_module_event(const hmr::module::ModuleEvent& e) override {
        const char* k = e.kind == hmr::module::ModuleEventKind::Loaded ? "loaded"
                        : e.kind == hmr::module::ModuleEventKind::Reloaded
                            ? "reloaded"
                            : "load-failed";
        std::printf("[observer] %s: %s%s%s\n", k,
                    e.module_name.empty() ? e.path.c_str()
                                          : e.module_name.c_str(),
                    e.error.empty() ? "" : " — ", e.error.c_str());
    }
};

fs::path temp_so(const char* tag) {
    return fs::temp_directory_path() /
           ("hmr_example_" + std::string(tag) + ".so");
}

// Compile an embedded ruleset to `out`, printing the module facts. Returns false
// (with a diagnostic) on failure.
bool compile(const char* source, const fs::path& out) {
    hmr::pipeline::Compiler compiler;
    hmr::pipeline::CompileOptions opts;
    opts.output_path = out.string();
    auto r = compiler.compile_to_file(source, opts);
    if (!r) {
        std::fprintf(stderr, "compile failed: %s\n", r.error().format().c_str());
        return false;
    }
    std::printf("[compile] %s: %u rule(s), %u regex(es), %zu bytes\n",
                r->module_name.c_str(), r->num_header_rules, r->num_regexes,
                r->byte_size);
    return true;
}

// A representative outbound INVITE that leaks internal topology. The raw bytes
// are a string literal (static storage), so the slices the message holds into
// them stay valid for the program's lifetime.
HmrSipMsg make_leaky_request() {
    return HmrSipMsg::parse(
        "INVITE sip:bob@example.com SIP/2.0\r\n"
        "From: \"Alice\" <sip:alice@internal.local>;tag=99\r\n"
        "To: <sip:bob@example.com>\r\n"
        "Server: SecretPBX/9.9\r\n"
        "User-Agent: InternalUA\r\n"
        "X-Internal-Route: hop-1\r\n"
        "Contact: <sip:alice@10.0.0.1>\r\n");
}

// Serialize `msg` into `scratch` and write it to stdout. A dedicated scratch
// arena (not the context's) keeps this print independent of the mutations that
// live in the context arena, so it is safe to call before and after apply.
void print_msg(const HmrSipMsg& msg, HmrArena& scratch) {
    scratch.reset();
    const HmrStr s = msg.serialize(scratch);
    std::fwrite(s.data, 1, s.len, stdout);
}

// Apply `mod` to a fresh leaky request and print before/after + verdict.
void apply_and_show(const hmr::module::LoadedModule& mod, const char* label,
                    HmrArena& scratch) {
    HmrSipMsg msg = make_leaky_request();

    auto ctx = hmr::runtime::make_context(mod.info());
    ctx.set_var(HMR_VAR_LOCAL_IP, "203.0.113.5");
    ctx.reset_for_apply();

    std::printf("\n=== %s ===\n--- before ---\n", label);
    print_msg(msg, scratch);
    const int verdict = mod.apply(&msg, &ctx);
    std::printf("--- verdict = %d (%s) ---\n--- after ---\n", verdict,
                verdict == HMR_OK         ? "OK"
                : verdict == HMR_REJECTED ? "REJECTED"
                                          : "ERROR");
    print_msg(msg, scratch);
}

}  // namespace

int main() {
    const fs::path so_v1 = temp_so("v1");
    const fs::path so_v2 = temp_so("v2");

    hmr::module::ModuleManager mgr;
    LoggingObserver observer;
    mgr.subscribe(&observer);

    // A scratch arena used only to render before/after snapshots for printing.
    auto scratch = std::make_unique<HmrArena>();

    // 1) Compile + load V1, then apply.
    if (!compile(kRulesetV1, so_v1)) return 1;
    auto loaded = mgr.load(so_v1.string());
    if (!loaded) {
        std::fprintf(stderr, "load failed: %s\n", loaded.error().format().c_str());
        return 1;
    }
    apply_and_show(**loaded, "V1: topology hiding", *scratch);

    // 2) Hot-swap to V2 (adds X-Anonymized) and apply against a fresh packet.
    //    Existing snapshots would keep using V1; new current() picks up V2.
    if (!compile(kRulesetV2, so_v2)) return 1;
    auto reloaded = mgr.load(so_v2.string());
    if (!reloaded) {
        std::fprintf(stderr, "reload failed: %s\n",
                     reloaded.error().format().c_str());
        return 1;
    }
    apply_and_show(*mgr.current(), "V2: after hot reload", *scratch);

    mgr.unsubscribe(&observer);
    std::error_code ec;
    fs::remove(so_v1, ec);
    fs::remove(so_v2, ec);
    return 0;
}
