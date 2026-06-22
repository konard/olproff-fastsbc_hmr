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

// sip_message.hpp — zero-copy SIP message model (review point #4).
//
// The first draft modelled a message as std::vector<Header> of std::string, so
// every packet copied each header name and value onto the heap and freed them
// afterwards — exactly the per-packet churn an SBC fast path cannot afford. This
// model removes all of it:
//
//   * The input packet is immutable (`raw`, `raw_len`). It is never modified.
//   * Header names/values, the request line, and URI components are *slices*
//     (pointer + length) into `raw`. Parsing copies nothing.
//   * Mutations are written into a per-worker bump arena (HmrArena, owned by the
//     HmrContext). A modified header value's slice simply re-points into the
//     arena; an added header's name/value are duped into the arena. Deletes are
//     tombstones — no array shifting, no frees.
//   * The whole message resets in O(1) (the arena is rewound, num_headers reset).
//
// `HmrSipMsg` is the concrete type behind the opaque handle in the C ABI
// (hmr_runtime.h). The generated module sees only the opaque pointer.
//
// Reconciling the reference struct with the generic header ABI: the reference
// sketch stored request-URI / From / To components as standalone slices plus
// `mod_*` arena pointers ("NULL = unmodified"). We keep that fast path for the
// *request-URI* (the request line is not a header, so it has no other home) via
// a single `mod_request_uri` override: a set_field rebuilds the whole request-URI
// into the arena and re-points this slice, reads re-derive the user/host
// components from it, and serialize splices it back into the start line
// (preserving the method and SIP-version byte-for-byte). From/To components are
// not cached separately: they are derived from — and written back through — the
// From/To header, so the header value stays the single source of truth and reads
// can never go stale within a packet. Both honour "modifications live in the
// arena, originals stay zero-copy in raw".

#pragma once

#include <cstdint>
#include <string_view>

#include "hmr/runtime/arena.hpp"
#include "hmr/runtime/hmr_runtime.h"

// Concrete definition of the C ABI's opaque SIP message handle.
struct HmrSipMsg {
    // A non-owning view into `raw` or the arena. Mirrors HmrStr but with helpers.
    struct Slice {
        const char* data = nullptr;
        std::uint32_t len = 0;

        [[nodiscard]] bool empty() const noexcept { return len == 0; }
        [[nodiscard]] bool present() const noexcept { return data != nullptr; }
        [[nodiscard]] std::string_view sv() const noexcept {
            return std::string_view{data ? data : "", len};
        }
        [[nodiscard]] HmrStr str() const noexcept { return HmrStr{data, len}; }

        static Slice of(std::string_view s) noexcept {
            return Slice{s.data(), static_cast<std::uint32_t>(s.size())};
        }
        static Slice of(HmrStr s) noexcept { return Slice{s.data, s.len}; }
    };

    struct Header {
        Slice name;
        Slice value;
        bool removed = false;  // tombstone: kept in the array, skipped on output
    };

    // ---- Immutable input -------------------------------------------------
    const char* raw = nullptr;
    std::uint32_t raw_len = 0;

    // ---- Headers (fixed array, no heap) ----------------------------------
    Header headers[HMR_MAX_HEADERS]{};
    std::uint32_t num_headers = 0;

    // ---- Request / status line -------------------------------------------
    bool is_request = true;
    Slice start_line;            // entire first line (no CRLF), verbatim in raw
    Slice method;                // requests: method token (slice into start_line)
    Slice request_uri;           // requests: request-URI (slice into start_line)
    std::uint32_t status_code = 0;  // responses
    Slice reason_phrase;         // responses (slice into start_line)
    Slice body;                  // bytes after the blank line, verbatim (may be empty)

    // ---- Request-URI override (arena; absent => unmodified) --------------
    // The whole rewritten request-URI. When present it supersedes `request_uri`
    // for reads (component extraction) and serialization (spliced into the line).
    Slice mod_request_uri;

    // ---- Header operations (case-insensitive names, RFC 3261 §7.3) -------
    [[nodiscard]] int find(std::string_view name) const noexcept;  // index or -1
    [[nodiscard]] Slice get_header(std::string_view name) const noexcept;

    // set rewrites the first live match (value duped into the arena), or appends
    // if none exists. Returns false on arena/array overflow.
    bool set_header(std::string_view name, std::string_view value,
                    HmrArena& arena) noexcept;
    bool add_header(std::string_view name, std::string_view value,
                    HmrArena& arena) noexcept;
    bool delete_header(std::string_view name) noexcept;  // tombstones all matches

    // ---- Pre-extracted URI component fields (HmrField) -------------------
    [[nodiscard]] Slice get_field(std::uint32_t field) const noexcept;
    bool set_field(std::uint32_t field, std::string_view value,
                   HmrArena& arena) noexcept;

    // ---- Parsing / serialization ----------------------------------------
    // Parse `raw` in place: fills slices, never copies. `raw` must outlive the
    // message. Tolerant of CRLF or LF line endings.
    static HmrSipMsg parse(std::string_view raw) noexcept;

    // Serialize the (possibly mutated) message to wire form (CRLF) into `arena`,
    // returning a view of the result. No heap allocation; empty on overflow.
    [[nodiscard]] HmrStr serialize(HmrArena& arena) const noexcept;
};

namespace hmr::runtime {

using SipMessage = ::HmrSipMsg;

// Case-insensitive ASCII equality, shared by the message model and runtime.
[[nodiscard]] bool iequals(std::string_view a, std::string_view b) noexcept;

}  // namespace hmr::runtime
