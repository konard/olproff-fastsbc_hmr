// SPDX-License-Identifier: MIT
//
// sip_message.cpp — zero-copy SIP message model (see sip_message.hpp).
//
// Parsing fills slices into the immutable `raw` buffer; mutations write into the
// per-worker arena and re-point the affected slices. Nothing is copied on the
// read path, and the write path never touches the heap.

#include "hmr/runtime/sip_message.hpp"

#include "hmr/runtime/uri.hpp"

namespace hmr::runtime {

// Branchless ASCII lowercase. SIP header field-names are US-ASCII tokens
// (RFC 3261 §7.3 / §25.1), so locale-aware std::tolower is both unnecessary and,
// because it routes through the C locale on every character, far too slow for
// the per-packet header lookups this powers (find/get_header run O(rules ×
// headers) times per message — the hottest loop in the data path).
constexpr char ascii_to_lower(char c) noexcept {
    return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
}

bool iequals(std::string_view a, std::string_view b) noexcept {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i)
        if (ascii_to_lower(a[i]) != ascii_to_lower(b[i])) return false;
    return true;
}

}  // namespace hmr::runtime

namespace {

// Trim ASCII spaces, tabs and a trailing CR. Returns a sub-view, so the result
// still points into the same backing buffer (zero-copy preserved).
std::string_view trim(std::string_view s) noexcept {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t'))
        s.remove_prefix(1);
    while (!s.empty() &&
           (s.back() == ' ' || s.back() == '\t' || s.back() == '\r'))
        s.remove_suffix(1);
    return s;
}

using Slice = HmrSipMsg::Slice;

}  // namespace

// ===========================================================================
// Header lookup / mutation
// ===========================================================================
int HmrSipMsg::find(std::string_view name) const noexcept {
    for (std::uint32_t i = 0; i < num_headers; ++i) {
        if (!headers[i].removed &&
            hmr::runtime::iequals(headers[i].name.sv(), name))
            return static_cast<int>(i);
    }
    return -1;
}

HmrSipMsg::Slice HmrSipMsg::get_header(std::string_view name) const noexcept {
    int i = find(name);
    return i < 0 ? Slice{} : headers[static_cast<std::uint32_t>(i)].value;
}

bool HmrSipMsg::set_header(std::string_view name, std::string_view value,
                           HmrArena& arena) noexcept {
    HmrStr dup = arena.dup(value);
    if (dup.data == nullptr && !value.empty()) return false;  // arena overflow
    int i = find(name);
    if (i >= 0) {
        headers[static_cast<std::uint32_t>(i)].value = Slice::of(dup);
        return true;
    }
    return add_header(name, value, arena);
}

bool HmrSipMsg::add_header(std::string_view name, std::string_view value,
                           HmrArena& arena) noexcept {
    if (num_headers >= HMR_MAX_HEADERS) return false;
    HmrStr dn = arena.dup(name);
    if (dn.data == nullptr && !name.empty()) return false;
    HmrStr dv = arena.dup(value);
    if (dv.data == nullptr && !value.empty()) return false;
    headers[num_headers] = Header{Slice::of(dn), Slice::of(dv), false};
    ++num_headers;
    return true;
}

bool HmrSipMsg::delete_header(std::string_view name) noexcept {
    bool any = false;
    for (std::uint32_t i = 0; i < num_headers; ++i) {
        if (!headers[i].removed &&
            hmr::runtime::iequals(headers[i].name.sv(), name)) {
            headers[i].removed = true;
            any = true;
        }
    }
    return any;
}

// ===========================================================================
// Pre-extracted URI component fields
// ===========================================================================
namespace {

// The current request-URI: the arena override if set, else the original slice.
Slice current_request_uri(const HmrSipMsg& m) noexcept {
    return m.mod_request_uri.present() ? m.mod_request_uri : m.request_uri;
}

// Map a From/To field to its header name + URI element.
struct FieldRef {
    const char* header;  // nullptr => request-URI (not a header)
    std::uint32_t element;
};

FieldRef field_ref(std::uint32_t field) noexcept {
    switch (field) {
        case HMR_FIELD_REQUEST_URI_USER: return {nullptr, HMR_URI_USER};
        case HMR_FIELD_REQUEST_URI_HOST: return {nullptr, HMR_URI_HOST};
        case HMR_FIELD_FROM_USER:        return {"From", HMR_URI_USER};
        case HMR_FIELD_FROM_HOST:        return {"From", HMR_URI_HOST};
        case HMR_FIELD_TO_USER:          return {"To", HMR_URI_USER};
        case HMR_FIELD_TO_HOST:          return {"To", HMR_URI_HOST};
        default:                         return {nullptr, HMR_URI_WHOLE};
    }
}

Slice component_of(std::string_view value, std::uint32_t element) noexcept {
    const hmr::runtime::ParsedUri u = hmr::runtime::parse_uri(value);
    switch (element) {
        case HMR_URI_USER: return Slice::of(u.user);
        case HMR_URI_HOST: return Slice::of(u.host);
        default:           return Slice{};
    }
}

}  // namespace

HmrSipMsg::Slice HmrSipMsg::get_field(std::uint32_t field) const noexcept {
    if (field >= HMR_FIELD_MAX) return Slice{};
    const FieldRef ref = field_ref(field);
    std::string_view value =
        ref.header ? get_header(ref.header).sv() : current_request_uri(*this).sv();
    return component_of(value, ref.element);
}

