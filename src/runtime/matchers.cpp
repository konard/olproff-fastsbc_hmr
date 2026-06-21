// SPDX-License-Identifier: MIT
//
// matchers.cpp — implementation of the specialized match-val-type matchers
// declared in matchers.hpp. All addresses are normalized to network byte order
// so that address equality, subnet membership and range tests reduce to a plain
// memcmp over the relevant byte count (big-endian byte order is numeric order).

#include "hmr/runtime/matchers.hpp"

#include <cstring>

namespace hmr::runtime {
namespace {

// ---- small character helpers (locale-independent) -------------------------
constexpr char to_lower(char c) noexcept {
    return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
}
constexpr bool is_digit(char c) noexcept { return c >= '0' && c <= '9'; }
constexpr int hex_val(char c) noexcept {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

std::string_view trim(std::string_view s) noexcept {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.remove_prefix(1);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t')) s.remove_suffix(1);
    return s;
}

// Parse an unsigned decimal in [0, max]. The whole view must be digits and
// non-empty. Returns false on overflow / bad input.
bool parse_uint(std::string_view s, std::uint32_t max, std::uint32_t& out) noexcept {
    if (s.empty()) return false;
    std::uint64_t v = 0;
    for (char c : s) {
        if (!is_digit(c)) return false;
        v = v * 10 + static_cast<std::uint32_t>(c - '0');
        if (v > max) return false;
    }
    out = static_cast<std::uint32_t>(v);
    return true;
}

// ---- IPv4 -----------------------------------------------------------------
// Strict dotted-quad: exactly four decimal octets, each 0..255.
bool parse_ipv4(std::string_view s, std::uint8_t out[4]) noexcept {
    int part = 0;
    std::size_t start = 0;
    for (std::size_t i = 0; i <= s.size(); ++i) {
        if (i == s.size() || s[i] == '.') {
            if (part >= 4) return false;
            std::uint32_t octet = 0;
            if (!parse_uint(s.substr(start, i - start), 255, octet)) return false;
            out[part++] = static_cast<std::uint8_t>(octet);
            start = i + 1;
        }
    }
    return part == 4;
}

// ---- IPv6 -----------------------------------------------------------------
// Parse a colon-separated list of hextets (no "::") into bytes. The final token
// may be a dotted-quad IPv4 tail. Returns the byte count, or -1 on error.
int parse_v6_part(std::string_view part, std::uint8_t* out) noexcept {
    if (part.empty()) return 0;
    int n = 0;
    std::size_t start = 0;
    for (std::size_t i = 0; i <= part.size(); ++i) {
        if (i == part.size() || part[i] == ':') {
            std::string_view tok = part.substr(start, i - start);
            if (tok.empty()) return -1;  // stray / doubled colon
            if (tok.find('.') != std::string_view::npos) {
                // Embedded IPv4 tail — only valid as the final token.
                if (i != part.size()) return -1;
                std::uint8_t v4[4];
                if (!parse_ipv4(tok, v4)) return -1;
                if (n + 4 > 16) return -1;
                std::memcpy(out + n, v4, 4);
                n += 4;
            } else {
                if (tok.size() > 4) return -1;
                int value = 0;
                for (char c : tok) {
                    int h = hex_val(c);
                    if (h < 0) return -1;
                    value = (value << 4) | h;
                }
                if (n + 2 > 16) return -1;
                out[n++] = static_cast<std::uint8_t>((value >> 8) & 0xFF);
                out[n++] = static_cast<std::uint8_t>(value & 0xFF);
            }
            start = i + 1;
        }
    }
    return n;
}

bool parse_ipv6(std::string_view s, std::uint8_t out[16]) noexcept {
    // Strip a surrounding "[...]" and any "%zone" suffix.
    if (s.size() >= 2 && s.front() == '[' && s.back() == ']')
        s = s.substr(1, s.size() - 2);
    if (auto pct = s.find('%'); pct != std::string_view::npos) s = s.substr(0, pct);
    if (s.empty()) return false;

    std::uint8_t buf[16] = {};
    std::size_t dc = s.find("::");
    if (dc != std::string_view::npos) {
        if (s.find("::", dc + 1) != std::string_view::npos) return false;  // only one
        std::uint8_t head[16], tail[16];
        int hn = parse_v6_part(s.substr(0, dc), head);
        int tn = parse_v6_part(s.substr(dc + 2), tail);
        if (hn < 0 || tn < 0) return false;
        if (hn + tn > 14) return false;  // "::" must elide at least one group
        std::memcpy(buf, head, static_cast<std::size_t>(hn));
        std::memcpy(buf + 16 - tn, tail, static_cast<std::size_t>(tn));
    } else {
        std::uint8_t full[16];
        int fn = parse_v6_part(s, full);
        if (fn != 16) return false;
        std::memcpy(buf, full, 16);
    }
    std::memcpy(out, buf, 16);
    return true;
}

// Compare two same-family addresses numerically (network byte order).
int addr_cmp(const IpAddr& a, const IpAddr& b) noexcept {
    return std::memcmp(a.bytes.data(), b.bytes.data(), a.byte_len());
}

}  // namespace

IpAddr parse_ip(std::string_view text) noexcept {
    text = trim(text);
    IpAddr addr;
    std::uint8_t v4[4];
    if (parse_ipv4(text, v4)) {
        addr.family = IpFamily::V4;
        std::memcpy(addr.bytes.data(), v4, 4);
        return addr;
    }
    std::uint8_t v6[16];
    if (parse_ipv6(text, v6)) {
        addr.family = IpFamily::V6;
        std::memcpy(addr.bytes.data(), v6, 16);
        return addr;
    }
    return addr;  // invalid
}

bool match_exact(std::string_view subject, std::string_view pattern,
                 bool case_insensitive) noexcept {
    if (subject.size() != pattern.size()) return false;
    if (!case_insensitive) return subject == pattern;
    for (std::size_t i = 0; i < subject.size(); ++i)
        if (to_lower(subject[i]) != to_lower(pattern[i])) return false;
    return true;
}

bool match_ip(std::string_view subject, std::string_view pattern) noexcept {
    IpAddr a = parse_ip(subject);
    IpAddr b = parse_ip(pattern);
    if (!a.valid() || !b.valid() || a.family != b.family) return false;
    return addr_cmp(a, b) == 0;
}

bool match_ip_mask(std::string_view subject, std::string_view pattern) noexcept {
    std::size_t slash = pattern.find('/');
    if (slash == std::string_view::npos) return false;
    std::string_view net_text = trim(pattern.substr(0, slash));
    std::string_view suffix = trim(pattern.substr(slash + 1));

    IpAddr subj = parse_ip(subject);
    IpAddr net = parse_ip(net_text);
    if (!subj.valid() || !net.valid() || subj.family != net.family) return false;

    const std::uint8_t width = net.width_bits();
    std::uint32_t prefix = 0;

    if (IpAddr dotted = parse_ip(suffix);
        dotted.valid() && dotted.family == net.family) {
        // Dotted netmask: must be contiguous leading ones; derive prefix length.
        bool seen_zero = false;
        for (std::uint8_t i = 0; i < dotted.byte_len(); ++i) {
            std::uint8_t byte = dotted.bytes[i];
            for (int bit = 7; bit >= 0; --bit) {
                bool one = (byte >> bit) & 1u;
                if (one) {
                    if (seen_zero) return false;  // non-contiguous mask
                    ++prefix;
                } else {
                    seen_zero = true;
                }
            }
        }
    } else if (!parse_uint(suffix, width, prefix)) {
        return false;
    }

    // Compare the first `prefix` bits of subject and network.
    std::uint32_t full_bytes = prefix / 8;
    std::uint32_t rem_bits = prefix % 8;
    if (std::memcmp(subj.bytes.data(), net.bytes.data(), full_bytes) != 0)
        return false;
    if (rem_bits != 0) {
        std::uint8_t mask = static_cast<std::uint8_t>(0xFF << (8 - rem_bits));
        if ((subj.bytes[full_bytes] & mask) != (net.bytes[full_bytes] & mask))
            return false;
    }
    return true;
}

bool match_ip_range(std::string_view subject, std::string_view pattern) noexcept {
    std::size_t dash = pattern.find('-');
    if (dash == std::string_view::npos) return false;
    IpAddr lo = parse_ip(trim(pattern.substr(0, dash)));
    IpAddr hi = parse_ip(trim(pattern.substr(dash + 1)));
    IpAddr subj = parse_ip(subject);
    if (!lo.valid() || !hi.valid() || !subj.valid()) return false;
    if (lo.family != hi.family || lo.family != subj.family) return false;
    return addr_cmp(subj, lo) >= 0 && addr_cmp(subj, hi) <= 0;
}

bool match_fqdn(std::string_view subject, std::string_view pattern) noexcept {
    // A single trailing dot is insignificant (the root label).
    if (subject.size() > 1 && subject.back() == '.') subject.remove_suffix(1);
    if (pattern.size() > 1 && pattern.back() == '.') pattern.remove_suffix(1);
    if (subject.empty() || pattern.empty()) return false;
    if (subject.size() != pattern.size()) return false;
    for (std::size_t i = 0; i < subject.size(); ++i)
        if (to_lower(subject[i]) != to_lower(pattern[i])) return false;
    return true;
}

}  // namespace hmr::runtime
