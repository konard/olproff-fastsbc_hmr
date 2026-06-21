// SPDX-License-Identifier: MIT
//
// test_runtime.cpp — unit tests for the zero-copy arena SIP model (review #4)
// and the match-val-type ABI dispatch (review #2).
//
// Covers: the bump arena (alloc/dup/reset/overflow); parse->serialize round
// trips; header get/set/add/delete over slices+arena; pre-extracted URI fields
// (request-URI and From/To); regex capture; and hmr_rt_match routing to each
// specialized matcher. Reads stay zero-copy (views into raw), writes go through
// the arena. These exercise the same callbacks the generated module calls, so
// they validate the runtime contract without needing LLVM.

#include <string>
#include <string_view>

#include "test.hpp"
#include "hmr/runtime/arena.hpp"
#include "hmr/runtime/context.hpp"
#include "hmr/runtime/hmr_runtime.h"
#include "hmr/runtime/sip_message.hpp"

namespace {

std::string_view sv(HmrStr s) noexcept {
    return std::string_view{s.data ? s.data : "", s.len};
}
HmrStr hs(std::string_view s) noexcept {
    return HmrStr{s.data(), static_cast<std::uint32_t>(s.size())};
}

HmrContext make_ctx(std::uint32_t slots = 4) {
    HmrModuleInfo info{HMR_ABI_VERSION, "test", slots, 0, nullptr};
    return hmr::runtime::make_context(info);
}

// A representative INVITE used across several cases.
const char* kInvite =
    "INVITE sip:bob@example.com SIP/2.0\r\n"
    "Via: SIP/2.0/UDP host.example.com;branch=z9hG4bK1\r\n"
    "From: \"Alice\" <sip:alice@atlanta.com>;tag=1928\r\n"
    "To: <sip:bob@example.com>\r\n"
    "Call-ID: a84b4c76e66710\r\n"
    "CSeq: 314159 INVITE\r\n"
    "\r\n";

}  // namespace

// ===========================================================================
// Arena
// ===========================================================================
TEST(Arena, AllocAndReset) {
    HmrArena a;
    CHECK_EQ(a.remaining(), HmrArena::kCapacity);
    char* p = a.alloc(10);
    CHECK(p != nullptr);
    CHECK_EQ(a.remaining(), HmrArena::kCapacity - 10);
    a.reset();
    CHECK_EQ(a.remaining(), HmrArena::kCapacity);
    // reset is O(1) reuse: the same storage is handed out again.
    CHECK_EQ(a.alloc(1), p);
}

TEST(Arena, DupCopiesAndIsStable) {
    HmrArena a;
    std::string src = "hello";
    HmrStr d = a.dup(src);
    CHECK_EQ(sv(d), "hello");
    CHECK(d.data != src.data());  // independent copy in the arena
    src[0] = 'H';
    CHECK_EQ(sv(d), "hello");  // unaffected by mutating the source
}

TEST(Arena, OverflowReturnsNull) {
    HmrArena a;
    CHECK(a.alloc(HmrArena::kCapacity) != nullptr);
    CHECK_EQ(a.alloc(1), static_cast<char*>(nullptr));
    HmrStr d = a.dup("x");
    CHECK_EQ(d.data, static_cast<const char*>(nullptr));
}

// ===========================================================================
// Parse / serialize
// ===========================================================================
TEST(SipModel, ParseRequestZeroCopy) {
    std::string raw = kInvite;
    HmrSipMsg m = HmrSipMsg::parse(raw);
    CHECK(m.is_request);
    CHECK_EQ(sv(m.method.str()), "INVITE");
    CHECK_EQ(sv(m.request_uri.str()), "sip:bob@example.com");
    CHECK_EQ(m.num_headers, 5u);  // Via, From, To, Call-ID, CSeq
    // A header value must be a view *into* the original buffer (no copy).
    HmrSipMsg::Slice cid = m.get_header("Call-ID");
    CHECK_EQ(cid.sv(), "a84b4c76e66710");
    CHECK(cid.data >= raw.data() && cid.data < raw.data() + raw.size());
}

TEST(SipModel, ParseResponse) {
    std::string raw =
        "SIP/2.0 200 OK\r\n"
        "CSeq: 1 INVITE\r\n"
        "\r\n";
    HmrSipMsg m = HmrSipMsg::parse(raw);
    CHECK(!m.is_request);
    CHECK_EQ(m.status_code, 200u);
    CHECK_EQ(sv(m.reason_phrase.str()), "OK");
}

TEST(SipModel, SerializeRoundTrip) {
    std::string raw = kInvite;
    HmrSipMsg m = HmrSipMsg::parse(raw);
    HmrArena arena;
    HmrStr out = m.serialize(arena);
    CHECK_EQ(sv(out), raw);  // byte-identical round trip
}

