// SPDX-License-Identifier: MIT
//
// Runtime.cpp — implementation of the stable C ABI (hmr_runtime.h) plus the
// HmrContext lifecycle. This is the thin native runtime every generated module
// links against (statically, in the host) and calls into.

#include "hmr/runtime/Runtime.hpp"

#include <cstring>

#include "hmr/runtime/SipMessage.hpp"

namespace {

HmrStr toStr(std::string_view s) {
    return HmrStr{s.data(), static_cast<std::uint32_t>(s.size())};
}
std::string_view view(HmrStr s) {
    return std::string_view{s.data ? s.data : "", s.len};
}
constexpr HmrStr kEmpty{nullptr, 0};

// --- Minimal SIP URI parsing for element rules ----------------------------
// Splits a header value of the form
//     [ display-name ] [<] sip:user@host[:port][;params] [>] [;hdr-params]
// into its components. Views point into the input; no allocation.
struct ParsedUri {
    std::string_view display;
    std::string_view scheme;
    std::string_view user;
    std::string_view host;
    std::string_view port;
    std::string_view tail;   // params / trailing text after host[:port]
    bool angled = false;
    std::size_t addrStart = 0;  // offset of scheme in the original value
    std::size_t addrEnd = 0;    // offset just past host[:port]
};

std::string_view trimv(std::string_view s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t'))
        s.remove_prefix(1);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t'))
        s.remove_suffix(1);
    return s;
}

ParsedUri parseUri(std::string_view v) {
    ParsedUri u;
    std::string_view s = v;

    // Display name + optional angle brackets.
    std::size_t lt = s.find('<');
    if (lt != std::string_view::npos) {
        u.display = trimv(s.substr(0, lt));
        if (!u.display.empty() && u.display.front() == '"' &&
            u.display.back() == '"' && u.display.size() >= 2)
            u.display = u.display.substr(1, u.display.size() - 2);
        std::size_t gt = s.find('>', lt + 1);
        u.angled = true;
        std::size_t inner_off = lt + 1;
        std::string_view inner =
            s.substr(inner_off, (gt == std::string_view::npos ? s.size() : gt) -
                                    inner_off);
        u.addrStart = inner_off;
        s = inner;
    } else {
        u.addrStart = 0;
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
    std::size_t hpEnd = hostport.find_first_of(";?>");
    std::string_view hp = hostport.substr(
        0, hpEnd == std::string_view::npos ? hostport.size() : hpEnd);
    if (hpEnd != std::string_view::npos) u.tail = hostport.substr(hpEnd);

    std::size_t portColon = hp.rfind(':');
    if (portColon != std::string_view::npos) {
        u.host = hp.substr(0, portColon);
        u.port = hp.substr(portColon + 1);
    } else {
        u.host = hp;
    }

    // Compute addrEnd offset (relative to the original value) for rebuilding.
    std::size_t hostOffInS =
        static_cast<std::size_t>(u.host.data() - s.data());
    u.addrEnd = u.addrStart + hostOffInS + u.host.size() +
                (u.port.empty() ? 0 : u.port.size() + 1);
    return u;
}

}  // namespace

// ===========================================================================
// HmrContext lifecycle
// ===========================================================================
void HmrContext::setVar(HmrVarId id, std::string value) {
    if (id > HMR_VAR_NONE && id < HMR_VAR_MAX)
        vars[static_cast<std::size_t>(id)] = std::move(value);
}

void HmrContext::prepare(const HmrModuleInfo& info) {
    regexes.clear();
    regexes.reserve(info.num_regexes);
    for (std::uint32_t i = 0; i < info.num_regexes; ++i) {
        auto flags = std::regex::ECMAScript | std::regex::optimize;
        if (info.regexes[i].flags & 1u) flags |= std::regex::icase;
        regexes.emplace_back(info.regexes[i].pattern, flags);
    }
    slots.assign(info.num_slots, std::string{});
    scratch.reserve(256);
}

void HmrContext::resetForApply() {
    hasMatch = false;
    scratch.clear();
    logs.clear();
    rejected = false;
    rejectCode = 0;
    rejectReason.clear();
}

namespace hmr::runtime {

Context makeContext(const HmrModuleInfo& info) {
    Context ctx;
    ctx.prepare(info);
    return ctx;
}

}  // namespace hmr::runtime

// ===========================================================================
// C ABI — header access
// ===========================================================================
extern "C" {

HmrStr hmr_rt_get_header(HmrSipMsg* msg, HmrStr name) {
    if (!msg) return kEmpty;
    return toStr(msg->getHeader(view(name)));
}

int hmr_rt_set_header(HmrSipMsg* msg, HmrStr name, HmrStr value) {
    if (!msg) return 0;
    return msg->setHeader(view(name), view(value)) ? 1 : 0;
}

int hmr_rt_add_header(HmrSipMsg* msg, HmrStr name, HmrStr value) {
    if (!msg) return 0;
    msg->addHeader(view(name), view(value));
    return 1;
}

int hmr_rt_delete_header(HmrSipMsg* msg, HmrStr name) {
    if (!msg) return 0;
    return msg->deleteHeader(view(name)) ? 1 : 0;
}

HmrStr hmr_rt_get_method(const HmrSipMsg* msg) {
    return msg ? toStr(msg->method) : kEmpty;
}

int hmr_rt_is_request(const HmrSipMsg* msg) {
    return (msg && msg->isRequest) ? 1 : 0;
}

uint32_t hmr_rt_status_code(const HmrSipMsg* msg) {
    return msg ? msg->statusCode : 0u;
}

// ===========================================================================
// C ABI — comparison
// ===========================================================================
int hmr_rt_str_eq(HmrStr a, HmrStr b, int case_insensitive) {
    if (case_insensitive)
        return hmr::runtime::iequals(view(a), view(b)) ? 1 : 0;
    return view(a) == view(b) ? 1 : 0;
}

int hmr_rt_regex_match(HmrContext* ctx, uint32_t regex_id, HmrStr subject) {
    if (!ctx || regex_id >= ctx->regexes.size()) return 0;
    const char* base = subject.data ? subject.data : "";
    bool ok = std::regex_search(base, base + subject.len, ctx->lastMatch,
                                ctx->regexes[regex_id]);
    ctx->hasMatch = ok;
    return ok ? 1 : 0;
}

// ===========================================================================
// C ABI — captures / variables / slots
// ===========================================================================
HmrStr hmr_rt_get_capture(const HmrContext* ctx, uint32_t index) {
    if (!ctx || !ctx->hasMatch || index >= ctx->lastMatch.size()) return kEmpty;
    const auto& sub = ctx->lastMatch[index];
    if (!sub.matched) return kEmpty;
    return HmrStr{&*sub.first,
                  static_cast<std::uint32_t>(sub.second - sub.first)};
}

HmrStr hmr_rt_get_var(const HmrContext* ctx, uint32_t var_id) {
    if (!ctx || var_id == 0 || var_id >= HMR_VAR_MAX) return kEmpty;
    return toStr(ctx->vars[var_id]);
}

void hmr_rt_store(HmrContext* ctx, uint32_t slot, HmrStr value) {
    if (!ctx || slot >= ctx->slots.size()) return;
    ctx->slots[slot].assign(view(value));
}

HmrStr hmr_rt_load(const HmrContext* ctx, uint32_t slot) {
    if (!ctx || slot >= ctx->slots.size()) return kEmpty;
    return toStr(ctx->slots[slot]);
}

// ===========================================================================
// C ABI — value builder
// ===========================================================================
void hmr_rt_val_reset(HmrContext* ctx) {
    if (ctx) ctx->scratch.clear();
}

void hmr_rt_val_append_lit(HmrContext* ctx, HmrStr literal) {
    if (ctx) ctx->scratch.append(view(literal));
}

void hmr_rt_val_append_var(HmrContext* ctx, uint32_t var_id) {
    if (ctx && var_id > 0 && var_id < HMR_VAR_MAX)
        ctx->scratch.append(ctx->vars[var_id]);
}

void hmr_rt_val_append_capture(HmrContext* ctx, uint32_t index) {
    if (!ctx) return;
    HmrStr c = hmr_rt_get_capture(ctx, index);
    ctx->scratch.append(view(c));
}

void hmr_rt_val_append_slot(HmrContext* ctx, uint32_t slot) {
    if (ctx && slot < ctx->slots.size()) ctx->scratch.append(ctx->slots[slot]);
}

HmrStr hmr_rt_val_finish(HmrContext* ctx) {
    return ctx ? toStr(ctx->scratch) : kEmpty;
}

// ===========================================================================
// C ABI — URI element access
// ===========================================================================
HmrStr hmr_rt_uri_get(HmrContext* ctx, HmrStr header_value,
                      uint32_t element_type) {
    if (!ctx) return kEmpty;
    ParsedUri u = parseUri(view(header_value));
    switch (element_type) {
        case HMR_URI_WHOLE:   return header_value;
        case HMR_URI_DISPLAY: return toStr(u.display);
        case HMR_URI_USER:    return toStr(u.user);
        case HMR_URI_HOST:    return toStr(u.host);
        case HMR_URI_PORT:    return toStr(u.port);
        default:              return kEmpty;
    }
}

HmrStr hmr_rt_uri_set(HmrContext* ctx, HmrStr header_value,
                      uint32_t element_type, HmrStr new_value) {
    if (!ctx) return kEmpty;
    std::string_view v = view(header_value);
    std::string_view nv = view(new_value);
    ParsedUri u = parseUri(v);

    std::string& out = ctx->scratch;
    out.clear();
    switch (element_type) {
        case HMR_URI_USER: {
            // Rebuild user@host, preserving everything around it.
            if (u.user.data()) {
                std::size_t userOff =
                    static_cast<std::size_t>(u.user.data() - v.data());
                out.append(v.substr(0, userOff));
                out.append(nv);
                out.append(v.substr(userOff + u.user.size()));
            } else if (u.host.data()) {
                // No user present: insert "user@" before host.
                std::size_t hostOff =
                    static_cast<std::size_t>(u.host.data() - v.data());
                out.append(v.substr(0, hostOff));
                out.append(nv);
                out.push_back('@');
                out.append(v.substr(hostOff));
            } else {
                out.append(v);
            }
            break;
        }
        case HMR_URI_HOST: {
            if (u.host.data()) {
                std::size_t hostOff =
                    static_cast<std::size_t>(u.host.data() - v.data());
                out.append(v.substr(0, hostOff));
                out.append(nv);
                out.append(v.substr(hostOff + u.host.size()));
            } else {
                out.append(v);
            }
            break;
        }
        case HMR_URI_DISPLAY: {
            if (u.display.data()) {
                std::size_t dispOff =
                    static_cast<std::size_t>(u.display.data() - v.data());
                out.append(v.substr(0, dispOff));
                out.append(nv);
                out.append(v.substr(dispOff + u.display.size()));
            } else {
                out.append(nv);
                out.push_back(' ');
                out.append(v);
            }
            break;
        }
        case HMR_URI_WHOLE:
        default:
            out.append(nv);
            break;
    }
    return toStr(out);
}

// ===========================================================================
// C ABI — diagnostics / control
// ===========================================================================
void hmr_rt_log(HmrContext* ctx, HmrStr message) {
    if (ctx) ctx->logs.emplace_back(view(message));
}

void hmr_rt_reject(HmrContext* ctx, uint32_t status_code, HmrStr reason) {
    if (!ctx) return;
    ctx->rejected = true;
    ctx->rejectCode = status_code;
    ctx->rejectReason.assign(view(reason));
}

}  // extern "C"
