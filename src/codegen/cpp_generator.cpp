// SPDX-License-Identifier: MIT
//
// cpp_generator.cpp — AST → C++ source lowering (the "GCC approach" back-end).
//
// A method-for-method mirror of src/codegen/ir_generator.cpp. Where the LLVM
// emitter builds basic blocks and SSA values, this one appends C++ statements;
// where LLVM decomposes HmrStr into (ptr, i32) scalars, this passes whole HmrStr
// structs by value (the C++ compiler lowers the ABI). The control flow that
// ir_generator.cpp expresses with conditional branches to a shared `cont` /
// `reject` block is expressed here with one immediately-invoked lambda per rule
// (and per element rule) returning bool: returning `false` falls through to the
// next rule/element; returning `true` is a reject that propagates out as
// HMR_REJECTED.
//
// Both back-ends call into codegen/lowering.hpp for every lowering decision and
// walk the AST in the same order, so they assign identical slot indices / regex
// ids and produce byte-identical packet output (the differential fuzzer and the
// interp-vs-compiled test assert exactly this).

#include "hmr/codegen/cpp_generator.hpp"

#include <algorithm>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "hmr/codegen/lowering.hpp"
#include "hmr/runtime/hmr_runtime.h"

namespace hmr::codegen {

namespace {

using hmr::ast::ComparisonType;
using hmr::ast::ElementAction;
using hmr::ast::ElementRule;
using hmr::ast::HeaderAction;
using hmr::ast::HeaderRule;
using hmr::ast::MatchValType;
using hmr::ast::MsgType;
using hmr::ast::RefKind;
using hmr::ast::Ruleset;
using hmr::ast::Value;
using hmr::ast::ValueSegment;

// Escape a byte string into the body of a C++ string literal (no surrounding
// quotes). Bytes outside printable ASCII become 3-digit octal escapes (always
// exactly 3 digits, so a following digit cannot extend the escape).
std::string esc(std::string_view s) {
    std::string out;
    out.reserve(s.size() + 2);
    for (char ch : s) {
        const auto c = static_cast<unsigned char>(ch);
        switch (c) {
            case '\\': out += "\\\\"; break;
            case '"': out += "\\\""; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c < 0x20 || c >= 0x7f) {
                    char buf[5] = {'\\',
                                   static_cast<char>('0' + ((c >> 6) & 7)),
                                   static_cast<char>('0' + ((c >> 3) & 7)),
                                   static_cast<char>('0' + (c & 7)), '\0'};
                    out += buf;
                } else {
                    out.push_back(static_cast<char>(c));
                }
        }
    }
    return out;
}

// Symbolic names for the runtime enums, so the generated source reads like
// hand-written C++ rather than a wall of integers. The values come from
// lowering.hpp / hmr_runtime.h; these switches only pick a spelling and fall back
// to the raw integer for anything unexpected.
std::string var_enum(HmrVarId v) {
    switch (v) {
        case HMR_VAR_NONE: return "HMR_VAR_NONE";
        case HMR_VAR_LOCAL_IP: return "HMR_VAR_LOCAL_IP";
        case HMR_VAR_REMOTE_IP: return "HMR_VAR_REMOTE_IP";
        case HMR_VAR_LOCAL_PORT: return "HMR_VAR_LOCAL_PORT";
        case HMR_VAR_REMOTE_PORT: return "HMR_VAR_REMOTE_PORT";
        case HMR_VAR_TRUNK_GROUP: return "HMR_VAR_TRUNK_GROUP";
        case HMR_VAR_REALM: return "HMR_VAR_REALM";
        case HMR_VAR_INTERFACE: return "HMR_VAR_INTERFACE";
        case HMR_VAR_METHOD: return "HMR_VAR_METHOD";
        case HMR_VAR_RURI_USER: return "HMR_VAR_RURI_USER";
        case HMR_VAR_RURI_HOST: return "HMR_VAR_RURI_HOST";
        case HMR_VAR_TO_USER: return "HMR_VAR_TO_USER";
        case HMR_VAR_TO_HOST: return "HMR_VAR_TO_HOST";
        case HMR_VAR_FROM_USER: return "HMR_VAR_FROM_USER";
        case HMR_VAR_FROM_HOST: return "HMR_VAR_FROM_HOST";
        default: break;
    }
    return std::to_string(static_cast<unsigned>(v)) + "u";
}

std::string match_enum(std::uint32_t t) {
    switch (t) {
        case HMR_MATCH_EXACT: return "HMR_MATCH_EXACT";
        case HMR_MATCH_EXACT_CI: return "HMR_MATCH_EXACT_CI";
        case HMR_MATCH_REGEX: return "HMR_MATCH_REGEX";
        case HMR_MATCH_IP: return "HMR_MATCH_IP";
        case HMR_MATCH_IP_MASK: return "HMR_MATCH_IP_MASK";
        case HMR_MATCH_IP_RANGE: return "HMR_MATCH_IP_RANGE";
        case HMR_MATCH_FQDN: return "HMR_MATCH_FQDN";
        default: return std::to_string(t) + "u";
    }
}

std::string uri_enum(std::uint32_t e) {
    switch (e) {
        case HMR_URI_WHOLE: return "HMR_URI_WHOLE";
        case HMR_URI_DISPLAY: return "HMR_URI_DISPLAY";
        case HMR_URI_USER: return "HMR_URI_USER";
        case HMR_URI_HOST: return "HMR_URI_HOST";
        case HMR_URI_PORT: return "HMR_URI_PORT";
        default: return std::to_string(e) + "u";
    }
}

// ---------------------------------------------------------------------------
// CppEmitter — appends C++ statements, walking the AST in Visitor order.
// ---------------------------------------------------------------------------
class CppEmitter {
public:
    explicit CppEmitter(const CppGenOptions& opts) : opts_(opts) {}

