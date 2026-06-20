// SPDX-License-Identifier: MIT
//
// Runtime.hpp — per-application context behind the opaque HmrContext handle.
//
// One HmrContext is bound to one worker thread. It owns all the scratch state a
// generated module needs (built-in variables, regex match captures, store/load
// slots, a value-builder buffer) and the precompiled regexes described by the
// module's HmrModuleInfo. Every buffer is reused across applications, so steady
// state header manipulation performs no heap allocation per packet.

#pragma once

#include <array>
#include <cstdint>
#include <regex>
#include <string>
#include <string_view>
#include <vector>

#include "hmr/runtime/SipMessage.hpp"
#include "hmr/runtime/hmr_runtime.h"

// Concrete definition of the C ABI's opaque per-apply context handle.
struct HmrContext {
    // Built-in variable values ($LOCAL_IP, ...), indexed by HmrVarId.
    std::array<std::string, HMR_VAR_MAX> vars;

    // Precompiled regexes from HmrModuleInfo (built once at prepare()).
    std::vector<std::regex> regexes;

    // store/load slots for cross-rule back-references.
    std::vector<std::string> slots;

    // Most recent regex match — submatches reference the subject buffer, so no
    // per-capture allocation. Valid until the next hmr_rt_regex_match call.
    std::cmatch lastMatch;
    bool hasMatch = false;

    // Reusable scratch buffer for new-value construction.
    std::string scratch;

    // Diagnostics / control side-channel.
    std::vector<std::string> logs;
    bool rejected = false;
    std::uint32_t rejectCode = 0;
    std::string rejectReason;

    // Bind a built-in variable value (host side, before apply).
    void setVar(HmrVarId id, std::string value);

    // Build regexes and size slots from the loaded module descriptor.
    void prepare(const HmrModuleInfo& info);

    // Clear per-application state (captures, scratch, verdict) while retaining
    // capacity. Call once before each hmr_apply.
    void resetForApply();
};

namespace hmr::runtime {

using Context = ::HmrContext;

// Construct a context already prepared for `info` (compiles regexes, sizes
// slots). Throws std::regex_error if a pattern is invalid.
[[nodiscard]] Context makeContext(const HmrModuleInfo& info);

}  // namespace hmr::runtime
