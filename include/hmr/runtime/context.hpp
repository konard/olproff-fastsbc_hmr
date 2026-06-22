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

// context.hpp — per-worker application context behind the opaque HmrContext.
//
// One HmrContext is bound to one worker thread. It owns:
//   * the zero-copy arena (HmrArena) into which all per-packet mutations are
//     written and which is reset in O(1) between packets,
//   * the built-in variable values ($LOCAL_IP, ...),
//   * the precompiled regexes from the module's HmrModuleInfo,
//   * store/load slots, the regex capture state, a value-builder scratch buffer,
//     and the diagnostics / reject side-channel.
//
// Every buffer is reused across applications: in steady state, applying a
// ruleset to a packet performs no heap allocation. The arena replaces the old
// std::string-per-header message storage (review point #4); the std::string
// members here are reused scratch, cleared (not freed) per packet.

#pragma once

#include <array>
#include <cstdint>
#include <regex>
#include <string>
#include <vector>

#include "hmr/runtime/arena.hpp"
#include "hmr/runtime/hmr_runtime.h"

// Concrete definition of the C ABI's opaque per-apply context handle.
struct HmrContext {
    // Per-worker bump arena: all modified/added header values and rebuilt URIs
    // live here. Reset (rewound) at the start of every apply.
    HmrArena arena;

    // Built-in variable values ($LOCAL_IP, ...), indexed by HmrVarId. Host-set,
    // persistent across packets.
    std::array<std::string, HMR_VAR_MAX> vars;

    // Precompiled regexes from HmrModuleInfo (built once at prepare()).
    std::vector<std::regex> regexes;

    // store/load slots for cross-rule back-references.
    std::vector<std::string> slots;

    // Most recent regex match — submatches reference the subject buffer, so no
    // per-capture allocation. Valid until the next match call.
    std::cmatch last_match;
    bool has_match = false;

    // Reusable scratch buffer for the value builder. The result is duped into
    // the arena by whichever set operation stores it, so intermediate value
    // construction never consumes arena space.
    std::string scratch;

    // Diagnostics / control side-channel.
    std::vector<std::string> logs;
    bool rejected = false;
    std::uint32_t reject_code = 0;
    std::string reject_reason;

    // Bind a built-in variable value (host side, before apply).
    void set_var(HmrVarId id, std::string value);

    // Build regexes and size slots from the loaded module descriptor.
    void prepare(const HmrModuleInfo& info);

    // Clear per-application state (arena, captures, scratch, verdict) while
    // retaining capacity. Call once before each hmr_apply.
    void reset_for_apply() noexcept;
};

namespace hmr::runtime {

using Context = ::HmrContext;

// Construct a context already prepared for `info` (compiles regexes, sizes
// slots). Throws std::regex_error if a pattern is invalid.
[[nodiscard]] Context make_context(const HmrModuleInfo& info);

}  // namespace hmr::runtime
