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

// test_interp.cpp — differential test: the AST interpreter (the reference
// oracle) must behave byte-for-byte like the compiled .so.
//
// For every valid sample ruleset we build BOTH back-ends from the same source —
// an hmr::interp::Interpreter (tree-walk) and a native module compiled to a .so
// and dlopen'd through the ModuleManager — then apply each to the same corpus of
// SIP packets. Because both paths bottom out in the one hmr_rt_* runtime and
// share the AST→runtime lowering (codegen/lowering.hpp), their observable
// results must be identical: the serialized message, the verdict, and the
// reject / log side-channel. A divergence means the interpreter and the code
// generator disagree about what a rule does.
//
// This is the equivalence the rest of the rework leans on. The "compiled vs
// interpreter" benchmark is only meaningful if the two compute the same thing,
// and the differential fuzzer trusts the interpreter as ground truth for random
// rulesets — this test pins that trust on a curated, human-readable corpus.
//
// Like test_codegen, the compiled module resolves its hmr_rt_* imports against
// this executable, so the Meson target builds with export_dynamic (-rdynamic)
// and links the runtime.

#include <unistd.h>  // getpid

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

#include "test.hpp"
#include "hmr/interp/interpreter.hpp"
#include "hmr/module/module_manager.hpp"
#include "hmr/parser/parser.hpp"
#include "hmr/pipeline/compiler.hpp"
#include "hmr/runtime/arena.hpp"
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

// A per-run-unique scratch path for a compiled module, removed on destruction.
fs::path temp_so_path() {
    static int counter = 0;
    return fs::temp_directory_path() /
           ("hmr_interp_test_" + std::to_string(::getpid()) + "_" +
            std::to_string(counter++) + ".so");
}

