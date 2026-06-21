// SPDX-License-Identifier: MIT
//
// interpreter.cpp — AST tree-walker that mirrors src/codegen/ir_generator.cpp.
//
// Every decision here is the runtime twin of an IR-emission decision there:
// where the generator emits `call @hmr_rt_get_header(...)`, the interpreter
// *calls* hmr_rt_get_header(...); where it emits a conditional branch past a
// rule, the interpreter `continue`s the loop. Read the two files side by side —
// each method below names the Emitter method it mirrors. Keeping them
// line-for-line aligned (and sharing the pure mappings via codegen/lowering.hpp)
// is what makes the interpreter a trustworthy oracle for the differential
// fuzzer: a divergence would be a visible asymmetry between the two walks.
//
// Two-mode walk. The constructor runs walk(nullptr, nullptr, scan=true) to
// assign the slot/regex tables for *every* construct the generator would emit
// (runtime guards do not short-circuit, and the null-safe hmr_rt_* calls are
// harmless no-ops). apply() then runs walk(msg, ctx, scan=false): guards
// short-circuit and the runtime calls do real work. Because both modes are the
// same code, the tables assigned at scan time are exactly the ones apply looks
// up — by construction.

#include "hmr/interp/interpreter.hpp"

#include <algorithm>
#include <optional>
#include <string>

#include "hmr/codegen/lowering.hpp"
#include "hmr/runtime/hmr_runtime.h"

