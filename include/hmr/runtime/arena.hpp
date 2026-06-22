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

// arena.hpp — per-worker bump allocator for the zero-copy SIP model.
//
// Review point #4: the first draft modelled a SIP message as std::vector<Header>
// of std::string, so every packet copied rows and churned the heap. The arena
// removes that: a worker owns one fixed buffer, modified/added header values are
// bump-allocated into it, and the whole arena is reset in O(1) between packets.
// No per-packet allocation, no frees, cache-friendly contiguous storage.
//
//   reset()  -> rewind to empty (O(1))
//   alloc()  -> hand out the next `n` bytes (nullptr when exhausted)
//   dup()    -> copy a view into the arena and return a stable HmrStr to it
//
// The arena lives inside HmrContext (one per worker thread), never per packet.

#pragma once

#include <array>
#include <cstdint>
#include <cstring>
#include <string_view>

#include "hmr/runtime/hmr_runtime.h"

struct HmrArena {
    // 64 KiB per worker, per the reference design. Large enough for the rewrites
    // a ruleset performs on one packet; adjust here if a workload needs more.
    static constexpr std::uint32_t kCapacity = 64u * 1024u;

    std::array<char, kCapacity> buffer{};
    std::uint32_t used = 0;

    // O(1) reset — just rewind the bump pointer; storage is reused in place.
    void reset() noexcept { used = 0; }

    [[nodiscard]] std::uint32_t remaining() const noexcept {
        return kCapacity - used;
    }

    // Hand out `n` contiguous bytes, or nullptr if the arena is exhausted. The
    // caller must check: exhaustion is a no-op at the ABI boundary, never UB.
    [[nodiscard]] char* alloc(std::uint32_t n) noexcept {
        if (n > remaining()) return nullptr;
        char* p = buffer.data() + used;
        used += n;
        return p;
    }

    // Copy [src, src+len) into the arena and return a stable view. On overflow
    // returns an empty view (data == nullptr).
    [[nodiscard]] HmrStr dup(const char* src, std::uint32_t len) noexcept {
        if (len == 0) return HmrStr{src, 0};
        char* p = alloc(len);
        if (p == nullptr) return HmrStr{nullptr, 0};
        std::memcpy(p, src, len);
        return HmrStr{p, len};
    }

    [[nodiscard]] HmrStr dup(std::string_view s) noexcept {
        return dup(s.data(), static_cast<std::uint32_t>(s.size()));
    }
};
