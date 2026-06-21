// SPDX-License-Identifier: MIT
//
// bench_common.hpp — shared fixtures for the HMR benchmarks.
//
// Both benchmarks measure the *same* ruleset over the *same* packet so their
// numbers compose: bench_frontend reports the front-end (parse+optimize) cost
// and the interpreter apply baseline; bench_pipeline reports the compiled-module
// cost and contrasts it with that baseline and with the "GCC approach". Keeping
// the workload here means a single source of truth for what "20 rules / one
// packet" denotes across every figure.
//
// The fixtures are parameterized by Match so the suite can also contrast the
// specialized matchers (review #2): the *same* topology-hiding workload built
// three ways — an exact strcmp, a std::regex, and a specialized IP-range test —
// shows directly that exact/ip matching is faster than routing everything
// through std::regex, which is the change review #2 demanded.

#pragma once

#include <chrono>
#include <cstdint>
#include <string>

#include "hmr/runtime/context.hpp"
#include "hmr/runtime/hmr_runtime.h"
#include "hmr/runtime/sip_message.hpp"

namespace bench {

using Clock = std::chrono::steady_clock;

// Which matcher the element-rule uses to test each X-Hop host. The logical work
// (rewrite the host to $LOCAL_IP when it matches) is identical across all three;
// only the comparison engine differs.
enum class Match {
    Exact,  // comparison-type case-sensitive — strcmp fast path
    Regex,  // comparison-type pattern-rule   — std::regex
    Ip,     // match-val-type ip (range)      — specialized address matcher
};

// The host each X-Hop URI carries. For the IP matcher the hosts are addresses in
// 10.0.0.0/24 (all inside the rule's range); otherwise a domain the exact/regex
// rules match.
inline std::string hop_host(Match m, int i) {
    return m == Match::Ip ? "10.0.0." + std::to_string(i % 254 + 1)
                          : "host.example";
}

// A representative SBC ruleset of `rules` header-rules. Each drills into a
// distinct X-Hop-i header's URI host and rewrites it to $LOCAL_IP when it
// matches — the get_header -> uri_get -> match -> uri_set -> set_header chain a
// real topology-hiding / normalization module runs per packet. The blocks are
// keyword-delimited Oracle HMR (header-rule / element-rule), not indentation;
// the leading whitespace is only for readability.
inline std::string make_ruleset(int rules, Match m = Match::Exact) {
    std::string s = "sip-manipulation BenchManip\n";
    for (int i = 0; i < rules; ++i) {
        const std::string n = std::to_string(i);
        s += "        header-rule\n";
        s += "                name             rewrite" + n + "\n";
        s += "                header-name      X-Hop-" + n + "\n";
        s += "                action           manipulate\n";
        s += "                comparison-type  case-sensitive\n";
        s += "                element-rule\n";
        s += "                        name             host" + n + "\n";
        s += "                        type             uri-host\n";
        s += "                        action           replace\n";
        switch (m) {
            case Match::Exact:
                s += "                        comparison-type  case-sensitive\n";
                s += "                        match-value      host.example\n";
                break;
            case Match::Regex:
                s += "                        comparison-type  pattern-rule\n";
                s += "                        match-value      host\\.example\n";
                break;
            case Match::Ip:
                s += "                        match-val-type   ip\n";
                s += "                        match-value      10.0.0.1-10.0.0.254\n";
                break;
        }
        s += "                        new-value        $LOCAL_IP\n";
    }
    return s;
}

// One representative INVITE carrying the `rules` X-Hop headers the ruleset
// rewrites, plus the standard headers a real packet has. Returns the raw wire
// bytes (CRLF). The returned string must outlive any message parsed from it:
// HmrSipMsg::parse slices into it zero-copy. Keep `rules` <= ~26 so the total
// header count stays under HMR_MAX_HEADERS.
inline std::string make_packet(int rules, Match m = Match::Exact) {
    std::string p =
        "INVITE sip:bob@example.com SIP/2.0\r\n"
        "Via: SIP/2.0/UDP 192.168.1.1:5060;branch=z9hG4bK1\r\n"
        "From: \"Alice\" <sip:alice@internal.example.com>;tag=99\r\n"
        "To: <sip:bob@example.com>\r\n"
        "Call-ID: deadbeef@host.example.com\r\n"
        "CSeq: 1 INVITE\r\n";
    for (int i = 0; i < rules; ++i)
        p += "X-Hop-" + std::to_string(i) + ": <sip:hop@" + hop_host(m, i) +
             ":5060>\r\n";
    p += "\r\n";
    return p;
}

// Bind every built-in variable, matching the values the tests use, so a rule
// reading any of them sees identical input on the compiled and interpreted side.
inline void set_all_vars(HmrContext& ctx) {
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

// Apply `fn` to a pristine copy of `base` `iters` times, returning ns/packet.
// `base` must hold only slices into its (still-live) raw buffer — never the
// arena — so each per-iteration copy restarts from the unmodified packet; the
// context is rewound in O(1) between applications. `fn` is the compiled module's
// apply or the interpreter's apply (both `int(HmrSipMsg*, HmrContext*)`).
template <class Apply>
double time_apply(const HmrSipMsg& base, HmrContext& ctx, Apply&& fn, int iters) {
    volatile std::uint32_t sink = 0;
    const auto t0 = Clock::now();
    for (int i = 0; i < iters; ++i) {
        HmrSipMsg msg = base;  // restart from the pristine, raw-only packet
        ctx.reset_for_apply();
        sink += static_cast<std::uint32_t>(fn(&msg, &ctx));
        sink += static_cast<std::uint32_t>(msg.get_header("X-Hop-0").len);
    }
    const auto t1 = Clock::now();
    (void)sink;
    return std::chrono::duration<double, std::nano>(t1 - t0).count() / iters;
}

}  // namespace bench
