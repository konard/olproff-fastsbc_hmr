// SPDX-License-Identifier: MIT
//
// Unit tests for the specialized match-val-type matchers (matchers.hpp). These
// pin the semantics the runtime dispatches to: exact byte / case-insensitive
// comparison, canonical IP equality (v4 & v6), CIDR / dotted-mask subnet
// membership, inclusive address ranges, and case-insensitive FQDN comparison.

#include "test.hpp"
#include "hmr/runtime/matchers.hpp"

using namespace hmr::runtime;

TEST(Matchers, ExactIsByteCompare) {
    CHECK(match_exact("INVITE", "INVITE", false));
    CHECK(!match_exact("INVITE", "invite", false));
    CHECK(!match_exact("INVITE", "INVITED", false));   // length differs
    CHECK(!match_exact("abc", "ab", false));
}

TEST(Matchers, ExactCaseInsensitive) {
    CHECK(match_exact("INVITE", "invite", true));
    CHECK(match_exact("Register", "rEgIsTeR", true));
    CHECK(!match_exact("INVITE", "BYE", true));
}

TEST(Matchers, IpParsesV4AndV6) {
    CHECK(parse_ip("192.168.0.1").family == IpFamily::V4);
    CHECK(parse_ip("::1").family == IpFamily::V6);
    CHECK(!parse_ip("not.an.ip").valid());
    CHECK(!parse_ip("256.0.0.1").valid());     // octet out of range
    CHECK(!parse_ip("1.2.3").valid());         // too few octets
    CHECK(!parse_ip("1.2.3.4.5").valid());     // too many octets
}

TEST(Matchers, IpEqualityCanonical) {
    CHECK(match_ip("192.168.0.1", "192.168.000.001"));  // leading zeros
    CHECK(!match_ip("192.168.0.1", "192.168.0.2"));
    // Compressed vs fully-expanded IPv6 are equal.
    CHECK(match_ip("2001:db8::1", "2001:0db8:0000:0000:0000:0000:0000:0001"));
    CHECK(match_ip("::1", "0:0:0:0:0:0:0:1"));
    // IPv4-mapped IPv6, two spellings.
    CHECK(match_ip("::ffff:192.168.0.1", "::ffff:c0a8:1"));
    // Different families never match.
    CHECK(!match_ip("192.168.0.1", "::ffff:192.168.0.1"));
    CHECK(!match_ip("garbage", "192.168.0.1"));
}

TEST(Matchers, IpMaskCidrV4) {
    CHECK(match_ip_mask("10.1.2.3", "10.0.0.0/8"));
    CHECK(!match_ip_mask("11.1.2.3", "10.0.0.0/8"));
    CHECK(match_ip_mask("192.168.1.130", "192.168.1.128/25"));
    CHECK(!match_ip_mask("192.168.1.127", "192.168.1.128/25"));
    CHECK(match_ip_mask("8.8.8.8", "0.0.0.0/0"));        // /0 matches all
    CHECK(match_ip_mask("192.168.1.1", "192.168.1.1/32"));
    CHECK(!match_ip_mask("192.168.1.2", "192.168.1.1/32"));
}

TEST(Matchers, IpMaskDottedNetmask) {
    CHECK(match_ip_mask("10.20.30.40", "10.20.0.0/255.255.0.0"));
    CHECK(!match_ip_mask("10.21.30.40", "10.20.0.0/255.255.0.0"));
    // A non-contiguous mask is rejected (no match).
    CHECK(!match_ip_mask("10.20.30.40", "10.20.0.0/255.0.255.0"));
}

TEST(Matchers, IpMaskCidrV6) {
    CHECK(match_ip_mask("2001:db8::1", "2001:db8::/32"));
    CHECK(!match_ip_mask("2001:db9::1", "2001:db8::/32"));
    CHECK(match_ip_mask("fe80::abcd", "fe80::/10"));
    // Mixed families do not match.
    CHECK(!match_ip_mask("192.168.0.1", "2001:db8::/32"));
}

TEST(Matchers, IpRangeInclusive) {
    CHECK(match_ip_range("192.168.0.10", "192.168.0.1-192.168.0.20"));
    CHECK(match_ip_range("192.168.0.1", "192.168.0.1-192.168.0.20"));   // low edge
    CHECK(match_ip_range("192.168.0.20", "192.168.0.1-192.168.0.20"));  // high edge
    CHECK(!match_ip_range("192.168.0.21", "192.168.0.1-192.168.0.20"));
    CHECK(!match_ip_range("192.168.0.0", "192.168.0.1-192.168.0.20"));
    // IPv6 range.
    CHECK(match_ip_range("2001:db8::5", "2001:db8::1-2001:db8::10"));
    CHECK(!match_ip_range("2001:db8::20", "2001:db8::1-2001:db8::10"));
    // Family mismatch never matches.
    CHECK(!match_ip_range("2001:db8::5", "192.168.0.1-192.168.0.20"));
}

TEST(Matchers, FqdnCaseInsensitiveAndTrailingDot) {
    CHECK(match_fqdn("example.com", "EXAMPLE.COM"));
    CHECK(match_fqdn("sip.example.com.", "sip.example.com"));  // trailing dot
    CHECK(match_fqdn("sip.example.com", "sip.example.com."));
    CHECK(!match_fqdn("sip.example.com", "sip.example.net"));
    CHECK(!match_fqdn("", "example.com"));
}

TEST_MAIN()
