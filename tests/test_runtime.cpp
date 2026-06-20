// SPDX-License-Identifier: MIT
//
// Unit tests for the C-ABI runtime: SIP model, value builder, regex captures
// and URI element access. These exercise the same callbacks the generated
// module calls, so they validate the runtime contract without needing LLVM.

#include <cstring>
#include <string>

#include "Test.hpp"
#include "hmr/runtime/Runtime.hpp"
#include "hmr/runtime/SipMessage.hpp"
#include "hmr/runtime/hmr_runtime.h"

namespace {
HmrStr S(std::string_view s) {
    return HmrStr{s.data(), static_cast<std::uint32_t>(s.size())};
}
std::string str(HmrStr s) {
    return std::string(s.data ? s.data : "", s.len);
}
}  // namespace

TEST(Runtime, SipParseRequest) {
    auto msg = HmrSipMsg::parse(
        "INVITE sip:bob@example.com SIP/2.0\r\n"
        "From: <sip:alice@a.com>;tag=1\r\n"
        "To: <sip:bob@example.com>\r\n"
        "CSeq: 1 INVITE\r\n"
        "\r\n");
    CHECK(msg.isRequest);
    CHECK_EQ(msg.method, std::string("INVITE"));
    CHECK_EQ(std::string(msg.getHeader("from")), std::string("<sip:alice@a.com>;tag=1"));
    CHECK_EQ(std::string(msg.getHeader("TO")), std::string("<sip:bob@example.com>"));
    CHECK(msg.getHeader("missing").empty());
}

TEST(Runtime, SipParseResponse) {
    auto msg = HmrSipMsg::parse(
        "SIP/2.0 200 OK\r\n"
        "CSeq: 1 INVITE\r\n"
        "\r\n");
    CHECK(!msg.isRequest);
    CHECK_EQ(msg.statusCode, std::uint32_t{200});
}

TEST(Runtime, HeaderMutationCallbacks) {
    HmrSipMsg msg;
    hmr_rt_add_header(&msg, S("X-A"), S("1"));
    hmr_rt_add_header(&msg, S("X-A"), S("2"));
    CHECK_EQ(str(hmr_rt_get_header(&msg, S("x-a"))), std::string("1"));

    hmr_rt_set_header(&msg, S("x-a"), S("99"));
    CHECK_EQ(str(hmr_rt_get_header(&msg, S("X-A"))), std::string("99"));

    CHECK(hmr_rt_delete_header(&msg, S("x-a")) != 0);
    CHECK(hmr_rt_get_header(&msg, S("X-A")).len == 0);
}

TEST(Runtime, ValueBuilderConcatenates) {
    HmrModuleInfo info{HMR_ABI_VERSION, "t", /*slots=*/2, 0, nullptr};
    auto ctx = hmr::runtime::makeContext(info);
    ctx.setVar(HMR_VAR_LOCAL_IP, "10.0.0.1");
    hmr_rt_store(&ctx, 0, S("trunkA"));

    hmr_rt_val_reset(&ctx);
    hmr_rt_val_append_lit(&ctx, S("sip:"));
    hmr_rt_val_append_slot(&ctx, 0);
    hmr_rt_val_append_lit(&ctx, S("@"));
    hmr_rt_val_append_var(&ctx, HMR_VAR_LOCAL_IP);
    CHECK_EQ(str(hmr_rt_val_finish(&ctx)), std::string("sip:trunkA@10.0.0.1"));
}

TEST(Runtime, RegexMatchAndCapture) {
    HmrRegexEntry entries[1] = {{"sip:([^@]+)@(.*)", 0}};
    HmrModuleInfo info{HMR_ABI_VERSION, "t", 0, 1, entries};
    auto ctx = hmr::runtime::makeContext(info);

    CHECK(hmr_rt_regex_match(&ctx, 0, S("sip:alice@example.com")) != 0);
    CHECK_EQ(str(hmr_rt_get_capture(&ctx, 1)), std::string("alice"));
    CHECK_EQ(str(hmr_rt_get_capture(&ctx, 2)), std::string("example.com"));

    CHECK(hmr_rt_regex_match(&ctx, 0, S("tel:+1234")) == 0);
}

TEST(Runtime, UriGetElements) {
    HmrModuleInfo info{HMR_ABI_VERSION, "t", 0, 0, nullptr};
    auto ctx = hmr::runtime::makeContext(info);
    HmrStr v = S("\"Alice\" <sip:alice@host.com:5060>");
    CHECK_EQ(str(hmr_rt_uri_get(&ctx, v, HMR_URI_USER)), std::string("alice"));
    CHECK_EQ(str(hmr_rt_uri_get(&ctx, v, HMR_URI_HOST)), std::string("host.com"));
    CHECK_EQ(str(hmr_rt_uri_get(&ctx, v, HMR_URI_PORT)), std::string("5060"));
}

TEST(Runtime, UriSetUserRewrites) {
    HmrModuleInfo info{HMR_ABI_VERSION, "t", 0, 0, nullptr};
    auto ctx = hmr::runtime::makeContext(info);
    HmrStr v = S("<sip:alice@host.com>");
    std::string out = str(hmr_rt_uri_set(&ctx, v, HMR_URI_USER, S("anonymous")));
    CHECK(out.find("anonymous@host.com") != std::string::npos);
    CHECK(out.find("alice") == std::string::npos);
}

TEST(Runtime, RejectIsRecorded) {
    HmrModuleInfo info{HMR_ABI_VERSION, "t", 0, 0, nullptr};
    auto ctx = hmr::runtime::makeContext(info);
    hmr_rt_reject(&ctx, 403, S("Forbidden"));
    CHECK(ctx.rejected);
    CHECK_EQ(ctx.rejectCode, std::uint32_t{403});
    CHECK_EQ(ctx.rejectReason, std::string("Forbidden"));
}