namespace hmr::interp {

using ast::ComparisonType;
using ast::ElementAction;
using ast::ElementRule;
using ast::HeaderAction;
using ast::HeaderRule;
using ast::MatchValType;
using ast::MsgType;
using ast::RefKind;
using ast::Value;
using ast::ValueSegment;

using codegen::capture_slot_key;
using codegen::count_capturing_groups;
using codegen::ip_match_type;
using codegen::is_case_insensitive;
using codegen::is_pattern;
using codegen::ref_base_name;
using codegen::uri_element;
using codegen::variable_id;

// --- construction -----------------------------------------------------------

Interpreter::Interpreter(const ast::Ruleset& rs)
    : rs_(rs), name_(rs.name.empty() ? "hmr" : rs.name) {
    // Pre-scan: assign slot/regex tables exactly as the code generator does.
    walk(/*msg=*/nullptr, /*ctx=*/nullptr, /*scan=*/true);
    frozen_ = true;

    // Publish the synthetic module descriptor. regexes_/regex_entries_ no longer
    // grow (frozen_), so the c_str() pointers stay valid for our lifetime.
    regex_entries_.reserve(regexes_.size());
    for (const auto& [pattern, flags] : regexes_)
        regex_entries_.push_back(HmrRegexEntry{pattern.c_str(), flags});

    info_.abi_version = HMR_ABI_VERSION;
    info_.name = name_.c_str();
    info_.num_slots = static_cast<std::uint32_t>(slots_.size());
    info_.num_regexes = static_cast<std::uint32_t>(regexes_.size());
    info_.regexes = regex_entries_.empty() ? nullptr : regex_entries_.data();
}

int Interpreter::apply(HmrSipMsg* msg, HmrContext* ctx) const {
    return walk(msg, ctx, /*scan=*/false);
}

// --- tables (mirror Emitter::add_regex / slot_for / slot_for_ref) -----------

unsigned Interpreter::add_regex(const std::string& pattern, bool ci) const {
    std::uint32_t flags = ci ? 1u : 0u;
    for (unsigned i = 0; i < regexes_.size(); ++i)
        if (regexes_[i].first == pattern && regexes_[i].second == flags) return i;
    if (frozen_) return 0;  // unreachable: the scan registered every pattern
    regexes_.emplace_back(pattern, flags);
    return static_cast<unsigned>(regexes_.size() - 1);
}

unsigned Interpreter::slot_for(const std::string& name) const {
    auto it = slots_.find(name);
    if (it != slots_.end()) return it->second;
    if (frozen_) return 0;  // unreachable: the scan registered every slot
    unsigned id = static_cast<unsigned>(slots_.size());
    slots_.emplace(name, id);
    return id;
}

unsigned Interpreter::slot_for_ref(const std::string& path,
                                   int capture_index) const {
    return slot_for(capture_slot_key(
        ref_base_name(path), static_cast<unsigned>(std::max(0, capture_index))));
}

HmrStr Interpreter::intern(std::string_view s) const {
    auto it = literal_pool_.insert(std::string(s)).first;
    return HmrStr{it->c_str(), static_cast<std::uint32_t>(it->size())};
}

// --- value & guard (mirror Emitter::emit_value / emit_match_guard) ----------

HmrStr Interpreter::eval_value(const Value& v, HmrContext* ctx) const {
    if (v.empty()) return intern("");
    if (v.is_pure_literal()) return intern(v.literal_text());

    if (v.is_single_ref()) {
        const auto& ref = v.segments().front().ref;
        switch (ref.kind) {
            case RefKind::Variable:
                return hmr_rt_get_var(ctx, static_cast<std::uint32_t>(variable_id(ref.name)));
            case RefKind::Capture:
                return hmr_rt_get_capture(ctx, static_cast<std::uint32_t>(std::max(0, ref.capture_index)));
            case RefKind::RuleRef:
                return hmr_rt_load(ctx, slot_for_ref(ref.name, ref.capture_index));
        }
    }

    // Multi-segment expression: build into the context scratch buffer.
    hmr_rt_val_reset(ctx);
    for (const ValueSegment& seg : v.segments()) {
        if (!seg.is_ref) {
            if (!seg.literal.empty()) hmr_rt_val_append_lit(ctx, intern(seg.literal));
            continue;
        }
        switch (seg.ref.kind) {
            case RefKind::Variable:
                hmr_rt_val_append_var(ctx, static_cast<std::uint32_t>(variable_id(seg.ref.name)));
                break;
            case RefKind::Capture:
                hmr_rt_val_append_capture(ctx, static_cast<std::uint32_t>(std::max(0, seg.ref.capture_index)));
                break;
            case RefKind::RuleRef:
                hmr_rt_val_append_slot(ctx, slot_for_ref(seg.ref.name, seg.ref.capture_index));
                break;
        }
    }
    return hmr_rt_val_finish(ctx);
}

bool Interpreter::eval_match_guard(ComparisonType cmp, MatchValType mvt,
                                   const Value& mv, HmrStr subject,
                                   HmrContext* ctx) const {
    if (mv.empty()) return true;  // no match-value → always applies

    // A $reference / interpolated match-value (boolean back-reference) is a
    // documented v1 gap: treat as always-true so the rule still fires.
    if (!mv.is_regex_literal() && !mv.is_pure_literal()) return true;

    // The literal match-value text ('!' negation already stripped by parse()).
    const std::string pattern =
        mv.is_pure_literal() ? mv.literal_text() : std::string{mv.raw()};

    // Select the specialized matcher (review #2), identically to the generator.
    std::uint32_t match_type;
    unsigned regex_id = 0;
    if (is_pattern(cmp)) {
        match_type = HMR_MATCH_REGEX;
        regex_id = add_regex(pattern, is_case_insensitive(cmp));
    } else {
        switch (mvt) {
            case MatchValType::Ip:   match_type = ip_match_type(pattern); break;
            case MatchValType::Fqdn: match_type = HMR_MATCH_FQDN; break;
            case MatchValType::Any:
            default:
                match_type = is_case_insensitive(cmp) ? HMR_MATCH_EXACT_CI
                                                      : HMR_MATCH_EXACT;
                break;
        }
    }

    HmrStr pat = intern(pattern);
    bool cond = hmr_rt_match(ctx, match_type, subject, pat, regex_id) != 0;
    if (mv.negated()) cond = !cond;
    return cond;
}

// --- rule emission (mirror Emitter::emit_header_action) ---------------------

bool Interpreter::run_header_action(const HeaderRule& hr, std::size_t idx,
                                    HmrStr hv, bool have_hv, HmrSipMsg* msg,
                                    HmrContext* ctx) const {
    const HmrStr name = intern(hr.header_name);
    switch (hr.action) {
        case HeaderAction::Add:
            hmr_rt_add_header(msg, ctx, name, eval_value(hr.new_value, ctx));
            return false;
        case HeaderAction::Replace:
        case HeaderAction::Manipulate:  // header-level replace via new-value
            if (!hr.new_value.empty() && hr.element_rules.empty())
                hmr_rt_set_header(msg, ctx, name, eval_value(hr.new_value, ctx));
            return false;  // pure manipulate → handled by element rules
        case HeaderAction::Delete:
        case HeaderAction::DeleteHeader:
            hmr_rt_delete_header(msg, name);
            return false;
        case HeaderAction::Store: {
            const std::string rule =
                hr.name.empty() ? ("rule" + std::to_string(idx)) : hr.name;
            if (is_pattern(hr.comparison)) {
                // Pattern-rule store: capture the whole match (group 0, keyed by
                // the bare rule name) *and* each sub-group under "rule.$N", so a
                // later $rule.$N back-reference loads group N (Oracle HMR). The
                // match-value's regex fixes how many groups exist.
                const std::string pat = hr.match_value.is_pure_literal()
                                            ? hr.match_value.literal_text()
                                            : std::string{hr.match_value.raw()};
                unsigned ngroups = count_capturing_groups(pat);
                for (unsigned g = 0; g <= ngroups; ++g)
                    hmr_rt_store(ctx, slot_for(capture_slot_key(rule, g)),
                                 hmr_rt_get_capture(ctx, g));
            } else {
                hmr_rt_store(ctx, slot_for(rule),
                             have_hv ? hv : hmr_rt_get_header(msg, name));
            }
            return false;
        }
        case HeaderAction::Log:
            hmr_rt_log(ctx, hr.new_value.empty() ? intern(hr.name)
                                                 : eval_value(hr.new_value, ctx));
            return false;
        case HeaderAction::Reject:
            hmr_rt_reject(ctx, 403u, hr.new_value.empty() ? intern(hr.name)
                                                          : eval_value(hr.new_value, ctx));
            return true;
        default:
            return false;  // None / DeleteElement / etc. — no header-level act
    }
}

// --- element rules (mirror Emitter::emit_element_rules) ----------------------

Interpreter::Flow Interpreter::run_element_rules(const HeaderRule& hr,
                                                 HmrSipMsg* msg, HmrContext* ctx,
                                                 bool scan) const {
    const HmrStr name = intern(hr.header_name);
    for (const ElementRule& er : hr.element_rules) {
        std::optional<std::uint32_t> elem = uri_element(er.type);
        if (!elem) continue;  // unsupported element kind: skip (no-op)

        // Re-read the header each time: a previous element rule may have mutated
        // it, so its old view would be stale.
        HmrStr hv = hmr_rt_get_header(msg, name);
        HmrStr ev = (*elem == HMR_URI_WHOLE) ? hv : hmr_rt_uri_get(ctx, hv, *elem);

        bool ok = eval_match_guard(er.comparison, er.match_val_type,
                                   er.match_value, ev, ctx);
        if (!scan && !ok) continue;  // guard fail → skip this element rule

        switch (er.action) {
            case ElementAction::Replace: {
                HmrStr nv = eval_value(er.new_value, ctx);
                if (*elem == HMR_URI_WHOLE)
                    hmr_rt_set_header(msg, ctx, name, nv);
                else
                    hmr_rt_set_header(msg, ctx, name, hmr_rt_uri_set(ctx, hv, *elem, nv));
                break;
            }
            case ElementAction::DeleteElement:
                if (*elem == HMR_URI_WHOLE)
                    hmr_rt_delete_header(msg, name);
                else
                    hmr_rt_set_header(msg, ctx, name, hmr_rt_uri_set(ctx, hv, *elem, intern("")));
                break;
            case ElementAction::Store: {
                unsigned slot = slot_for(er.name.empty() ? (hr.name + ".el") : er.name);
                hmr_rt_store(ctx, slot, ev);
                break;
            }
            case ElementAction::Reject:
                hmr_rt_reject(ctx, 403u, er.new_value.empty() ? intern(er.name)
                                                              : eval_value(er.new_value, ctx));
                // A reject terminates apply, but the generator still *emits* the
                // following element rules (reachable only when this one's guard
                // fails), so the scan must keep registering their tables.
                if (!scan) return Flow::Rejected;
                break;
            default:
                break;  // Add/None/etc. on elements: not in v1
        }
    }
    return Flow::Continue;
}

// --- header rule (mirror Emitter::emit_header_rule) -------------------------

Interpreter::Flow Interpreter::run_header_rule(const HeaderRule& hr,
                                               std::size_t idx, HmrSipMsg* msg,
                                               HmrContext* ctx, bool scan) const {
    // Guard: message type.
    if (hr.msg_type == MsgType::Request) {
        if (!scan && hmr_rt_is_request(msg) == 0) return Flow::Continue;
    } else if (hr.msg_type == MsgType::Reply) {
        if (!scan && hmr_rt_is_request(msg) != 0) return Flow::Continue;
    }

    // Guard: method whitelist (case-insensitive OR across the listed methods).
    if (hr.has_method_filter()) {
        HmrStr method = hmr_rt_get_method(msg);
        bool acc = false;
        for (const std::string& m : hr.methods)
            acc = acc || (hmr_rt_str_eq(method, intern(m), /*ci=*/1) != 0);
        if (!scan && !acc) return Flow::Continue;
    }

    // Fetch the header value if the match-guard or action needs it.
    const bool need_hv = !hr.match_value.empty() || hr.action == HeaderAction::Store;
    HmrStr hv{nullptr, 0};
    bool have_hv = false;
    if (need_hv) {
        hv = hmr_rt_get_header(msg, intern(hr.header_name));
        have_hv = true;
    }

    // Guard: header-level match-value (no per-element match-val-type → Any).
    if (!hr.match_value.empty()) {
        bool ok = eval_match_guard(hr.comparison, MatchValType::Any,
                                   hr.match_value, hv, ctx);
        if (!scan && !ok) return Flow::Continue;
    }

    bool terminated = run_header_action(hr, idx, hv, have_hv, msg, ctx);
    if (terminated) return Flow::Rejected;  // reject also skips element rules

    // Element rules drive `manipulate` (and run after any other header action).
    if (!hr.element_rules.empty()) {
        if (run_element_rules(hr, msg, ctx, scan) == Flow::Rejected)
            return Flow::Rejected;
    }
    return Flow::Continue;
}

// --- top-level walk (mirror Emitter::emit) ----------------------------------

int Interpreter::walk(HmrSipMsg* msg, HmrContext* ctx, bool scan) const {
    for (std::size_t i = 0; i < rs_.header_rules.size(); ++i) {
        Flow f = run_header_rule(rs_.header_rules[i], i, msg, ctx, scan);
        // At runtime a reject returns immediately; during the scan every rule is
        // emitted, so keep walking to register the remaining tables.
        if (!scan && f == Flow::Rejected) return HMR_REJECTED;
    }
    return HMR_OK;
}

}  // namespace hmr::interp
