// SPDX-License-Identifier: MIT
//
// uri.hpp — minimal, allocation-free SIP URI splitter shared by the SIP model
// (to pre-extract user/host component slices) and the C ABI (uri_get/uri_set).
//
// It parses values of the form
//     [ display-name ] [<] (sip|sips|tel):user@host[:port][;params] [>] [;hdr]
// into component views that point back into the input — no copying.

#pragma once

#include <cstddef>
#include <string_view>

#include "hmr/runtime/arena.hpp"
#include "hmr/runtime/hmr_runtime.h"

namespace hmr::runtime {

struct ParsedUri {
    std::string_view display;
    std::string_view scheme;
    std::string_view user;
    std::string_view host;
    std::string_view port;
    std::string_view tail;       // params / trailing text after host[:port]
    bool angled = false;
    std::size_t addr_start = 0;  // offset of scheme in the original value
    std::size_t addr_end = 0;    // offset just past host[:port]
};

[[nodiscard]] ParsedUri parse_uri(std::string_view value) noexcept;

// Rewrite a single URI element (HMR_URI_*) of `value`, writing the rebuilt URI
// into `arena` and returning a view of it. Everything around the element is
// preserved verbatim; an absent user/display/port is inserted where it belongs.
// Returns an empty view (data == nullptr) only on arena overflow. Shared by the
// ABI's uri_set and the SIP model's set_field so both rewrite identically.
[[nodiscard]] HmrStr rebuild_uri(HmrArena& arena, std::string_view value,
                                 std::uint32_t element_type,
                                 std::string_view new_value) noexcept;

}  // namespace hmr::runtime
