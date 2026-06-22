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
