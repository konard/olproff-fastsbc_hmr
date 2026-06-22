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

// uri.cpp — implementation of parse_uri (see uri.hpp). All component views point
// into the caller's input; nothing is copied or allocated.

#include "hmr/runtime/uri.hpp"

#include <cstring>

namespace hmr::runtime {
namespace {

std::string_view trim(std::string_view s) noexcept {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.remove_prefix(1);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t')) s.remove_suffix(1);
    return s;
}

}  // namespace

ParsedUri parse_uri(std::string_view value) noexcept {
    ParsedUri u;
    std::string_view s = value;

    // Display name + optional angle brackets.
    std::size_t lt = s.find('<');
    if (lt != std::string_view::npos) {
        u.display = trim(s.substr(0, lt));
        if (u.display.size() >= 2 && u.display.front() == '"' &&
            u.display.back() == '"')
            u.display = u.display.substr(1, u.display.size() - 2);
        std::size_t gt = s.find('>', lt + 1);
        u.angled = true;
        std::size_t inner_off = lt + 1;
        std::string_view inner =
            s.substr(inner_off, (gt == std::string_view::npos ? s.size() : gt) -
                                    inner_off);
        u.addr_start = inner_off;
        s = inner;
    } else {
        u.addr_start = 0;
    }

    // scheme:
    std::size_t colon = s.find(':');
    std::size_t cursor = 0;
    if (colon != std::string_view::npos &&
        (s.substr(0, colon) == "sip" || s.substr(0, colon) == "sips" ||
         s.substr(0, colon) == "tel")) {
        u.scheme = s.substr(0, colon);
        cursor = colon + 1;
    }

    std::string_view rest = s.substr(cursor);
    std::size_t at = rest.find('@');
    std::string_view hostport;
    if (at != std::string_view::npos) {
        u.user = rest.substr(0, at);
        hostport = rest.substr(at + 1);
    } else {
        hostport = rest;
    }

    // host[:port][;params]
    std::size_t hp_end = hostport.find_first_of(";?>");
    std::string_view hp = hostport.substr(
        0, hp_end == std::string_view::npos ? hostport.size() : hp_end);
    if (hp_end != std::string_view::npos) u.tail = hostport.substr(hp_end);

    std::size_t port_colon = hp.rfind(':');
    if (port_colon != std::string_view::npos) {
        u.host = hp.substr(0, port_colon);
        u.port = hp.substr(port_colon + 1);
    } else {
        u.host = hp;
    }

    // Compute addr_end (offset relative to the original value) for rebuilding.
    std::size_t host_off_in_s =
        static_cast<std::size_t>(u.host.data() - s.data());
    u.addr_end = u.addr_start + host_off_in_s + u.host.size() +
                 (u.port.empty() ? 0 : u.port.size() + 1);
    return u;
}

HmrStr rebuild_uri(HmrArena& arena, std::string_view value,
                   std::uint32_t element_type, std::string_view nv) noexcept {
    const ParsedUri u = parse_uri(value);
    const std::size_t len = value.size();
    const char* base = value.data();

    // Describe the rewrite as: result = value[0:start] + pre + nv + post +
    // value[end:]. Defaults to a whole-value replacement.
    std::size_t start = 0;
    std::size_t end = len;
    std::string_view pre;
    std::string_view post;

    auto off = [&](std::string_view part) -> std::size_t {
        return static_cast<std::size_t>(part.data() - base);
    };

    switch (element_type) {
        case HMR_URI_USER:
            if (u.user.data()) {
                start = off(u.user);
                end = start + u.user.size();
            } else if (u.host.data()) {
                start = end = off(u.host);  // insert "nv@" before host
                post = "@";
            }
            break;
        case HMR_URI_HOST:
            if (u.host.data()) {
                start = off(u.host);
                end = start + u.host.size();
            } else {
                return arena.dup(value);  // no host to rewrite; leave unchanged
            }
            break;
        case HMR_URI_PORT:
            if (u.port.data()) {
                start = off(u.port);
                end = start + u.port.size();
            } else if (u.host.data()) {
                start = end = off(u.host) + u.host.size();  // insert ":nv"
                pre = ":";
            } else {
                return arena.dup(value);
            }
            break;
        case HMR_URI_DISPLAY:
            if (u.display.data()) {
                start = off(u.display);
                end = start + u.display.size();
            } else {
                start = end = 0;  // prepend "nv " before the addr-spec
                post = " ";
            }
            break;
        case HMR_URI_WHOLE:
        default:
            start = 0;
            end = len;
            break;
    }

    const std::uint32_t total = static_cast<std::uint32_t>(
        start + pre.size() + nv.size() + post.size() + (len - end));
    char* dst = arena.alloc(total);
    if (dst == nullptr && total != 0) return HmrStr{nullptr, 0};

    char* p = dst;
    auto put = [&](std::string_view s) {
        if (!s.empty()) {
            std::memcpy(p, s.data(), s.size());
            p += s.size();
        }
    };
    put(value.substr(0, start));
    put(pre);
    put(nv);
    put(post);
    put(value.substr(end));
    return HmrStr{dst, total};
}

}  // namespace hmr::runtime
