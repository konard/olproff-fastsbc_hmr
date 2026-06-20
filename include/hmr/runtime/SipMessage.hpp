// SPDX-License-Identifier: MIT
//
// SipMessage.hpp — minimal SIP message model used by the runtime.
//
// `HmrSipMsg` is the concrete C++ type behind the opaque handle declared in the
// C ABI (hmr_runtime.h). Defining it here once — rather than casting an
// unrelated struct — keeps the host/runtime side strongly typed while the
// generated module still sees only an opaque pointer.
//
// The model is deliberately small: an (ordered) list of headers plus the
// request line or status line. Header name lookups are case-insensitive per
// RFC 3261 §7.3. It is not a full SIP stack; it carries exactly what HMR rules
// read and mutate.

#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "hmr/runtime/hmr_runtime.h"

// Concrete definition of the C ABI's opaque SIP message handle.
struct HmrSipMsg {
    struct Header {
        std::string name;
        std::string value;
    };

    bool isRequest = true;
    std::string method;        // INVITE, REGISTER, ... (requests)
    std::string requestUri;    // request line URI (requests)
    std::uint32_t statusCode = 0;   // 100..699 (responses)
    std::string reasonPhrase;  // (responses)
    std::vector<Header> headers;

    // Case-insensitive header access. getHeader returns an empty view when the
    // header is absent.
    [[nodiscard]] std::string_view getHeader(std::string_view name) const;
    [[nodiscard]] const Header* find(std::string_view name) const;

    // setHeader rewrites the first matching header, or appends if none exists.
    bool setHeader(std::string_view name, std::string_view value);
    void addHeader(std::string_view name, std::string_view value);
    bool deleteHeader(std::string_view name);  // removes all matches

    // Parse a SIP message from raw text (CRLF or LF line endings). Tolerant:
    // used by the CLI and tests, not on the per-packet fast path.
    [[nodiscard]] static HmrSipMsg parse(std::string_view raw);

    // Serialize back to wire text (CRLF). For diagnostics / round-trip tests.
    [[nodiscard]] std::string toString() const;
};

namespace hmr::runtime {

using SipMessage = ::HmrSipMsg;

// Case-insensitive ASCII equality, shared by the message model and runtime.
[[nodiscard]] bool iequals(std::string_view a, std::string_view b) noexcept;

}  // namespace hmr::runtime