// Owns a compiled .so on disk; deletes the file when it goes out of scope.
struct CompiledSo {
    fs::path path;
    CompiledSo() = default;
    CompiledSo(const CompiledSo&) = delete;
    CompiledSo& operator=(const CompiledSo&) = delete;
    CompiledSo(CompiledSo&& o) noexcept : path(std::move(o.path)) {
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

// Compile HMR `src` to a fresh temp .so; prints the diagnostic and returns
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
    return c;
}

// Bind every built-in variable to a fixed value, so a rule that reads any of
// them sees the same input on both sides regardless of which the sample uses.
void set_all_vars(HmrContext& ctx) {
    ctx.set_var(HMR_VAR_LOCAL_IP, "203.0.113.5");
    ctx.set_var(HMR_VAR_REMOTE_IP, "198.51.100.20");
    ctx.set_var(HMR_VAR_LOCAL_PORT, "5060");
    ctx.set_var(HMR_VAR_REMOTE_PORT, "5061");
    ctx.set_var(HMR_VAR_TRUNK_GROUP, "tg-42");
    ctx.set_var(HMR_VAR_REALM, "core.example.net");
    ctx.set_var(HMR_VAR_INTERFACE, "eth0");
    ctx.set_var(HMR_VAR_METHOD, "INVITE");
    ctx.set_var(HMR_VAR_RURI_USER, "bob");
    ctx.set_var(HMR_VAR_RURI_HOST, "example.com");
    ctx.set_var(HMR_VAR_TO_USER, "bob");
    ctx.set_var(HMR_VAR_TO_HOST, "example.com");
    ctx.set_var(HMR_VAR_FROM_USER, "alice");
    ctx.set_var(HMR_VAR_FROM_HOST, "internal.local");
}

// The full observable result of applying a ruleset to one packet.
struct Outcome {
    int verdict = 0;
    std::string wire;
    bool rejected = false;
    std::uint32_t reject_code = 0;
    std::string reject_reason;
    std::vector<std::string> logs;
    bool operator==(const Outcome&) const = default;
};

// Serialize into a dedicated arena (the message's slices point into the
// context's arena, so writing the wire form there too is safe — the arena is a
// fixed buffer and never moves existing bytes — but a separate one keeps the
// per-packet arena's budget out of the comparison).
std::string serialize_to_string(const HmrSipMsg& msg) {
    auto arena = std::make_unique<HmrArena>();
    HmrStr w = msg.serialize(*arena);
    return std::string(w.data ? w.data : "", w.len);
}

// Apply `fn` (a compiled module or the interpreter) to a fresh parse of
// `packet`, sizing the context from `info`, and capture everything observable.
template <class Apply>
Outcome capture(const HmrModuleInfo& info, Apply&& fn, std::string_view packet) {
    HmrSipMsg msg = HmrSipMsg::parse(packet);
    auto ctx = hmr::runtime::make_context(info);
    set_all_vars(ctx);
    ctx.reset_for_apply();

    Outcome o;
    o.verdict = fn(&msg, &ctx);
    o.wire = serialize_to_string(msg);
    o.rejected = ctx.rejected;
    o.reject_code = ctx.reject_code;
    o.reject_reason = ctx.reject_reason;
    o.logs = ctx.logs;
    return o;
}

struct Packet {
    const char* label;
    std::string text;
};

// A corpus chosen to drive the sample rules down their real branches:
//   * internal hosts / private IPs / a hex Call-ID / a user.host From so the
//     manipulate, capture, IP-subnet and topology rules fire,
//   * a blocked 1900x destination so the reject rules trigger,
//   * a response so reply-only rules run and request-only rules skip,
//   * a REGISTER so method-filtered rules skip,
//   * an all-external packet so the IP/host matches miss,
//   * a sparse OPTIONS so absent headers exercise the empty-value paths.
std::vector<Packet> corpus() {
    std::vector<Packet> v;
    v.push_back({"invite_internal",
        "INVITE sip:bob@example.com SIP/2.0\r\n"
        "Via: SIP/2.0/UDP 192.168.1.1:5060;branch=z9hG4bK1\r\n"
        "From: \"Alice Smith\" <sip:alice.smith@internal.example.com>;tag=99\r\n"
        "To: <sip:bob@example.com>\r\n"
        "Contact: <sip:dev@10.1.2.3:5060>\r\n"
        "Route: <sip:relay@172.16.5.5>\r\n"
        "Call-ID: deadbeef@host.example.com\r\n"
        "CSeq: 1 INVITE\r\n"
        "Server: SecretPBX/9.9\r\n"
        "User-Agent: InternalUA/2.1\r\n"
        "X-Internal-Route: hop-1\r\n"
        "\r\n"});
    v.push_back({"invite_blocked",
        "INVITE sip:19005551234@example.com SIP/2.0\r\n"
        "Via: SIP/2.0/UDP 198.51.100.7:5060;branch=z9hG4bK2\r\n"
        "From: <sip:carol@example.org>;tag=7\r\n"
        "To: <sip:19005551234@example.com>\r\n"
        "Contact: <sip:carol@198.51.100.7:5060>\r\n"
        "Call-ID: 12ab@example.org\r\n"
        "CSeq: 1 INVITE\r\n"
        "\r\n"});
    v.push_back({"response_200",
        "SIP/2.0 200 OK\r\n"
        "Via: SIP/2.0/UDP 203.0.113.9:5060;branch=z9hG4bK3\r\n"
        "From: <sip:alice@internal.example.com>;tag=99\r\n"
        "To: <sip:bob@example.com>;tag=44\r\n"
        "Contact: <sip:bob@10.0.0.9:5060>\r\n"
        "Call-ID: cafe01@host.example.com\r\n"
        "CSeq: 1 INVITE\r\n"
        "Server: SecretPBX/9.9\r\n"
        "\r\n"});
    v.push_back({"register",
        "REGISTER sip:registrar.example.com SIP/2.0\r\n"
        "Via: SIP/2.0/UDP 203.0.113.20:5060;branch=z9hG4bK4\r\n"
        "From: <sip:dave@example.com>;tag=1\r\n"
        "To: <sip:dave@example.com>\r\n"
        "Contact: <sip:dave@10.9.8.7:5060>\r\n"
        "Call-ID: 99ff@example.com\r\n"
        "CSeq: 1 REGISTER\r\n"
        "\r\n"});
    v.push_back({"invite_external",
        "INVITE sip:bob@carrier.example.net SIP/2.0\r\n"
        "Via: SIP/2.0/UDP 203.0.113.30:5060;branch=z9hG4bK5\r\n"
        "From: <sip:eve@trusted.example.net>;tag=3\r\n"
        "To: <sip:bob@carrier.example.net>\r\n"
        "Contact: <sip:eve@203.0.113.30:5060>\r\n"
        "Route: <sip:gw@9.9.9.9>\r\n"
        "Call-ID: 5050@trusted.example.net\r\n"
        "CSeq: 1 INVITE\r\n"
        "\r\n"});
    v.push_back({"options_sparse",
        "OPTIONS sip:bob@example.com SIP/2.0\r\n"
        "Via: SIP/2.0/UDP 203.0.113.40:5060;branch=z9hG4bK6\r\n"
        "From: <sip:probe@example.com>;tag=2\r\n"
        "To: <sip:bob@example.com>\r\n"
        "CSeq: 1 OPTIONS\r\n"
        "\r\n"});
    return v;
}

// The valid corpus, mirroring tests/meson.build's valid_samples list.
const char* const kSamples[] = {
    "all_variables.hmr", "anonymize_from.hmr",  "full_example.hmr",
    "ip_matching.hmr",   "minimal_ruleset.hmr", "multiple_headers.hmr",
    "nested_elements.hmr", "normalize_pai.hmr", "regex_captures.hmr",
    "topology_hiding.hmr",
};

// Compile + load + interpret one sample, then assert the two agree on every
// packet. Reported as one TEST per sample for granular failure output.
void check_sample(const char* sample) {
    auto src = read_sample(sample);
    REQUIRE(!src.empty());

    // Compiled module.
    auto so = compile(src);
    REQUIRE(so.has_value());
    hmr::module::ModuleManager mgr;
    auto mod = mgr.load(so->path.string());
    REQUIRE(mod.has_value());

    // Interpreter over an independent parse of the same source.
    auto rs = hmr::parser::Parser::parse_string(src);
    REQUIRE(rs.has_value());
    hmr::interp::Interpreter interp(*rs);

    // The synthetic and the real descriptor must agree on the table sizes: a
    // mismatch means a slot/regex id would denote different things on the two
    // sides, which is the first thing a divergence would corrupt.
    CHECK_EQ(interp.info().num_slots, (*mod)->info().num_slots);
    CHECK_EQ(interp.info().num_regexes, (*mod)->info().num_regexes);
    CHECK_EQ(interp.info().abi_version, (*mod)->info().abi_version);

    for (const Packet& p : corpus()) {
        Outcome compiled = capture(
            (*mod)->info(),
            [&](HmrSipMsg* m, HmrContext* c) { return (*mod)->apply(m, c); },
            p.text);
        Outcome walked = capture(
            interp.info(),
            [&](HmrSipMsg* m, HmrContext* c) { return interp.apply(m, c); },
            p.text);

        if (!(compiled == walked)) {
            std::fprintf(stderr,
                         "    divergence: sample=%s packet=%s\n"
                         "      verdict  compiled=%d interp=%d\n"
                         "      rejected compiled=%d interp=%d code=%u/%u\n"
                         "      logs     compiled=%zu interp=%zu\n"
                         "      --- compiled wire ---\n%s\n"
                         "      --- interp wire ---\n%s\n",
                         sample, p.label, compiled.verdict, walked.verdict,
                         compiled.rejected, walked.rejected,
                         compiled.reject_code, walked.reject_code,
                         compiled.logs.size(), walked.logs.size(),
                         compiled.wire.c_str(), walked.wire.c_str());
        }
        // Granular assertions so a failure names the exact axis that diverged.
        CHECK_EQ(compiled.verdict, walked.verdict);
        CHECK_EQ(compiled.wire, walked.wire);
        CHECK_EQ(compiled.rejected, walked.rejected);
        CHECK_EQ(compiled.reject_code, walked.reject_code);
        CHECK_EQ(compiled.reject_reason, walked.reject_reason);
        CHECK(compiled.logs == walked.logs);
    }
}

}  // namespace