    std::string generate(const Ruleset& rs, CppGenStats* stats);

private:
    // --- low-level source builders ----------------------------------------
    std::string next_temp() { return "t" + std::to_string(temp_++); }
    void line(const std::string& s) {
        body_.append(static_cast<std::size_t>(indent_) * 4, ' ');
        body_ += s;
        body_.push_back('\n');
    }
    void blank() { body_.push_back('\n'); }
    void open(const std::string& s) {
        line(s);
        ++indent_;
    }
    void close(const std::string& s) {
        --indent_;
        line(s);
    }
    void comment(const std::string& s) {
        if (opts_.emit_comments) line("// " + s);
    }

    // An HmrStr literal expression: HmrStr{"<escaped>", <len>u}.
    static std::string str_lit(std::string_view s) {
        return "HmrStr{\"" + esc(s) + "\", " + std::to_string(s.size()) + "u}";
    }
    static std::string uint_lit(unsigned n) { return std::to_string(n) + "u"; }

    // --- tables (identical bookkeeping to ir_generator.cpp) ----------------
    unsigned add_regex(std::string pattern, bool ci) {
        std::uint32_t flags = ci ? 1u : 0u;
        for (unsigned i = 0; i < regexes_.size(); ++i)
            if (regexes_[i].first == pattern && regexes_[i].second == flags)
                return i;
        regexes_.emplace_back(std::move(pattern), flags);
        return static_cast<unsigned>(regexes_.size() - 1);
    }
    unsigned slot_for(const std::string& name) {
        auto [it, ins] =
            slots_.try_emplace(name, static_cast<unsigned>(slots_.size()));
        return it->second;
    }
    unsigned slot_for_ref(const std::string& path, int capture_index) {
        return slot_for(capture_slot_key(
            ref_base_name(path), static_cast<unsigned>(std::max(0, capture_index))));
    }

