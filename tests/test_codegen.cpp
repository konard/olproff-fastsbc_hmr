// SPDX-License-Identifier: MIT
//
// test_codegen.cpp — end-to-end tests for the LLVM-backed half of the pipeline.
//
// These compile real HMR samples all the way to a native .so, load the module
// through the ModuleManager (dlopen + RCU hot-swap), and apply it to live SIP
// messages — exercising the exact path a host SBC takes. They are the only
// tests that would catch a codegen/runtime ABI mismatch or a wrong match-guard,
// so they double as regression coverage for the "match-value is a regex" fix.
//
// Resolving the module's `hmr_rt_*` imports requires the test executable to
// export those symbols: the Meson target builds with `export_dynamic: true`
// (-rdynamic) and references hmr::runtime::make_context, which lives in the same
// translation unit as every hmr_rt_* definition, so the whole runtime object is
// linked in and published to the dynamic symbol table.

#include <unistd.h>  // getpid

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>

#include "test.hpp"
#include "hmr/backend/backend.hpp"
#include "hmr/module/module_manager.hpp"
#include "hmr/pipeline/compiler.hpp"
#include "hmr/runtime/context.hpp"
#include "hmr/runtime/sip_message.hpp"
#include "hmr/runtime/hmr_runtime.h"

#ifndef HMR_SAMPLES_DIR
#define HMR_SAMPLES_DIR "."
#endif