bool HmrSipMsg::set_field(std::uint32_t field, std::string_view value,
                          HmrArena& arena) noexcept {
    if (field >= HMR_FIELD_MAX) return false;
    const FieldRef ref = field_ref(field);
    if (ref.header == nullptr) {
        // Request-URI: rebuild from the current value, store as the override.
        HmrStr rebuilt = hmr::runtime::rebuild_uri(
            arena, current_request_uri(*this).sv(), ref.element, value);
        if (rebuilt.data == nullptr) return false;
        mod_request_uri = Slice::of(rebuilt);
        return true;
    }
    // From/To: rewrite the header value in place — the header stays the single
    // source of truth (no separate cached slice to go stale).
    int i = find(ref.header);
    if (i < 0) return false;
    HmrStr rebuilt = hmr::runtime::rebuild_uri(
        arena, headers[static_cast<std::uint32_t>(i)].value.sv(), ref.element,
        value);
    if (rebuilt.data == nullptr) return false;
    headers[static_cast<std::uint32_t>(i)].value = Slice::of(rebuilt);
    return true;
}

// ===========================================================================
// Parsing
// ===========================================================================
HmrSipMsg HmrSipMsg::parse(std::string_view raw) noexcept {
    HmrSipMsg msg;
    msg.raw = raw.data();
    msg.raw_len = static_cast<std::uint32_t>(raw.size());

    std::size_t pos = 0;
    bool first = true;

    auto next_line = [&](std::string_view& out) -> bool {
        if (pos > raw.size()) return false;
        std::size_t nl = raw.find('\n', pos);
        if (nl == std::string_view::npos) {
            out = raw.substr(pos);
            pos = raw.size() + 1;
        } else {
            out = raw.substr(pos, nl - pos);
            pos = nl + 1;
        }
        return true;
    };

    std::string_view line;
    while (next_line(line)) {
        std::string_view t = trim(line);
        if (first) {
            first = false;
            msg.start_line = Slice::of(t);
            if (t.starts_with("SIP/")) {
                // Status line: "SIP/2.0 200 OK".
                msg.is_request = false;
                std::size_t sp1 = t.find(' ');
                std::size_t sp2 = sp1 == std::string_view::npos
                                      ? sp1
                                      : t.find(' ', sp1 + 1);
                if (sp1 != std::string_view::npos) {
                    std::string_view code = t.substr(
                        sp1 + 1,
                        (sp2 == std::string_view::npos ? t.size() : sp2) - sp1 -
                            1);
                    msg.status_code = 0;
                    for (char c : code)
                        if (c >= '0' && c <= '9')
                            msg.status_code =
                                msg.status_code * 10 +
                                static_cast<std::uint32_t>(c - '0');
                    if (sp2 != std::string_view::npos)
                        msg.reason_phrase = Slice::of(trim(t.substr(sp2 + 1)));
                }
            } else {
                // Request line: "INVITE sip:bob@example.com SIP/2.0".
                msg.is_request = true;
                std::size_t sp1 = t.find(' ');
                if (sp1 != std::string_view::npos) {
                    msg.method = Slice::of(t.substr(0, sp1));
                    std::size_t sp2 = t.find(' ', sp1 + 1);
                    msg.request_uri = Slice::of(trim(t.substr(
                        sp1 + 1,
                        (sp2 == std::string_view::npos ? t.size() : sp2) - sp1 -
                            1)));
                }
            }
            continue;
        }
        if (t.empty()) {
            // Blank line: end of headers. Everything after is the body.
            msg.body = Slice::of(raw.substr(pos));
            break;
        }

        std::size_t colon = line.find(':');
        if (colon == std::string_view::npos) continue;  // malformed; skip
        std::string_view name = trim(line.substr(0, colon));
        std::string_view value = trim(line.substr(colon + 1));
        if (msg.num_headers < HMR_MAX_HEADERS) {
            msg.headers[msg.num_headers] =
                Header{Slice::of(name), Slice::of(value), false};
            ++msg.num_headers;
        }
    }
    return msg;
}

// ===========================================================================
// Serialization
// ===========================================================================
HmrStr HmrSipMsg::serialize(HmrArena& arena) const noexcept {
    const std::uint32_t begin = arena.used;
    char* const out = arena.buffer.data() + begin;
    bool ok = true;

    auto put = [&](std::string_view s) {
        if (!ok || s.empty()) return;
        char* p = arena.alloc(static_cast<std::uint32_t>(s.size()));
        if (p == nullptr) {
            ok = false;
            return;
        }
        std::memcpy(p, s.data(), s.size());
    };

    // First line: splice the request-URI override into the original line so the
    // method and SIP-version are preserved byte-for-byte.
    if (is_request && mod_request_uri.present() && request_uri.present()) {
        std::string_view sl = start_line.sv();
        std::size_t ruri_off =
            static_cast<std::size_t>(request_uri.data - start_line.data);
        put(sl.substr(0, ruri_off));
        put(mod_request_uri.sv());
        put(sl.substr(ruri_off + request_uri.len));
    } else {
        put(start_line.sv());
    }
    put("\r\n");

    for (std::uint32_t i = 0; i < num_headers; ++i) {
        if (headers[i].removed) continue;
        put(headers[i].name.sv());
        put(": ");
        put(headers[i].value.sv());
        put("\r\n");
    }
    put("\r\n");
    put(body.sv());

    if (!ok) return HmrStr{nullptr, 0};
    return HmrStr{out, arena.used - begin};
}