// One TEST per sample (so a failure pinpoints the offending ruleset).
TEST(Interp, AllVariablesMatchesCompiled) { check_sample("all_variables.hmr"); }
TEST(Interp, AnonymizeFromMatchesCompiled) { check_sample("anonymize_from.hmr"); }
TEST(Interp, FullExampleMatchesCompiled) { check_sample("full_example.hmr"); }
TEST(Interp, IpMatchingMatchesCompiled) { check_sample("ip_matching.hmr"); }
TEST(Interp, MinimalRulesetMatchesCompiled) { check_sample("minimal_ruleset.hmr"); }
TEST(Interp, MultipleHeadersMatchesCompiled) { check_sample("multiple_headers.hmr"); }
TEST(Interp, NestedElementsMatchesCompiled) { check_sample("nested_elements.hmr"); }
TEST(Interp, NormalizePaiMatchesCompiled) { check_sample("normalize_pai.hmr"); }
TEST(Interp, RegexCapturesMatchesCompiled) { check_sample("regex_captures.hmr"); }
TEST(Interp, TopologyHidingMatchesCompiled) { check_sample("topology_hiding.hmr"); }

// A focused, self-documenting assertion that the interpreter actually *does* the
// work (not just "agrees with an equally-broken compiled module"): the topology
// rewrite must fire and the reject corpus packet must be rejected. Pins the
// oracle to a known-good ground truth independent of the differential loop.
TEST(Interp, TopologyHidingRewritesAndRejectsAreReal) {
    auto src = read_sample("full_example.hmr");
    REQUIRE(!src.empty());
    auto rs = hmr::parser::Parser::parse_string(src);
    REQUIRE(rs.has_value());
    hmr::interp::Interpreter interp(*rs);

    // A normal INVITE: From host masked, Server dropped, capture echoed, no reject.
    {
        HmrSipMsg msg = HmrSipMsg::parse(
            "INVITE sip:bob@example.com SIP/2.0\r\n"
            "From: \"Alice\" <sip:alice@internal.example.com>;tag=99\r\n"
            "To: <sip:bob@example.com>\r\n"
            "Call-ID: deadbeef@host.example.com\r\n"
            "Server: SecretPBX/9.9\r\n"
            "\r\n");
        auto ctx = hmr::runtime::make_context(interp.info());
        set_all_vars(ctx);
        ctx.reset_for_apply();
        CHECK_EQ(interp.apply(&msg, &ctx), HMR_OK);

        const std::string from(msg.get_header("From").sv());
        CHECK(from.find("203.0.113.5") != std::string::npos);          // masked
        CHECK(from.find("internal.example.com") == std::string::npos);  // gone
        CHECK(from.find("anonymous") != std::string::npos);            // user
        CHECK(msg.get_header("Server").empty());                       // dropped
        CHECK(!msg.get_header("P-Asserted-Identity").empty());         // added
        CHECK_EQ(std::string(msg.get_header("X-Orig-Call-ID").sv()),
                 std::string("deadbeef"));  // $captureCallId.$1
    }

    // A blocked 1900x destination must be rejected (403).
    {
        HmrSipMsg msg = HmrSipMsg::parse(
            "INVITE sip:19005551234@example.com SIP/2.0\r\n"
            "From: <sip:carol@example.org>;tag=7\r\n"
            "To: <sip:19005551234@example.com>\r\n"
            "\r\n");
        auto ctx = hmr::runtime::make_context(interp.info());
        set_all_vars(ctx);
        ctx.reset_for_apply();
        CHECK_EQ(interp.apply(&msg, &ctx), HMR_REJECTED);
        CHECK(ctx.rejected);
        CHECK_EQ(ctx.reject_code, std::uint32_t{403});
    }
}