namespace {

namespace fs = std::filesystem;

std::string read_sample(const std::string& name) {
    std::ifstream in(fs::path(HMR_SAMPLES_DIR) / name, std::ios::binary);
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

// Count non-overlapping occurrences of `needle` in `hay` (for IR marker checks).
std::size_t count_occurrences(const std::string& hay, const std::string& needle) {
    std::size_t n = 0;
    for (std::size_t i = hay.find(needle); i != std::string::npos;
         i = hay.find(needle, i + needle.size()))
        ++n;
    return n;
}

// A per-run-unique scratch path for a compiled module, removed on destruction.
fs::path temp_so_path() {
    static int counter = 0;
    return fs::temp_directory_path() /
           ("hmr_codegen_test_" + std::to_string(::getpid()) + "_" +
            std::to_string(counter++) + ".so");
}

// Owns a compiled .so on disk plus its compile-time facts; deletes the file when
// it goes out of scope. Movable (so it can be returned), non-copyable.
struct CompiledSo {
    fs::path path;
    hmr::pipeline::CompileResult result{};

    CompiledSo() = default;
    CompiledSo(const CompiledSo&) = delete;
    CompiledSo& operator=(const CompiledSo&) = delete;
    CompiledSo(CompiledSo&& o) noexcept
        : path(std::move(o.path)), result(std::move(o.result)) {
        o.path.clear();
    }
    CompiledSo& operator=(CompiledSo&&) = delete;
    ~CompiledSo() {
        if (!path.empty()) {
            std::error_code ec;
            fs::remove(path, ec);
        }
    }
};

// Compile HMR `src` to a fresh temp .so. Prints the diagnostic and returns
// nullopt on failure so the caller can REQUIRE() it.
std::optional<CompiledSo> compile(const std::string& src) {
    CompiledSo c;
    c.path = temp_so_path();
    hmr::pipeline::Compiler comp;
    hmr::pipeline::CompileOptions opts;
    opts.output_path = c.path.string();
    auto r = comp.compile_to_file(src, opts);
    if (!r) {
        std::fprintf(stderr, "    compile error: %s\n",
                     r.error().format().c_str());
        return std::nullopt;
    }
    c.result = std::move(*r);
    return c;
}

// Records the last event an observer saw — used to assert lifecycle signalling.
struct RecordingObserver : hmr::module::ModuleObserver {
    int events = 0;
    hmr::module::ModuleEvent last{};
    void on_module_event(const hmr::module::ModuleEvent& e) override {
        ++events;
        last = e;
    }
};

}  // namespace

// ---------------------------------------------------------------------------
// IR-level structural checks (cheap: no backend / linker).
// ---------------------------------------------------------------------------
TEST(Codegen, IrExposesEntryPointsAndRegexGuard) {
    auto src = read_sample("topology_hiding.hmr");
    REQUIRE(!src.empty());

    hmr::pipeline::Compiler comp;
    auto ir = comp.compile_to_ir(src);
    REQUIRE(ir.has_value());

    // The module's stable exports.
    CHECK(ir->find("@hmr_apply(") != std::string::npos);
    CHECK(ir->find("@hmr_module_info") != std::string::npos);
    // The pattern-rule match-value `internal\.local` must lower to the unified
    // matcher dispatched with match-type 2 (HMR_MATCH_REGEX), NOT a literal
    // str_eq — this is the guard the regression below depends on.
    CHECK(ir->find("@hmr_rt_match") != std::string::npos);
    CHECK(ir->find("@hmr_rt_match(ptr %ctx, i32 2,") != std::string::npos);
    CHECK(ir->find("@hmr_rt_str_eq") == std::string::npos);
    // Topology hiding drops internal headers.
    CHECK(ir->find("@hmr_rt_delete_header") != std::string::npos);
}

// ---------------------------------------------------------------------------
// Backend::optimize_ir — the engine behind `hmrc dump-ir --opt`. Proves both
// stages of the IR pipeline run: the custom HmrAttributePass (adds `nounwind`
// at every level) and the standard -O<n> pipeline (marks tail calls and folds
// the straight-line basic-block chain only at O>0). The before/after pair is the
// "LLVM IR examples (before/after optimization)" deliverable.
// ---------------------------------------------------------------------------
TEST(Codegen, OptimizeIrRunsCustomPassAndOptPipeline) {
    auto src = read_sample("topology_hiding.hmr");
    REQUIRE(!src.empty());

    hmr::pipeline::Compiler comp;
    auto before = comp.compile_to_ir(src);
    REQUIRE(before.has_value());
    // The generator emits no attributes / tail calls and a basic-block chain.
    const std::size_t branches_before = count_occurrences(*before, "br label");
    CHECK(before->find("nounwind") == std::string::npos);
    CHECK(before->find("tail call") == std::string::npos);
    CHECK(branches_before > 1);

    hmr::backend::Backend backend;

    // -O3: custom pass + full pipeline. nounwind appears, calls become tail
    // calls, and the trivial block chain folds away.
    hmr::backend::BackendOptions o3;
    o3.opt_level = 3;
    auto opt3 = backend.optimize_ir(*before, o3);
    REQUIRE(opt3.has_value());
    CHECK(opt3->find("@hmr_apply(") != std::string::npos);  // entry point kept
    CHECK(opt3->find("nounwind") != std::string::npos);     // HmrAttributePass
    CHECK(opt3->find("tail call") != std::string::npos);    // -O3
    CHECK(count_occurrences(*opt3, "br label") < branches_before);  // folded

    // -O0: the custom pass still runs (nounwind present) but the opt pipeline
    // does not mark tail calls — isolating the two contributions.
    hmr::backend::BackendOptions o0;
    o0.opt_level = 0;
    auto opt0 = backend.optimize_ir(*before, o0);
    REQUIRE(opt0.has_value());
    CHECK(opt0->find("nounwind") != std::string::npos);   // custom pass, any -O
    CHECK(opt0->find("tail call") == std::string::npos);  // no -O0 tail marking
}

// ---------------------------------------------------------------------------
// optimize_ir surfaces a diagnostic (rather than crashing) on malformed IR.
// ---------------------------------------------------------------------------
TEST(Codegen, OptimizeIrRejectsInvalidIr) {
    hmr::backend::Backend backend;
    auto bad = backend.optimize_ir("this is not llvm ir");
    CHECK(!bad.has_value());
}

// ---------------------------------------------------------------------------
// Full pipeline: compile → load → inspect descriptor.
// ---------------------------------------------------------------------------
TEST(Codegen, CompileProducesLoadableModule) {
    auto src = read_sample("topology_hiding.hmr");
    REQUIRE(!src.empty());
    auto so = compile(src);
    REQUIRE(so.has_value());

    // Size budget from the issue: a single ruleset stays well under 50 KB.
    CHECK(so->result.byte_size > 0);
    CHECK(so->result.byte_size < 50u * 1024u);
    CHECK_EQ(so->result.module_name, std::string("TopologyHiding"));
    CHECK_EQ(so->result.num_regexes, 1u);  // internal\.local

    hmr::module::ModuleManager mgr;
    auto mod = mgr.load(so->path.string());
    REQUIRE(mod.has_value());
    CHECK_EQ((*mod)->info().abi_version, std::uint32_t{HMR_ABI_VERSION});
    CHECK_EQ((*mod)->name(), std::string("TopologyHiding"));
    CHECK(mgr.current() != nullptr);
}

// ---------------------------------------------------------------------------
// Regression: a regex match-value (escaped dot) must actually fire. Before the
// fix, case-sensitive comparisons used a literal str_eq and `internal\.local`
// never matched the host `internal.local`, so the From rewrite silently no-op'd.
// ---------------------------------------------------------------------------
TEST(Codegen, TopologyHidingRewritesHostAndDropsHeaders) {
    auto src = read_sample("topology_hiding.hmr");
    REQUIRE(!src.empty());
    auto so = compile(src);
    REQUIRE(so.has_value());

    hmr::module::ModuleManager mgr;
    auto mod = mgr.load(so->path.string());
    REQUIRE(mod.has_value());

    HmrSipMsg msg = HmrSipMsg::parse(
        "INVITE sip:bob@example.com SIP/2.0\r\n"
        "From: \"Alice\" <sip:alice@internal.local>;tag=99\r\n"
        "To: <sip:bob@example.com>\r\n"
        "Server: SecretPBX/9.9\r\n"
        "User-Agent: InternalUA\r\n"
        "X-Internal-Route: hop-1\r\n"
        "\r\n");

    auto ctx = hmr::runtime::make_context((*mod)->info());
    ctx.set_var(HMR_VAR_LOCAL_IP, "203.0.113.5");
    ctx.reset_for_apply();

    CHECK_EQ((*mod)->apply(&msg, &ctx), HMR_OK);

    const std::string from(msg.get_header("From").sv());
    CHECK(from.find("203.0.113.5") != std::string::npos);     // rewritten
    CHECK(from.find("internal.local") == std::string::npos);  // gone
    CHECK(msg.get_header("Server").empty());                  // dropped
    CHECK(msg.get_header("User-Agent").empty());              // dropped
    CHECK(msg.get_header("X-Internal-Route").empty());        // dropped
}

// ---------------------------------------------------------------------------
// manipulate / element-rule replace on URI sub-fields (user + display name).
// ---------------------------------------------------------------------------
TEST(Codegen, AnonymizeFromReplacesUserAndDisplay) {
    auto src = read_sample("anonymize_from.hmr");
    REQUIRE(!src.empty());
    auto so = compile(src);
    REQUIRE(so.has_value());

    hmr::module::ModuleManager mgr;
    auto mod = mgr.load(so->path.string());
    REQUIRE(mod.has_value());

    HmrSipMsg msg = HmrSipMsg::parse(
        "INVITE sip:bob@example.com SIP/2.0\r\n"
        "From: \"Alice\" <sip:alice@internal.local>;tag=99\r\n"
        "To: <sip:bob@example.com>\r\n"
        "\r\n");

    auto ctx = hmr::runtime::make_context((*mod)->info());
    ctx.reset_for_apply();
    CHECK_EQ((*mod)->apply(&msg, &ctx), HMR_OK);

    const std::string from(msg.get_header("From").sv());
    CHECK(from.find("anonymous@internal.local") != std::string::npos);
    CHECK(from.find("Anonymous") != std::string::npos);  // display rewritten
    CHECK(from.find("alice") == std::string::npos);      // original user gone
    CHECK_EQ(std::string(msg.get_header("Privacy").sv()), std::string("id"));
}

// ---------------------------------------------------------------------------
// add with $variable interpolation + method/msg-type guards.
// ---------------------------------------------------------------------------
TEST(Codegen, NormalizePaiAddsInterpolatedHeaders) {
    auto src = read_sample("normalize_pai.hmr");
    REQUIRE(!src.empty());
    auto so = compile(src);
    REQUIRE(so.has_value());

    hmr::module::ModuleManager mgr;
    auto mod = mgr.load(so->path.string());
    REQUIRE(mod.has_value());

    HmrSipMsg msg = HmrSipMsg::parse(
        "INVITE sip:bob@example.com SIP/2.0\r\n"
        "From: <sip:alice@core.example.net>;tag=1\r\n"
        "To: <sip:bob@example.com>\r\n"
        "\r\n");

    auto ctx = hmr::runtime::make_context((*mod)->info());
    ctx.set_var(HMR_VAR_LOCAL_IP, "203.0.113.5");
    ctx.set_var(HMR_VAR_TRUNK_GROUP, "tg-42");
    ctx.set_var(HMR_VAR_REALM, "core.example.net");
    ctx.reset_for_apply();

    CHECK_EQ((*mod)->apply(&msg, &ctx), HMR_OK);
    CHECK_EQ(std::string(msg.get_header("P-Asserted-Identity").sv()),
             std::string("sip:tg-42@203.0.113.5"));
    CHECK_EQ(std::string(msg.get_header("X-Realm").sv()),
             std::string("realm=core.example.net"));
}

// ---------------------------------------------------------------------------
// reject: a pattern-rule match on a blocked prefix returns HMR_REJECTED.
// ---------------------------------------------------------------------------
TEST(Codegen, NormalizePaiRejectsBlockedPrefix) {
    auto src = read_sample("normalize_pai.hmr");
    REQUIRE(!src.empty());
    auto so = compile(src);
    REQUIRE(so.has_value());

    hmr::module::ModuleManager mgr;
    auto mod = mgr.load(so->path.string());
    REQUIRE(mod.has_value());

    HmrSipMsg msg = HmrSipMsg::parse(
        "INVITE sip:19005551234@example.com SIP/2.0\r\n"
        "From: <sip:alice@core.example.net>;tag=1\r\n"
        "To: <sip:19005551234@example.com>\r\n"
        "\r\n");

    auto ctx = hmr::runtime::make_context((*mod)->info());
    ctx.set_var(HMR_VAR_LOCAL_IP, "203.0.113.5");
    ctx.set_var(HMR_VAR_TRUNK_GROUP, "tg-42");
    ctx.set_var(HMR_VAR_REALM, "core.example.net");
    ctx.reset_for_apply();

    CHECK_EQ((*mod)->apply(&msg, &ctx), HMR_REJECTED);
    CHECK(ctx.rejected);
}

// ---------------------------------------------------------------------------
// Observer (GoF) + RCU hot-swap: a second load reports Reloaded and republishes.
// ---------------------------------------------------------------------------
TEST(Codegen, ModuleManagerObserverAndHotSwap) {
    auto src = read_sample("topology_hiding.hmr");
    REQUIRE(!src.empty());
    auto so = compile(src);
    REQUIRE(so.has_value());

    hmr::module::ModuleManager mgr;
    RecordingObserver obs;
    mgr.subscribe(&obs);

    auto first = mgr.load(so->path.string());
    REQUIRE(first.has_value());
    CHECK_EQ(obs.events, 1);
    CHECK(obs.last.kind == hmr::module::ModuleEventKind::Loaded);
    CHECK_EQ(obs.last.module_name, std::string("TopologyHiding"));
    auto snapshot = mgr.current();  // hold a reader snapshot across the swap

    auto second = mgr.load(so->path.string());
    REQUIRE(second.has_value());
    CHECK_EQ(obs.events, 2);
    CHECK(obs.last.kind == hmr::module::ModuleEventKind::Reloaded);
    CHECK(mgr.current() != snapshot);  // current republished
    CHECK(snapshot != nullptr);        // old module still alive for its reader

    mgr.unsubscribe(&obs);
}

// ---------------------------------------------------------------------------
// A load failure is reported, not fatal, and leaves current() untouched.
// ---------------------------------------------------------------------------
TEST(Codegen, ModuleManagerReportsLoadFailure) {
    hmr::module::ModuleManager mgr;
    RecordingObserver obs;
    mgr.subscribe(&obs);

    auto bad = mgr.load("/nonexistent/path/to/module.so");
    CHECK(!bad.has_value());
    CHECK_EQ(obs.events, 1);
    CHECK(obs.last.kind == hmr::module::ModuleEventKind::LoadFailed);
    CHECK(mgr.current() == nullptr);

    mgr.unsubscribe(&obs);
}

TEST_MAIN()