    // --- value & guard emission -------------------------------------------
    // Emits any preparatory statements and returns a C++ expression of type
    // HmrStr (a temp name or an inline literal). Mirrors Emitter::emit_value.
    std::string emit_value(const Value& v);
    // Emits `if (<guard fails>) return false;` so a non-matching subject skips
    // the enclosing rule/element lambda. Mirrors Emitter::emit_match_guard.
    void emit_match_guard(ComparisonType cmp, MatchValType mvt, const Value& mv,
                          const std::string& subject);

    // --- rule emission (Visitor-style) ------------------------------------
    void emit_header_rule(const HeaderRule& hr, std::size_t idx);
    // Returns true if the action terminated the rule (a reject).
    bool emit_header_action(const HeaderRule& hr, std::size_t idx,
                            const std::string& hv, bool have_hv);
    void emit_element_rules(const HeaderRule& hr);

    const CppGenOptions& opts_;

    std::string body_;
    unsigned temp_ = 0;
    int indent_ = 1;  // statements live inside hmr_apply's body

    std::vector<std::pair<std::string, std::uint32_t>> regexes_;
    std::map<std::string, unsigned> slots_;
};

std::string CppEmitter::emit_value(const Value& v) {
    if (v.empty()) return str_lit("");
    if (v.is_pure_literal()) return str_lit(v.literal_text());

    if (v.is_single_ref()) {
        const auto& ref = v.segments().front().ref;
        const std::string t = next_temp();
        switch (ref.kind) {
            case RefKind::Variable:
                line("const HmrStr " + t + " = hmr_rt_get_var(ctx, " +
                     var_enum(variable_id(ref.name)) + ");");
                return t;
            case RefKind::Capture:
                line("const HmrStr " + t + " = hmr_rt_get_capture(ctx, " +
                     uint_lit(static_cast<unsigned>(std::max(0, ref.capture_index))) +
                     ");");
                return t;
            case RefKind::RuleRef:
                line("const HmrStr " + t + " = hmr_rt_load(ctx, " +
                     uint_lit(slot_for_ref(ref.name, ref.capture_index)) + ");");
                return t;
        }
    }

    // Multi-segment expression: build into the context scratch buffer.
    line("hmr_rt_val_reset(ctx);");
    for (const ValueSegment& seg : v.segments()) {
        if (!seg.is_ref) {
            if (!seg.literal.empty())
                line("hmr_rt_val_append_lit(ctx, " + str_lit(seg.literal) + ");");
            continue;
        }
        switch (seg.ref.kind) {
            case RefKind::Variable:
                line("hmr_rt_val_append_var(ctx, " +
                     var_enum(variable_id(seg.ref.name)) + ");");
                break;
            case RefKind::Capture:
                line("hmr_rt_val_append_capture(ctx, " +
                     uint_lit(static_cast<unsigned>(
                         std::max(0, seg.ref.capture_index))) +
                     ");");
                break;
            case RefKind::RuleRef:
                line("hmr_rt_val_append_slot(ctx, " +
                     uint_lit(slot_for_ref(seg.ref.name, seg.ref.capture_index)) +
                     ");");
                break;
        }
    }
    const std::string t = next_temp();
    line("const HmrStr " + t + " = hmr_rt_val_finish(ctx);");
    return t;
}

void CppEmitter::emit_match_guard(ComparisonType cmp, MatchValType mvt,
                                  const Value& mv, const std::string& subject) {
    if (mv.empty()) return;  // no match-value → always applies

    // A $reference / interpolated match-value (boolean back-reference) is not yet
    // evaluated as a guard; treat as always-true (documented v1 gap).
    if (!mv.is_regex_literal() && !mv.is_pure_literal()) return;

    const std::string pattern =
        mv.is_pure_literal() ? mv.literal_text() : std::string{mv.raw()};

    // Select the specialized matcher (review #2): pattern-rule → regex; an
    // ip/fqdn match-val-type → the dedicated matcher; otherwise an exact byte
    // compare (case-folded per comparison-type). Routing everything through
    // std::regex was the old, slow design.
    std::uint32_t match_type;
    unsigned regex_id = 0;
    if (is_pattern(cmp)) {
        match_type = HMR_MATCH_REGEX;
        regex_id = add_regex(pattern, is_case_insensitive(cmp));
    } else {
        switch (mvt) {
            case MatchValType::Ip: match_type = ip_match_type(pattern); break;
            case MatchValType::Fqdn: match_type = HMR_MATCH_FQDN; break;
            case MatchValType::Any:
            default:
                match_type = is_case_insensitive(cmp) ? HMR_MATCH_EXACT_CI
                                                      : HMR_MATCH_EXACT;
                break;
        }
    }

    const std::string call = "hmr_rt_match(ctx, " + match_enum(match_type) + ", " +
                             subject + ", " + str_lit(pattern) + ", " +
                             uint_lit(regex_id) + ")";
    // The guard fires the rule when the subject matches; a negated match-value
    // (!pattern) flips that. Emit the skip condition directly.
    line("if (" + call + (mv.negated() ? " != 0" : " == 0") + ") return false;");
}

bool CppEmitter::emit_header_action(const HeaderRule& hr, std::size_t idx,
                                    const std::string& hv, bool have_hv) {
    const std::string name = str_lit(hr.header_name);
    switch (hr.action) {
        case HeaderAction::Add: {
            const std::string v = emit_value(hr.new_value);
            line("hmr_rt_add_header(msg, ctx, " + name + ", " + v + ");");
            return false;
        }
        case HeaderAction::Replace:
        case HeaderAction::Manipulate:
            if (!hr.new_value.empty() && hr.element_rules.empty()) {
                const std::string v = emit_value(hr.new_value);
                line("hmr_rt_set_header(msg, ctx, " + name + ", " + v + ");");
            }
            return false;  // pure manipulate → handled by element rules
        case HeaderAction::Delete:
        case HeaderAction::DeleteHeader:
            line("hmr_rt_delete_header(msg, " + name + ");");
            return false;
        case HeaderAction::Store: {
            const std::string rule =
                hr.name.empty() ? ("rule" + std::to_string(idx)) : hr.name;
            if (is_pattern(hr.comparison)) {
                // Pattern-rule store: capture the whole match (group 0, keyed by
                // the bare rule name) and each sub-group under "rule.$N", so a
                // later $rule.$N back-reference loads group N (Oracle HMR).
                const std::string pat = hr.match_value.is_pure_literal()
                                            ? hr.match_value.literal_text()
                                            : std::string{hr.match_value.raw()};
                unsigned ngroups = count_capturing_groups(pat);
                for (unsigned g = 0; g <= ngroups; ++g) {
                    const std::string cap = next_temp();
                    line("const HmrStr " + cap + " = hmr_rt_get_capture(ctx, " +
                         uint_lit(g) + ");");
                    line("hmr_rt_store(ctx, " +
                         uint_lit(slot_for(capture_slot_key(rule, g))) + ", " +
                         cap + ");");
                }
            } else if (have_hv) {
                line("hmr_rt_store(ctx, " + uint_lit(slot_for(rule)) + ", " + hv +
                     ");");
            } else {
                const std::string gh = next_temp();
                line("const HmrStr " + gh + " = hmr_rt_get_header(msg, " + name +
                     ");");
                line("hmr_rt_store(ctx, " + uint_lit(slot_for(rule)) + ", " + gh +
                     ");");
            }
            return false;
        }
        case HeaderAction::Log: {
            const std::string v =
                hr.new_value.empty() ? str_lit(hr.name) : emit_value(hr.new_value);
            line("hmr_rt_log(ctx, " + v + ");");
            return false;
        }
        case HeaderAction::Reject: {
            const std::string v =
                hr.new_value.empty() ? str_lit(hr.name) : emit_value(hr.new_value);
            line("hmr_rt_reject(ctx, 403u, " + v + ");");
            line("return true;");
            return true;
        }
        default:
            return false;  // None / DeleteElement / etc. — no header-level act
    }
}

void CppEmitter::emit_element_rules(const HeaderRule& hr) {
    // Bind the header name once; each element lambda captures it by reference.
    const std::string nm = next_temp();
    line("const HmrStr " + nm + " = " + str_lit(hr.header_name) + ";");

    for (const ElementRule& er : hr.element_rules) {
        std::optional<std::uint32_t> elem = uri_element(er.type);
        if (!elem) continue;  // unsupported element kind: skip (no-op)

        comment(er.name.empty() ? "element-rule" : ("element-rule " + er.name));
        open("if ([&]() -> bool {");

        // Re-read the header each time: a previous element rule may have mutated
        // it, so its old view would be stale.
        const std::string hv = next_temp();
        line("const HmrStr " + hv + " = hmr_rt_get_header(msg, " + nm + ");");
        std::string ev;
        if (*elem == HMR_URI_WHOLE) {
            ev = hv;
        } else {
            ev = next_temp();
            line("const HmrStr " + ev + " = hmr_rt_uri_get(ctx, " + hv + ", " +
                 uri_enum(*elem) + ");");
        }

        emit_match_guard(er.comparison, er.match_val_type, er.match_value, ev);

        bool terminated = false;
        switch (er.action) {
            case ElementAction::Replace: {
                const std::string nv = emit_value(er.new_value);
                if (*elem == HMR_URI_WHOLE) {
                    line("hmr_rt_set_header(msg, ctx, " + nm + ", " + nv + ");");
                } else {
                    const std::string ns = next_temp();
                    line("const HmrStr " + ns + " = hmr_rt_uri_set(ctx, " + hv +
                         ", " + uri_enum(*elem) + ", " + nv + ");");
                    line("hmr_rt_set_header(msg, ctx, " + nm + ", " + ns + ");");
                }
                break;
            }
            case ElementAction::DeleteElement:
                if (*elem == HMR_URI_WHOLE) {
                    line("hmr_rt_delete_header(msg, " + nm + ");");
                } else {
                    const std::string ns = next_temp();
                    line("const HmrStr " + ns + " = hmr_rt_uri_set(ctx, " + hv +
                         ", " + uri_enum(*elem) + ", " + str_lit("") + ");");
                    line("hmr_rt_set_header(msg, ctx, " + nm + ", " + ns + ");");
                }
                break;
            case ElementAction::Store:
                line("hmr_rt_store(ctx, " +
                     uint_lit(slot_for(er.name.empty() ? (hr.name + ".el")
                                                       : er.name)) +
                     ", " + ev + ");");
                break;
            case ElementAction::Reject: {
                const std::string v = er.new_value.empty()
                                          ? str_lit(er.name)
                                          : emit_value(er.new_value);
                line("hmr_rt_reject(ctx, 403u, " + v + ");");
                line("return true;");
                terminated = true;
                break;
            }
            default:
                break;  // Add/None/etc. on elements: not in v1
        }
        if (!terminated) line("return false;");
        close("}()) return true;");
    }
}

void CppEmitter::emit_header_rule(const HeaderRule& hr, std::size_t idx) {
    comment(hr.name.empty() ? ("header-rule " + std::to_string(idx))
                            : ("header-rule " + std::to_string(idx) + ": " + hr.name));
    open("if ([&]() -> bool {");

    // Guard: message type.
    if (hr.msg_type == MsgType::Request)
        line("if (hmr_rt_is_request(msg) == 0) return false;");
    else if (hr.msg_type == MsgType::Reply)
        line("if (hmr_rt_is_request(msg) != 0) return false;");

    // Guard: method whitelist (case-insensitive OR across the listed methods).
    if (hr.has_method_filter()) {
        const std::string method = next_temp();
        line("const HmrStr " + method + " = hmr_rt_get_method(msg);");
        std::string cond;
        for (std::size_t i = 0; i < hr.methods.size(); ++i) {
            if (i) cond += " || ";
            cond += "hmr_rt_str_eq(" + method + ", " + str_lit(hr.methods[i]) +
                    ", 1) != 0";
        }
        line("if (!(" + cond + ")) return false;");
    }

    // Fetch the header value if the match-guard or action needs it.
    const bool need_hv =
        !hr.match_value.empty() || hr.action == HeaderAction::Store;
    std::string hv;
    bool have_hv = false;
    if (need_hv) {
        hv = next_temp();
        line("const HmrStr " + hv + " = hmr_rt_get_header(msg, " +
             str_lit(hr.header_name) + ");");
        have_hv = true;
    }

    // Guard: header-level match-value (no per-element match-val-type → Any).
    if (!hr.match_value.empty())
        emit_match_guard(hr.comparison, MatchValType::Any, hr.match_value, hv);

    bool terminated = emit_header_action(hr, idx, hv, have_hv);

    // Element rules drive `manipulate` (and run after any other header action).
    if (!terminated && !hr.element_rules.empty()) emit_element_rules(hr);

    if (!terminated) line("return false;");
    close("}()) return HMR_REJECTED;");
    blank();
}

std::string CppEmitter::generate(const Ruleset& rs, CppGenStats* stats) {
    const std::string module_name =
        !opts_.module_name.empty() ? opts_.module_name
        : rs.name.empty()          ? "hmr"
                                   : rs.name;

    for (std::size_t i = 0; i < rs.header_rules.size(); ++i)
        emit_header_rule(rs.header_rules[i], i);

    std::string out;
    out +=
        "// Generated by hmr::codegen::CppGenerator — the \"GCC approach\" "
        "back-end.\n";
    out +=
        "// HMR DSL -> C++ -> g++ -O3 -shared -fPIC -> .so, ABI-identical to the\n"
        "// LLVM-direct module (same hmr_apply + hmr_module_info; every hmr_rt_*\n"
        "// stays undefined and resolves against the host at dlopen). Do not "
        "edit;\n"
        "// regenerate from the ruleset.\n\n";
    out += "#include \"" + opts_.runtime_header + "\"\n\n";

    out += "extern \"C\" int hmr_apply(HmrSipMsg* msg, HmrContext* ctx) {\n";
    out += "    (void)msg;\n";
    out += "    (void)ctx;\n";
    out += body_;
    out += "    return HMR_OK;\n";
    out += "}\n\n";

    if (!regexes_.empty()) {
        out += "static const HmrRegexEntry hmr_regexes_[] = {\n";
        for (const auto& [pat, flags] : regexes_)
            out += "    { \"" + esc(pat) + "\", " + std::to_string(flags) +
                   "u },\n";
        out += "};\n\n";
    }

    out += "extern \"C\" const HmrModuleInfo hmr_module_info = {\n";
    out += "    HMR_ABI_VERSION,\n";
    out += "    \"" + esc(module_name) + "\",\n";
    out += "    " + std::to_string(slots_.size()) + "u,\n";
    out += "    " + std::to_string(regexes_.size()) + "u,\n";
    out += "    " +
           (regexes_.empty() ? std::string("nullptr") : std::string("hmr_regexes_")) +
           ",\n";
    out += "};\n";

    if (stats) {
        stats->module_name = module_name;
        stats->num_slots = static_cast<unsigned>(slots_.size());
        stats->num_regexes = static_cast<unsigned>(regexes_.size());
        stats->num_header_rules =
            static_cast<unsigned>(rs.header_rules.size());
    }
    return out;
}

}  // namespace

Result<std::string> CppGenerator::generate_cpp(const ast::Ruleset& rs,
                                               const CppGenOptions& opts,
                                               CppGenStats* stats) {
    CppEmitter emitter(opts);
    return emitter.generate(rs, stats);
}

}  // namespace hmr::codegen
