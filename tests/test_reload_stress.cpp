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

// test_reload_stress.cpp — hot-reload stress test (review #5: "Stress tests for
// hot reload"; the issue specifies 1000 consecutive reloads).
//
// Two rulesets that each tag a different marker header are compiled once to a
// native .so, then hot-swapped through the ModuleManager 1000 times in a row.
// The test asserts that every swap succeeds, that current() always reflects the
// last-loaded ruleset (the X-Variant marker proves the RCU publish took effect),
// that a reader snapshot taken before a swap keeps applying correctly against the
// outgoing module (the RCU grace period), and that exactly one Loaded + 999
// Reloaded events fire. Driving dlopen/dlclose at this volume is what would
// surface a handle, fd or memory leak in the loader.
//
// Compiling is the expensive step (~tens of ms), so the two modules are built
// once and only the load/apply cycle is repeated 1000×. LLVM-gated (needs the
// compiler) and built with export_dynamic so the loaded modules resolve their
// hmr_rt_* imports against this binary (see meson.build).

#include <unistd.h>  // getpid

#include <cstdio>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>

#include "test.hpp"
#include "hmr/module/module_manager.hpp"
#include "hmr/pipeline/compiler.hpp"
#include "hmr/runtime/context.hpp"
#include "hmr/runtime/hmr_runtime.h"
#include "hmr/runtime/sip_message.hpp"

namespace {

namespace fs = std::filesystem;

fs::path temp_so_path(const std::string& tag) {
    static int counter = 0;
    return fs::temp_directory_path() /
           ("hmr_reload_" + tag + "_" + std::to_string(::getpid()) + "_" +
            std::to_string(counter++) + ".so");
}

// Compile a ruleset whose only effect is to add `X-Variant: <variant>`, so the
// applied output uniquely identifies which module is current. Modelled on the
// `tagRealm` rule in normalize_pai.hmr (header-level add + static new-value).
std::optional<fs::path> compile_variant(const std::string& variant) {
    const std::string src =
        "sip-manipulation Variant" + variant +
        "\n"
        "        header-rule\n"
        "                name  tagVariant\n"
        "                header-name  X-Variant\n"
        "                action  add\n"
        "                new-value  " +
        variant + "\n";
    const fs::path out = temp_so_path(variant);
    hmr::pipeline::Compiler comp;
    hmr::pipeline::CompileOptions opts;
    opts.output_path = out.string();
    auto r = comp.compile_to_file(src, opts);
    if (!r) {
        std::fprintf(stderr, "    compile failed: %s\n",
                     r.error().format().c_str());
        return std::nullopt;
    }
    return out;
}

struct CountingObserver : hmr::module::ModuleObserver {
    int loaded = 0;
    int reloaded = 0;
    int failed = 0;
    void on_module_event(const hmr::module::ModuleEvent& e) override {
        using K = hmr::module::ModuleEventKind;
        if (e.kind == K::Loaded) {
            ++loaded;
        } else if (e.kind == K::Reloaded) {
            ++reloaded;
        } else {
            ++failed;
        }
    }
};

// Apply `mod` to a fresh INVITE and return the X-Variant header it tagged.
std::string applied_variant(const hmr::module::LoadedModule& mod) {
    HmrSipMsg msg = HmrSipMsg::parse(
        "INVITE sip:bob@example.com SIP/2.0\r\n"
        "From: <sip:alice@example.net>;tag=1\r\n"
        "To: <sip:bob@example.com>\r\n"
        "\r\n");
    auto ctx = hmr::runtime::make_context(mod.info());
    ctx.reset_for_apply();
    if (mod.apply(&msg, &ctx) != HMR_OK) return "<rejected>";
    return std::string(msg.get_header("X-Variant").sv());
}

}  // namespace

TEST(ReloadStress, ThousandConsecutiveReloads) {
    auto so_a = compile_variant("A");
    auto so_b = compile_variant("B");
    REQUIRE(so_a.has_value());
    REQUIRE(so_b.has_value());

    hmr::module::ModuleManager mgr;
    CountingObserver obs;
    mgr.subscribe(&obs);

    constexpr int kReloads = 1000;
    int swaps_ok = 0;
    bool variant_ok = true;  // current() always reflects the latest load
    bool rcu_ok = true;      // a snapshot held across a swap stays valid

    for (int i = 0; i < kReloads; ++i) {
        const bool use_a = (i % 2) == 0;
        const std::string& path = use_a ? *so_a : *so_b;
        const char* want = use_a ? "A" : "B";

        // Every 100th swap (after the first), pin the OUTGOING module's snapshot
        // across the load and confirm it still applies correctly afterwards —
        // the RCU grace period in action.
        std::shared_ptr<const hmr::module::LoadedModule> snap;
        const char* snap_want = nullptr;
        if (i > 0 && (i % 100) == 0) {
            snap = mgr.current();
            snap_want = (((i - 1) % 2) == 0) ? "A" : "B";
        }

        auto loaded = mgr.load(path);
        if (!loaded) {
            variant_ok = false;
            break;
        }
        ++swaps_ok;

        auto cur = mgr.current();
        if (!cur || applied_variant(*cur) != want) variant_ok = false;
        if (snap && applied_variant(*snap) != snap_want) rcu_ok = false;
    }

    mgr.unsubscribe(&obs);
    std::error_code ec;
    fs::remove(*so_a, ec);
    fs::remove(*so_b, ec);

    CHECK_EQ(swaps_ok, kReloads);
    CHECK(variant_ok);                     // every swap published the new module
    CHECK(rcu_ok);                         // old snapshots survived the swap
    CHECK_EQ(obs.loaded, 1);               // first load only
    CHECK_EQ(obs.reloaded, kReloads - 1);  // the remaining 999 are hot reloads
    CHECK_EQ(obs.failed, 0);
}

TEST_MAIN()
