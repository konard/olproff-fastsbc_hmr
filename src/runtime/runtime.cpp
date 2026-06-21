// SPDX-License-Identifier: MIT
//
// runtime.cpp — implementation of the stable C ABI (hmr_runtime.h) plus the
// HmrContext lifecycle. This is the thin native runtime every generated module
// links against (statically, in the host) and calls into.
//
// The model is zero-copy (see sip_message.hpp / arena.hpp): reads return views
// into the immutable packet, writes go through the per-worker arena owned by the
// context. match-val-type comparisons dispatch to the specialized matchers
// (matchers.hpp) instead of routing everything through std::regex.

#include "hmr/runtime/context.hpp"

#include "hmr/runtime/matchers.hpp"
#include "hmr/runtime/sip_message.hpp"
#include "hmr/runtime/uri.hpp"

namespace {

std::string_view view(HmrStr s) noexcept {
    return std::string_view{s.data ? s.data : "", s.len};
}
HmrStr to_str(std::string_view s) noexcept {
    return HmrStr{s.data(), static_cast<std::uint32_t>(s.size())};
}
constexpr HmrStr kEmpty{nullptr, 0};

}  // namespace

// ===========================================================================
// HmrContext lifecycle
// ===========================================================================
void HmrContext::set_var(HmrVarId id, std::string value) {
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

void HmrContext::reset_for_apply() noexcept {
    arena.reset();
    has_match = false;
    scratch.clear();
    logs.clear();
    rejected = false;
    reject_code = 0;
    reject_reason.clear();
}

namespace hmr::runtime {

Context make_context(const HmrModuleInfo& info) {
    Context ctx;
    ctx.prepare(info);
    return ctx;
}

}  // namespace hmr::runtime

// ===========================================================================
// C ABI — header access
// ===========================================================================
extern "C" {

HmrStr hmr_rt_get_header(const HmrSipMsg* msg, HmrStr name) {
    if (!msg) return kEmpty;
    return msg->get_header(view(name)).str();
}

int hmr_rt_set_header(HmrSipMsg* msg, HmrContext* ctx, HmrStr name,
                      HmrStr value) {
    if (!msg || !ctx) return 0;
    return msg->set_header(view(name), view(value), ctx->arena) ? 1 : 0;
}

int hmr_rt_add_header(HmrSipMsg* msg, HmrContext* ctx, HmrStr name,
                      HmrStr value) {
    if (!msg || !ctx) return 0;
    return msg->add_header(view(name), view(value), ctx->arena) ? 1 : 0;
}

int hmr_rt_delete_header(HmrSipMsg* msg, HmrStr name) {
    if (!msg) return 0;
    return msg->delete_header(view(name)) ? 1 : 0;
}

HmrStr hmr_rt_get_method(const HmrSipMsg* msg) {
    return msg ? msg->method.str() : kEmpty;
}

int hmr_rt_is_request(const HmrSipMsg* msg) {
    return (msg && msg->is_request) ? 1 : 0;
}

uint32_t hmr_rt_status_code(const HmrSipMsg* msg) {
    return msg ? msg->status_code : 0u;
}

// ===========================================================================
// C ABI — pre-extracted URI component fields
// ===========================================================================
HmrStr hmr_rt_get_field(const HmrSipMsg* msg, uint32_t field) {
    if (!msg) return kEmpty;
    return msg->get_field(field).str();
}

int hmr_rt_set_field(HmrSipMsg* msg, HmrContext* ctx, uint32_t field,
                     HmrStr value) {
    if (!msg || !ctx) return 0;
    return msg->set_field(field, view(value), ctx->arena) ? 1 : 0;
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
    bool ok = std::regex_search(base, base + subject.len, ctx->last_match,
                                ctx->regexes[regex_id]);
    ctx->has_match = ok;
    return ok ? 1 : 0;
}

int hmr_rt_match(HmrContext* ctx, uint32_t match_type, HmrStr subject,
                 HmrStr pattern, uint32_t regex_id) {
    using namespace hmr::runtime;
    const std::string_view s = view(subject);
    const std::string_view p = view(pattern);
    switch (match_type) {
        case HMR_MATCH_EXACT:    return match_exact(s, p, false) ? 1 : 0;
        case HMR_MATCH_EXACT_CI: return match_exact(s, p, true) ? 1 : 0;
        case HMR_MATCH_REGEX:    return hmr_rt_regex_match(ctx, regex_id, subject);
        case HMR_MATCH_IP:       return match_ip(s, p) ? 1 : 0;
        case HMR_MATCH_IP_MASK:  return match_ip_mask(s, p) ? 1 : 0;
        case HMR_MATCH_IP_RANGE: return match_ip_range(s, p) ? 1 : 0;
        case HMR_MATCH_FQDN:     return match_fqdn(s, p) ? 1 : 0;
        default:                 return 0;
    }
}

// ===========================================================================
// C ABI — captures / variables / slots
// ===========================================================================
HmrStr hmr_rt_get_capture(const HmrContext* ctx, uint32_t index) {
    if (!ctx || !ctx->has_match || index >= ctx->last_match.size())
        return kEmpty;
    const auto& sub = ctx->last_match[index];
    if (!sub.matched) return kEmpty;
    return HmrStr{&*sub.first,
                  static_cast<std::uint32_t>(sub.second - sub.first)};
}

HmrStr hmr_rt_get_var(const HmrContext* ctx, uint32_t var_id) {
    if (!ctx || var_id == 0 || var_id >= HMR_VAR_MAX) return kEmpty;
    return to_str(ctx->vars[var_id]);
}

void hmr_rt_store(HmrContext* ctx, uint32_t slot, HmrStr value) {
    if (!ctx || slot >= ctx->slots.size()) return;
    ctx->slots[slot].assign(view(value));
}

HmrStr hmr_rt_load(const HmrContext* ctx, uint32_t slot) {
    if (!ctx || slot >= ctx->slots.size()) return kEmpty;
    return to_str(ctx->slots[slot]);
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
    return ctx ? to_str(ctx->scratch) : kEmpty;
}

// ===========================================================================
// C ABI — URI element access
// ===========================================================================
HmrStr hmr_rt_uri_get(HmrContext* ctx, HmrStr header_value,
                      uint32_t element_type) {
    if (!ctx) return kEmpty;
    hmr::runtime::ParsedUri u = hmr::runtime::parse_uri(view(header_value));
    switch (element_type) {
        case HMR_URI_WHOLE:   return header_value;
        case HMR_URI_DISPLAY: return to_str(u.display);
        case HMR_URI_USER:    return to_str(u.user);
        case HMR_URI_HOST:    return to_str(u.host);
        case HMR_URI_PORT:    return to_str(u.port);
        default:              return kEmpty;
    }
}

HmrStr hmr_rt_uri_set(HmrContext* ctx, HmrStr header_value,
                      uint32_t element_type, HmrStr new_value) {
    if (!ctx) return kEmpty;
    return hmr::runtime::rebuild_uri(ctx->arena, view(header_value),
                                     element_type, view(new_value));
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
    ctx->reject_code = status_code;
    ctx->reject_reason.assign(view(reason));
}

}  // extern "C"
