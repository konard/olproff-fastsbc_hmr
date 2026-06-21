// SPDX-License-Identifier: MIT
//
// host_integration — a complete, self-contained example of embedding the HMR
// compiler in a host (the role a real SBC data-plane plays).
//
// It exercises every layer of the pipeline in one program:
//   1. compile an embedded HMR ruleset to a native .so   (pipeline::Compiler)
//   2. observe load events                                (module::ModuleObserver)
//   3. dlopen + ABI-check the module                      (module::ModuleManager)
//   4. build a per-thread runtime context                 (runtime::makeContext)
//   5. apply the module to a SIP message, with before/after + verdict
//   6. hot-swap the module for a new ruleset and re-apply  (RCU reload)
//
// The generated module resolves its hmr_rt_* callbacks against THIS process, so
// the example is linked with exported dynamic symbols (see CMakeLists.txt).

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
                        match-value  internal\.local
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
                        match-value  internal\.local
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
    void onModuleEvent(const hmr::module::ModuleEvent& e) override {
        const char* k = e.kind == hmr::module::ModuleEventKind::Loaded ? "loaded"
                        : e.kind == hmr::module::ModuleEventKind::Reloaded
                            ? "reloaded"
                            : "load-failed";
        std::printf("[observer] %s: %s%s%s\n", k,
                    e.moduleName.empty() ? e.path.c_str() : e.moduleName.c_str(),
                    e.error.empty() ? "" : " — ", e.error.c_str());
    }
};

fs::path tempSo(const char* tag) {
    return fs::temp_directory_path() /
           ("hmr_example_" + std::string(tag) + ".so");
}

// Compile an embedded ruleset to `out`, printing the module facts. Returns false
// (with a diagnostic) on failure.
bool compile(const char* source, const fs::path& out) {
    hmr::pipeline::Compiler compiler;
    hmr::pipeline::CompileOptions opts;
    opts.outputPath = out.string();
    auto r = compiler.compileToFile(source, opts);
    if (!r) {
        std::fprintf(stderr, "compile failed: %s\n", r.error().format().c_str());
        return false;
    }
    std::printf("[compile] %s: %u rule(s), %u regex(es), %zu bytes\n",
                r->moduleName.c_str(), r->numHeaderRules, r->numRegexes,
                r->byteSize);
    return true;
}

// A representative outbound INVITE that leaks internal topology.
HmrSipMsg makeLeakyRequest() {
    return HmrSipMsg::parse(
        "INVITE sip:bob@example.com SIP/2.0\r\n"
        "From: \"Alice\" <sip:alice@internal.local>;tag=99\r\n"
        "To: <sip:bob@example.com>\r\n"
        "Server: SecretPBX/9.9\r\n"
        "User-Agent: InternalUA\r\n"
        "X-Internal-Route: hop-1\r\n"
        "Contact: <sip:alice@10.0.0.1>\r\n");
}

// Apply `mod` to a fresh leaky request and print before/after + verdict.
void applyAndShow(const hmr::module::LoadedModule& mod, const char* label) {
    HmrSipMsg msg = makeLeakyRequest();

    auto ctx = hmr::runtime::makeContext(mod.info());
    ctx.setVar(HMR_VAR_LOCAL_IP, "203.0.113.5");
    ctx.resetForApply();

    std::printf("\n=== %s ===\n--- before ---\n%s", label,
                msg.toString().c_str());
    int verdict = mod.apply(&msg, &ctx);
    std::printf("--- verdict = %d (%s) ---\n--- after ---\n%s", verdict,
                verdict == HMR_OK ? "OK" : verdict == HMR_REJECTED ? "REJECTED"
                                                                   : "ERROR",
                msg.toString().c_str());
}

}  // namespace

int main() {
    const fs::path soV1 = tempSo("v1");
    const fs::path soV2 = tempSo("v2");

    hmr::module::ModuleManager mgr;
    LoggingObserver observer;
    mgr.subscribe(&observer);

    // 1) Compile + load V1, then apply.
    if (!compile(kRulesetV1, soV1)) return 1;
    auto loaded = mgr.load(soV1.string());
    if (!loaded) {
        std::fprintf(stderr, "load failed: %s\n", loaded.error().format().c_str());
        return 1;
    }
    applyAndShow(**loaded, "V1: topology hiding");

    // 2) Hot-swap to V2 (adds X-Anonymized) and apply against a fresh packet.
    //    Existing snapshots would keep using V1; new current() picks up V2.
    if (!compile(kRulesetV2, soV2)) return 1;
    auto reloaded = mgr.load(soV2.string());
    if (!reloaded) {
        std::fprintf(stderr, "reload failed: %s\n",
                     reloaded.error().format().c_str());
        return 1;
    }
    applyAndShow(*mgr.current(), "V2: after hot reload");

    mgr.unsubscribe(&observer);
    std::error_code ec;
    fs::remove(soV1, ec);
    fs::remove(soV2, ec);
    return 0;
}
