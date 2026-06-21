// SPDX-License-Identifier: MIT
//
// matchers.hpp — specialized match-val-type matchers.
//
// Oracle HMR distinguishes several kinds of value comparison (see the HMR guide,
// "match-val-type"). Routing every one of them through std::regex — as the first
// draft did — is both slow and semantically wrong: an exact comparison must be a
// byte compare, and an address comparison must understand IP arithmetic. This
// header declares the specialized matchers the runtime dispatches to:
//
//   exact     byte / case-insensitive comparison        match_exact
//   regex     std::regex (lives in the context)         hmr_rt_regex_match
//   ip        canonical address equality (v4 & v6)      match_ip
//   ip-mask   subnet membership (CIDR or dotted mask)   match_ip_mask
//   ip-range  inclusive lo-hi membership                match_ip_range
//   fqdn      case-insensitive domain comparison        match_fqdn
//
// Every matcher is a pure, allocation-free, noexcept function over string views;
// malformed input never throws — it simply does not match. They are unit-tested
// in isolation (tests/test_matchers.cpp) and exposed across the C ABI by the
// runtime so generated modules can call the right one directly.

#pragma once

#include <array>
#include <cstdint>
#include <string_view>

namespace hmr::runtime {

// Parsed IP address in network byte order. v4 occupies the first four bytes.
enum class IpFamily : std::uint8_t { None, V4, V6 };

struct IpAddr {
    IpFamily family = IpFamily::None;
    std::array<std::uint8_t, 16> bytes{};

    [[nodiscard]] bool valid() const noexcept { return family != IpFamily::None; }
    [[nodiscard]] std::uint8_t byte_len() const noexcept {
        return family == IpFamily::V4 ? 4u : 16u;
    }
    [[nodiscard]] std::uint8_t width_bits() const noexcept {
        return family == IpFamily::V4 ? 32u : 128u;
    }
};

// Parse a textual IPv4 ("192.168.0.1") or IPv6 ("2001:db8::1", "::ffff:1.2.3.4")
// address. Surrounding brackets on an IPv6 literal ("[::1]") and a trailing
// "%zone" id are tolerated and stripped. Returns an invalid IpAddr on failure.
[[nodiscard]] IpAddr parse_ip(std::string_view text) noexcept;

// exact — the match-val-type that "must be strcmp". Byte-exact when
// case_insensitive is false, ASCII case-folded otherwise.
[[nodiscard]] bool match_exact(std::string_view subject, std::string_view pattern,
                               bool case_insensitive) noexcept;

// ip — true when both sides parse to the same address (any canonical spelling,
// same family). "192.168.000.001" matches "192.168.0.1".
[[nodiscard]] bool match_ip(std::string_view subject,
                            std::string_view pattern) noexcept;

// ip-mask — true when subject lies in the subnet described by pattern. Pattern
// is "address/prefix" where prefix is a CIDR length ("10.0.0.0/8",
// "2001:db8::/32") or, for IPv4, a dotted netmask ("10.0.0.0/255.0.0.0").
[[nodiscard]] bool match_ip_mask(std::string_view subject,
                                 std::string_view pattern) noexcept;

// ip-range — true when low <= subject <= high (inclusive). Pattern is
// "low-high" ("192.168.0.10-192.168.0.20"); all three addresses must share a
// family.
[[nodiscard]] bool match_ip_range(std::string_view subject,
                                  std::string_view pattern) noexcept;

// fqdn — case-insensitive domain-name comparison. A single trailing dot is
// insignificant ("example.com." matches "example.com").
[[nodiscard]] bool match_fqdn(std::string_view subject,
                              std::string_view pattern) noexcept;

}  // namespace hmr::runtime