TEST(SipModel, HeaderLookupCaseInsensitive) {
    std::string raw = kInvite;
    HmrSipMsg m = HmrSipMsg::parse(raw);
    CHECK_EQ(m.get_header("call-id").sv(), "a84b4c76e66710");
    CHECK_EQ(m.get_header("CALL-ID").sv(), "a84b4c76e66710");
    CHECK_EQ(m.get_header("absent").sv(), "");
}

TEST(SipModel, SetHeaderRewrites) {
    std::string raw = kInvite;
    HmrSipMsg m = HmrSipMsg::parse(raw);
    HmrArena arena;
    CHECK(m.set_header("Call-ID", "rewritten@id", arena));
    CHECK_EQ(m.get_header("Call-ID").sv(), "rewritten@id");
    CHECK_EQ(m.num_headers, 5u);  // rewrite, not append
    HmrStr out = m.serialize(arena);
    CHECK(sv(out).find("Call-ID: rewritten@id\r\n") != std::string_view::npos);
    CHECK(sv(out).find("a84b4c76e66710") == std::string_view::npos);
}

TEST(SipModel, AddHeaderAppends) {
    std::string raw = kInvite;
    HmrSipMsg m = HmrSipMsg::parse(raw);
    HmrArena arena;
    CHECK(m.add_header("X-Custom", "yes", arena));
    CHECK_EQ(m.num_headers, 6u);
    HmrStr out = m.serialize(arena);
    CHECK(sv(out).find("X-Custom: yes\r\n") != std::string_view::npos);
}

TEST(SipModel, DeleteHeaderTombstones) {
    std::string raw = kInvite;
    HmrSipMsg m = HmrSipMsg::parse(raw);
    HmrArena arena;
    CHECK(m.delete_header("Via"));
    CHECK_EQ(m.get_header("Via").sv(), "");
    HmrStr out = m.serialize(arena);
    CHECK(sv(out).find("Via:") == std::string_view::npos);
    // Other headers remain intact.
    CHECK(sv(out).find("Call-ID: a84b4c76e66710\r\n") !=
          std::string_view::npos);
}

// ===========================================================================
// Pre-extracted URI fields
// ===========================================================================
TEST(Fields, GetRequestUriComponents) {
    std::string raw = kInvite;
    HmrSipMsg m = HmrSipMsg::parse(raw);
    CHECK_EQ(m.get_field(HMR_FIELD_REQUEST_URI_USER).sv(), "bob");
    CHECK_EQ(m.get_field(HMR_FIELD_REQUEST_URI_HOST).sv(), "example.com");
}

TEST(Fields, GetFromAndToComponents) {
    std::string raw = kInvite;
    HmrSipMsg m = HmrSipMsg::parse(raw);
    CHECK_EQ(m.get_field(HMR_FIELD_FROM_USER).sv(), "alice");
    CHECK_EQ(m.get_field(HMR_FIELD_FROM_HOST).sv(), "atlanta.com");
    CHECK_EQ(m.get_field(HMR_FIELD_TO_USER).sv(), "bob");
    CHECK_EQ(m.get_field(HMR_FIELD_TO_HOST).sv(), "example.com");
}

TEST(Fields, SetRequestUriHostRebuildsLine) {
    std::string raw = kInvite;
    HmrSipMsg m = HmrSipMsg::parse(raw);
    HmrArena arena;
    CHECK(m.set_field(HMR_FIELD_REQUEST_URI_HOST, "sbc.local", arena));
    CHECK_EQ(m.get_field(HMR_FIELD_REQUEST_URI_HOST).sv(), "sbc.local");
    HmrStr out = m.serialize(arena);
    // Method and SIP-version preserved; only the host changed.
    CHECK(sv(out).find("INVITE sip:bob@sbc.local SIP/2.0\r\n") !=
          std::string_view::npos);
}

TEST(Fields, SetFromUserUpdatesHeader) {
    std::string raw = kInvite;
    HmrSipMsg m = HmrSipMsg::parse(raw);
    HmrArena arena;
    CHECK(m.set_field(HMR_FIELD_FROM_USER, "anonymous", arena));
    // The header value is the single source of truth: get_field re-derives it.
    CHECK_EQ(m.get_field(HMR_FIELD_FROM_USER).sv(), "anonymous");
    CHECK(m.get_header("From").sv().find("sip:anonymous@atlanta.com") !=
          std::string_view::npos);
    // Display name and tag are preserved by the splice.
    CHECK(m.get_header("From").sv().find("\"Alice\"") !=
          std::string_view::npos);
    CHECK(m.get_header("From").sv().find("tag=1928") !=
          std::string_view::npos);
}