// Ground-truth for Oracle's capture-group back-references ($rule.$N selects
// group N, not the whole match) and self-captures ($1/$2 in a new-value). Pins
// the exact produced bytes so the differential loop cannot agree on a *wrong*
// value: storeFromParts captures sip:([^@]+)@([^;>]+) over the From URI, a later
// element rule flips the user via $2.$1, and echoStoredUser echoes the *original*
// group 1 with $storeFromParts.$1.
TEST(Interp, RegexCaptureGroupBackReferencesAreReal) {
    auto src = read_sample("regex_captures.hmr");
    REQUIRE(!src.empty());
    auto rs = hmr::parser::Parser::parse_string(src);
    REQUIRE(rs.has_value());
    hmr::interp::Interpreter interp(*rs);

    HmrSipMsg msg = HmrSipMsg::parse(
        "INVITE sip:bob@example.com SIP/2.0\r\n"
        "From: \"Alice\" <sip:alice.smith@internal.example.com>;tag=99\r\n"
        "To: <sip:bob@example.com>\r\n"
        "\r\n");
    auto ctx = hmr::runtime::make_context(interp.info());
    set_all_vars(ctx);
    ctx.reset_for_apply();
    CHECK_EQ(interp.apply(&msg, &ctx), HMR_OK);

    // $storeFromParts.$1 is group 1 of sip:([^@]+)@([^;>]+) = the user part,
    // captured from the *original* header before flipUser rewrote it.
    CHECK_EQ(std::string(msg.get_header("X-Original-User").sv()),
             std::string("alice.smith"));

    // flipUser replaced the URI user "alice.smith" with $2.$1 = "smith.alice".
    const std::string from(msg.get_header("From").sv());
    CHECK(from.find("smith.alice") != std::string::npos);
    CHECK(from.find("internal.example.com") != std::string::npos);
}

TEST_MAIN()