// ===========================================================================
// C ABI surface
// ===========================================================================
TEST(Abi, HeaderAndFieldCallbacks) {
    std::string raw = kInvite;
    HmrSipMsg m = HmrSipMsg::parse(raw);
    HmrContext ctx = make_ctx();

    CHECK_EQ(sv(hmr_rt_get_header(&m, hs("Call-ID"))), "a84b4c76e66710");
    CHECK_EQ(hmr_rt_is_request(&m), 1);
    CHECK_EQ(sv(hmr_rt_get_method(&m)), "INVITE");

    CHECK_EQ(hmr_rt_set_header(&m, &ctx, hs("Call-ID"), hs("new-id")), 1);
    CHECK_EQ(sv(hmr_rt_get_header(&m, hs("Call-ID"))), "new-id");

    CHECK_EQ(sv(hmr_rt_get_field(&m, HMR_FIELD_REQUEST_URI_USER)), "bob");
    CHECK_EQ(hmr_rt_set_field(&m, &ctx, HMR_FIELD_REQUEST_URI_USER,
                             hs("carol")),
             1);
    CHECK_EQ(sv(hmr_rt_get_field(&m, HMR_FIELD_REQUEST_URI_USER)), "carol");
}

TEST(Abi, MatchDispatch) {
    HmrContext ctx = make_ctx();
    // exact / exact-ci
    CHECK_EQ(hmr_rt_match(&ctx, HMR_MATCH_EXACT, hs("INVITE"), hs("INVITE"), 0),
             1);
    CHECK_EQ(hmr_rt_match(&ctx, HMR_MATCH_EXACT, hs("invite"), hs("INVITE"), 0),
             0);
    CHECK_EQ(
        hmr_rt_match(&ctx, HMR_MATCH_EXACT_CI, hs("invite"), hs("INVITE"), 0),
        1);
    // ip / ip-mask / ip-range
    CHECK_EQ(hmr_rt_match(&ctx, HMR_MATCH_IP, hs("192.168.000.001"),
                         hs("192.168.0.1"), 0),
             1);
    CHECK_EQ(hmr_rt_match(&ctx, HMR_MATCH_IP_MASK, hs("10.1.2.3"),
                         hs("10.0.0.0/8"), 0),
             1);
    CHECK_EQ(hmr_rt_match(&ctx, HMR_MATCH_IP_MASK, hs("11.1.2.3"),
                         hs("10.0.0.0/8"), 0),
             0);
    CHECK_EQ(hmr_rt_match(&ctx, HMR_MATCH_IP_RANGE, hs("192.168.0.15"),
                         hs("192.168.0.10-192.168.0.20"), 0),
             1);
    // fqdn
    CHECK_EQ(hmr_rt_match(&ctx, HMR_MATCH_FQDN, hs("Example.COM."),
                         hs("example.com"), 0),
             1);
}

TEST(Abi, RegexMatchAndCapture) {
    HmrRegexEntry entries[1] = {{"sip:([^@]+)@(.*)", 0}};
    HmrModuleInfo info{HMR_ABI_VERSION, "t", 0, 1, entries};
    HmrContext ctx = hmr::runtime::make_context(info);

    CHECK_EQ(hmr_rt_match(&ctx, HMR_MATCH_REGEX, hs("sip:alice@example.com"),
                         hs(""), 0),
             1);
    CHECK_EQ(sv(hmr_rt_get_capture(&ctx, 1)), "alice");
    CHECK_EQ(sv(hmr_rt_get_capture(&ctx, 2)), "example.com");
    CHECK_EQ(hmr_rt_match(&ctx, HMR_MATCH_REGEX, hs("tel:+1234"), hs(""), 0), 0);
}

TEST(Abi, UriGetSet) {
    HmrContext ctx = make_ctx();
    HmrStr from = hs("\"Bob\" <sip:bob@biloxi.com>;tag=99");
    CHECK_EQ(sv(hmr_rt_uri_get(&ctx, from, HMR_URI_USER)), "bob");
    CHECK_EQ(sv(hmr_rt_uri_get(&ctx, from, HMR_URI_HOST)), "biloxi.com");
    HmrStr rebuilt = hmr_rt_uri_set(&ctx, from, HMR_URI_HOST, hs("sbc.net"));
    CHECK(sv(rebuilt).find("sip:bob@sbc.net") != std::string_view::npos);
    CHECK(sv(rebuilt).find("tag=99") != std::string_view::npos);
}

TEST(Abi, ValueBuilderAndReset) {
    HmrContext ctx = make_ctx();
    ctx.set_var(HMR_VAR_REALM, "core");
    hmr_rt_val_reset(&ctx);
    hmr_rt_val_append_lit(&ctx, hs("realm="));
    hmr_rt_val_append_var(&ctx, HMR_VAR_REALM);
    CHECK_EQ(sv(hmr_rt_val_finish(&ctx)), "realm=core");

    // reset_for_apply rewinds the arena for the next packet.
    char* p = ctx.arena.alloc(100);
    CHECK(p != nullptr);
    ctx.reset_for_apply();
    CHECK_EQ(ctx.arena.remaining(), HmrArena::kCapacity);
}

TEST(Abi, RejectIsRecorded) {
    HmrContext ctx = make_ctx();
    hmr_rt_reject(&ctx, 403, hs("Forbidden"));
    CHECK(ctx.rejected);
    CHECK_EQ(ctx.reject_code, 403u);
    CHECK_EQ(ctx.reject_reason, std::string("Forbidden"));
}

TEST_MAIN()
