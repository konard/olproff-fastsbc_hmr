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

// ir_generator.cpp — direct AST → LLVM IR lowering via IRBuilder.
//
// The generator emits one function, `hmr_apply(HmrSipMsg*, HmrContext*)`, whose
// body is a straight-line sequence of guarded rule applications, plus a static
// `hmr_module_info` descriptor (name + slot count + precompiled regex table).
// All state lives in the caller-owned HmrContext; the generated code only calls
// the `hmr_rt_*` runtime entry points (resolved against the host at load time).
//
// ABI note: HmrStr { const char*; uint32_t } is passed by value as two scalars
// (ptr, i32) and returned as the literal struct { ptr, i32 }. This matches
// exactly what Clang emits for the C header on x86-64 SysV (verified against
// clang-18), so the generated module and the g++-compiled runtime agree.

#include "hmr/codegen/ir_generator.hpp"

#include <algorithm>
#include <cctype>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/GlobalVariable.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Support/Alignment.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/TargetParser/Host.h"

#include "hmr/ast/ast_factory.hpp"
#include "hmr/codegen/lowering.hpp"
#include "hmr/runtime/hmr_runtime.h"

namespace hmr::codegen {

namespace {

using namespace llvm;
using hmr::ast::ComparisonType;
using hmr::ast::ElementAction;
using hmr::ast::ElementRule;
using hmr::ast::ElementType;
using hmr::ast::HeaderAction;
using hmr::ast::HeaderRule;
using hmr::ast::MatchValType;
using hmr::ast::MsgType;
using hmr::ast::RefKind;
using hmr::ast::Ruleset;
using hmr::ast::Value;
using hmr::ast::ValueSegment;

// An HmrStr decomposed into its ABI scalars.
struct IrStr {
    llvm::Value* data;  // i8* (opaque ptr)
    llvm::Value* len;   // i32
};

// The pure AST → runtime-immediate mappings (variable_id, uri_element,
// is_case_insensitive, is_pattern, ip_match_type) live in codegen/lowering.hpp
// so the interpreter oracle (src/interp/interpreter.cpp) lowers identically.
// They are declared in this namespace (hmr::codegen), so the unqualified calls
// below resolve to them.

// ---------------------------------------------------------------------------
// Emitter — builds the module via IRBuilder, walking the AST in Visitor order.
// ---------------------------------------------------------------------------
class Emitter {
public:
    Emitter(LLVMContext& ctx, Module& mod, const IrGenOptions& opts)
        : ctx_(ctx), mod_(mod), b_(ctx), opts_(opts) {
        ptr_ty_ = PointerType::getUnqual(ctx_);
        i32_ty_ = Type::getInt32Ty(ctx_);
        void_ty_ = Type::getVoidTy(ctx_);
        hmr_str_ty_ = StructType::get(ctx_, {ptr_ty_, i32_ty_});
    }

    Result<void> emit(const Ruleset& rs, const opt::DecisionPlan& plan,
                      IrModuleStats* stats);

private:
    // --- low-level builders ------------------------------------------------
    ConstantInt* ci32(uint32_t v) { return b_.getInt32(v); }

    // Intern a string literal as a private [N+1 x i8] constant; return the
    // HmrStr view (pointer to the bytes + length, NUL excluded).
    IrStr literal(std::string_view text) {
        auto it = strs_.find(std::string(text));
        GlobalVariable* gv;
        if (it != strs_.end()) {
            gv = it->second;
        } else {
            Constant* c = ConstantDataArray::getString(ctx_, text, /*AddNull=*/true);
            gv = new GlobalVariable(mod_, c->getType(), /*isConstant=*/true,
                                    GlobalValue::PrivateLinkage, c, ".hmrstr");
            gv->setUnnamedAddr(GlobalValue::UnnamedAddr::Global);
            gv->setAlignment(Align(1));
            strs_.emplace(std::string(text), gv);
        }
        return {gv, ci32(static_cast<uint32_t>(text.size()))};
    }

    FunctionCallee rt(const char* name, Type* ret, ArrayRef<Type*> args) {
        return mod_.getOrInsertFunction(name, FunctionType::get(ret, args, false));
    }
    IrStr unpack(CallInst* c) {
        return {b_.CreateExtractValue(c, 0), b_.CreateExtractValue(c, 1)};
    }

    // --- runtime callbacks (coerced ABI) -----------------------------------
    IrStr rt_get_header(IrStr name) {
        return unpack(b_.CreateCall(
            rt("hmr_rt_get_header", hmr_str_ty_, {ptr_ty_, ptr_ty_, i32_ty_}),
            {msg_, name.data, name.len}));
    }
    void rt_set_header(IrStr name, IrStr val) {
        b_.CreateCall(rt("hmr_rt_set_header", i32_ty_,
                         {ptr_ty_, ptr_ty_, ptr_ty_, i32_ty_, ptr_ty_, i32_ty_}),
                      {msg_, ctx_arg_, name.data, name.len, val.data, val.len});
    }
    void rt_add_header(IrStr name, IrStr val) {
        b_.CreateCall(rt("hmr_rt_add_header", i32_ty_,
                         {ptr_ty_, ptr_ty_, ptr_ty_, i32_ty_, ptr_ty_, i32_ty_}),
                      {msg_, ctx_arg_, name.data, name.len, val.data, val.len});
    }
    void rt_delete_header(IrStr name) {
        b_.CreateCall(rt("hmr_rt_delete_header", i32_ty_, {ptr_ty_, ptr_ty_, i32_ty_}),
                      {msg_, name.data, name.len});
    }
    IrStr rt_get_method() {
        return unpack(b_.CreateCall(rt("hmr_rt_get_method", hmr_str_ty_, {ptr_ty_}),
                                    {msg_}));
    }
    llvm::Value* rt_is_request() {
        return b_.CreateCall(rt("hmr_rt_is_request", i32_ty_, {ptr_ty_}), {msg_});
    }
    llvm::Value* rt_str_eq(IrStr a, IrStr b, bool ci) {
        return b_.CreateCall(
            rt("hmr_rt_str_eq", i32_ty_, {ptr_ty_, i32_ty_, ptr_ty_, i32_ty_, i32_ty_}),
            {a.data, a.len, b.data, b.len, ci32(ci ? 1 : 0)});
    }
    // Unified match-val-type dispatch (review #2). For HMR_MATCH_REGEX the
    // runtime uses `regex_id` (and records captures) and ignores `pattern`; for
    // every other type `pattern` is the literal match-value and `regex_id` is
    // ignored.
    llvm::Value* rt_match(uint32_t match_type, IrStr subject, IrStr pattern,
                          unsigned regex_id) {
        return b_.CreateCall(
            rt("hmr_rt_match", i32_ty_,
               {ptr_ty_, i32_ty_, ptr_ty_, i32_ty_, ptr_ty_, i32_ty_, i32_ty_}),
            {ctx_arg_, ci32(match_type), subject.data, subject.len, pattern.data,
             pattern.len, ci32(regex_id)});
    }
    IrStr rt_get_capture(unsigned idx) {
        return unpack(b_.CreateCall(
            rt("hmr_rt_get_capture", hmr_str_ty_, {ptr_ty_, i32_ty_}),
            {ctx_arg_, ci32(idx)}));
    }
    IrStr rt_get_var(unsigned id) {
        return unpack(b_.CreateCall(rt("hmr_rt_get_var", hmr_str_ty_, {ptr_ty_, i32_ty_}),
                                    {ctx_arg_, ci32(id)}));
    }
    void rt_store(unsigned slot, IrStr v) {
        b_.CreateCall(rt("hmr_rt_store", void_ty_, {ptr_ty_, i32_ty_, ptr_ty_, i32_ty_}),
                      {ctx_arg_, ci32(slot), v.data, v.len});
    }
    IrStr rt_load(unsigned slot) {
        return unpack(b_.CreateCall(rt("hmr_rt_load", hmr_str_ty_, {ptr_ty_, i32_ty_}),
                                    {ctx_arg_, ci32(slot)}));
    }
    void rt_val_reset() {
        b_.CreateCall(rt("hmr_rt_val_reset", void_ty_, {ptr_ty_}), {ctx_arg_});
    }
    void rt_val_append_lit(IrStr lit) {
        b_.CreateCall(rt("hmr_rt_val_append_lit", void_ty_, {ptr_ty_, ptr_ty_, i32_ty_}),
                      {ctx_arg_, lit.data, lit.len});
    }
    void rt_val_append_var(unsigned id) {
        b_.CreateCall(rt("hmr_rt_val_append_var", void_ty_, {ptr_ty_, i32_ty_}),
                      {ctx_arg_, ci32(id)});
    }
    void rt_val_append_capture(unsigned idx) {
        b_.CreateCall(rt("hmr_rt_val_append_capture", void_ty_, {ptr_ty_, i32_ty_}),
                      {ctx_arg_, ci32(idx)});
    }
    void rt_val_append_slot(unsigned slot) {
        b_.CreateCall(rt("hmr_rt_val_append_slot", void_ty_, {ptr_ty_, i32_ty_}),
                      {ctx_arg_, ci32(slot)});
    }
    IrStr rt_val_finish() {
        return unpack(b_.CreateCall(rt("hmr_rt_val_finish", hmr_str_ty_, {ptr_ty_}),
                                    {ctx_arg_}));
    }
    IrStr rt_uri_get(IrStr hv, uint32_t elem) {
        return unpack(b_.CreateCall(
            rt("hmr_rt_uri_get", hmr_str_ty_, {ptr_ty_, ptr_ty_, i32_ty_, i32_ty_}),
            {ctx_arg_, hv.data, hv.len, ci32(elem)}));
    }
    IrStr rt_uri_set(IrStr hv, uint32_t elem, IrStr nv) {
        return unpack(b_.CreateCall(
            rt("hmr_rt_uri_set", hmr_str_ty_,
               {ptr_ty_, ptr_ty_, i32_ty_, i32_ty_, ptr_ty_, i32_ty_}),
            {ctx_arg_, hv.data, hv.len, ci32(elem), nv.data, nv.len}));
    }
    void rt_log(IrStr m) {
        b_.CreateCall(rt("hmr_rt_log", void_ty_, {ptr_ty_, ptr_ty_, i32_ty_}),
                      {ctx_arg_, m.data, m.len});
    }
    void rt_reject(unsigned code, IrStr reason) {
        b_.CreateCall(rt("hmr_rt_reject", void_ty_, {ptr_ty_, i32_ty_, ptr_ty_, i32_ty_}),
                      {ctx_arg_, ci32(code), reason.data, reason.len});
    }

    // --- tables ------------------------------------------------------------
    unsigned add_regex(std::string pattern, bool ci) {
        uint32_t flags = ci ? 1u : 0u;
        for (unsigned i = 0; i < regexes_.size(); ++i)
            if (regexes_[i].first == pattern && regexes_[i].second == flags)
                return i;
        regexes_.emplace_back(std::move(pattern), flags);
        return static_cast<unsigned>(regexes_.size() - 1);
    }
    unsigned slot_for(const std::string& name) {
        auto [it, ins] = slots_.try_emplace(name, static_cast<unsigned>(slots_.size()));
        return it->second;
    }
    // Resolve a `$rule.$N` back-reference to the slot the matching pattern-rule
    // store wrote: group N of the base rule (N defaults to 0 -> the whole match).
    unsigned slot_for_ref(const std::string& path, int capture_index) {
        return slot_for(capture_slot_key(
            ref_base_name(path), static_cast<unsigned>(std::max(0, capture_index))));
    }

    // --- control-flow helpers ---------------------------------------------
    BasicBlock* block(const Twine& name) {
        return BasicBlock::Create(ctx_, name, apply_fn_);
    }
    // Branch to `ok_bb` when `cond_i1` holds, else to `skip`; continue at ok_bb.
    void guard(llvm::Value* cond_i1, const Twine& ok_name, BasicBlock* skip) {
        BasicBlock* ok = block(ok_name);
        b_.CreateCondBr(cond_i1, ok, skip);
        b_.SetInsertPoint(ok);
    }
    llvm::Value* truthy(llvm::Value* i32v) { return b_.CreateICmpNE(i32v, ci32(0)); }
    BasicBlock* reject_block() {
        if (!reject_bb_) reject_bb_ = BasicBlock::Create(ctx_, "reject", apply_fn_);
        return reject_bb_;
    }

    // --- value & guard emission -------------------------------------------
    IrStr emit_value(const Value& v);
    void emit_match_guard(ComparisonType cmp, MatchValType mvt, const Value& mv,
                          IrStr subject, BasicBlock* skip);

    // --- rule emission (Visitor-style) ------------------------------------
    void emit_header_rule(const HeaderRule& hr, std::size_t idx);
    bool emit_header_action(const HeaderRule& hr, std::size_t idx, IrStr hv,
                            bool have_hv);  // returns true if it terminated the block
    void emit_element_rules(const HeaderRule& hr);

    void build_module_info(const std::string& module_name);

    LLVMContext& ctx_;
    Module& mod_;
    IRBuilder<> b_;
    const IrGenOptions& opts_;

    PointerType* ptr_ty_;
    Type* i32_ty_;
    Type* void_ty_;
    StructType* hmr_str_ty_;

    Function* apply_fn_ = nullptr;
    llvm::Value* msg_ = nullptr;
    llvm::Value* ctx_arg_ = nullptr;
    BasicBlock* ret_ok_bb_ = nullptr;
    BasicBlock* reject_bb_ = nullptr;

    std::map<std::string, GlobalVariable*> strs_;
    std::vector<std::pair<std::string, uint32_t>> regexes_;
    std::map<std::string, unsigned> slots_;
};

IrStr Emitter::emit_value(const Value& v) {
    if (v.empty()) return literal("");
    if (v.is_pure_literal()) return literal(v.literal_text());

    if (v.is_single_ref()) {
        const auto& ref = v.segments().front().ref;
        switch (ref.kind) {
            case RefKind::Variable:
                return rt_get_var(static_cast<unsigned>(variable_id(ref.name)));
            case RefKind::Capture:
                return rt_get_capture(static_cast<unsigned>(std::max(0, ref.capture_index)));
            case RefKind::RuleRef:
                return rt_load(slot_for_ref(ref.name, ref.capture_index));
        }
    }

    // Multi-segment expression: build into the context scratch buffer.
    rt_val_reset();
    for (const ValueSegment& seg : v.segments()) {
        if (!seg.is_ref) {
            if (!seg.literal.empty()) rt_val_append_lit(literal(seg.literal));
            continue;
        }
        switch (seg.ref.kind) {
            case RefKind::Variable:
                rt_val_append_var(static_cast<unsigned>(variable_id(seg.ref.name)));
                break;
            case RefKind::Capture:
                rt_val_append_capture(static_cast<unsigned>(std::max(0, seg.ref.capture_index)));
                break;
            case RefKind::RuleRef:
                rt_val_append_slot(slot_for_ref(seg.ref.name, seg.ref.capture_index));
                break;
        }
    }
    return rt_val_finish();
}

void Emitter::emit_match_guard(ComparisonType cmp, MatchValType mvt,
                               const Value& mv, IrStr subject,
                               BasicBlock* skip) {
    if (mv.empty()) return;  // no match-value → always applies

    // A $reference / interpolated match-value (boolean back-reference) is not
    // yet evaluated as a guard; treat as always-true so the rule still fires
    // (documented v1 gap).
    if (!mv.is_regex_literal() && !mv.is_pure_literal()) return;

    // The literal match-value text (the leading '!' negation, if any, is already
    // stripped by Value::parse and reflected in mv.negated()).
    const std::string pattern =
        mv.is_pure_literal() ? mv.literal_text() : std::string{mv.raw()};

    // Select the specialized matcher (review #2): `pattern-rule` is an explicit
    // regex; an ip/fqdn match-val-type routes to the dedicated matcher; anything
    // else is an exact byte compare (case-folded per comparison-type). Routing
    // every value through std::regex was the old, slow design.
    uint32_t match_type;
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

    IrStr pat = literal(pattern);
    llvm::Value* cond = truthy(rt_match(match_type, subject, pat, regex_id));
    if (mv.negated()) cond = b_.CreateNot(cond);
    guard(cond, "match.ok", skip);
}

bool Emitter::emit_header_action(const HeaderRule& hr, std::size_t idx, IrStr hv,
                                 bool have_hv) {
    const IrStr name = literal(hr.header_name);
    switch (hr.action) {
        case HeaderAction::Add:
            rt_add_header(name, emit_value(hr.new_value));
            return false;
        case HeaderAction::Replace:
        case HeaderAction::Manipulate /*header-level replace via new-value*/:
            if (!hr.new_value.empty() && hr.element_rules.empty()) {
                rt_set_header(name, emit_value(hr.new_value));
                return false;
            }
            return false;  // pure manipulate → handled by element rules
        case HeaderAction::Delete:
        case HeaderAction::DeleteHeader:
            rt_delete_header(name);
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
                    rt_store(slot_for(capture_slot_key(rule, g)), rt_get_capture(g));
            } else {
                rt_store(slot_for(rule), have_hv ? hv : rt_get_header(name));
            }
            return false;
        }
        case HeaderAction::Log:
            rt_log(hr.new_value.empty() ? literal(hr.name) : emit_value(hr.new_value));
            return false;
        case HeaderAction::Reject:
            rt_reject(403u, hr.new_value.empty() ? literal(hr.name)
                                                 : emit_value(hr.new_value));
            b_.CreateBr(reject_block());
            return true;
        default:
            return false;  // None / DeleteElement / etc. — no header-level act
    }
}

void Emitter::emit_element_rules(const HeaderRule& hr) {
    const IrStr name = literal(hr.header_name);
    for (const ElementRule& er : hr.element_rules) {
        std::optional<uint32_t> elem = uri_element(er.type);
        if (!elem) continue;  // unsupported element kind: skip (no-op)

        BasicBlock* cont = block("el.cont");
        // Re-read the header each time: a previous element rule may have
        // mutated it, so its old view would be stale.
        IrStr hv = rt_get_header(name);
        IrStr ev = (*elem == HMR_URI_WHOLE) ? hv : rt_uri_get(hv, *elem);

        emit_match_guard(er.comparison, er.match_val_type, er.match_value, ev,
                         cont);

        bool terminated = false;
        switch (er.action) {
            case ElementAction::Replace: {
                IrStr nv = emit_value(er.new_value);
                if (*elem == HMR_URI_WHOLE)
                    rt_set_header(name, nv);
                else
                    rt_set_header(name, rt_uri_set(hv, *elem, nv));
                break;
            }
            case ElementAction::DeleteElement:
                if (*elem == HMR_URI_WHOLE)
                    rt_delete_header(name);
                else
                    rt_set_header(name, rt_uri_set(hv, *elem, literal("")));
                break;
            case ElementAction::Store: {
                unsigned slot = slot_for(er.name.empty() ? (hr.name + ".el") : er.name);
                rt_store(slot, ev);
                break;
            }
            case ElementAction::Reject:
                rt_reject(403u, er.new_value.empty() ? literal(er.name)
                                                     : emit_value(er.new_value));
                b_.CreateBr(reject_block());
                terminated = true;
                break;
            default:
                break;  // Add/None/etc. on elements: not in v1
        }
        if (!terminated) b_.CreateBr(cont);
        b_.SetInsertPoint(cont);
    }
}

void Emitter::emit_header_rule(const HeaderRule& hr, std::size_t idx) {
    BasicBlock* cont = block("rule.cont");

    // Guard: message type.
    if (hr.msg_type == MsgType::Request)
        guard(truthy(rt_is_request()), "is.req", cont);
    else if (hr.msg_type == MsgType::Reply)
        guard(b_.CreateNot(truthy(rt_is_request())), "is.reply", cont);

    // Guard: method whitelist (case-insensitive OR across the listed methods).
    if (hr.has_method_filter()) {
        IrStr method = rt_get_method();
        llvm::Value* acc = ConstantInt::getFalse(ctx_);
        for (const std::string& m : hr.methods)
            acc = b_.CreateOr(acc, truthy(rt_str_eq(method, literal(m), /*ci=*/true)));
        guard(acc, "method.ok", cont);
    }

    // Fetch the header value if the match-guard or action needs it.
    const bool need_hv = !hr.match_value.empty() || hr.action == HeaderAction::Store;
    IrStr hv{nullptr, nullptr};
    bool have_hv = false;
    if (need_hv) {
        hv = rt_get_header(literal(hr.header_name));
        have_hv = true;
    }

    // Guard: header-level match-value (no per-element match-val-type → Any).
    if (!hr.match_value.empty())
        emit_match_guard(hr.comparison, MatchValType::Any, hr.match_value, hv,
                         cont);

    bool terminated = emit_header_action(hr, idx, hv, have_hv);

    // Element rules drive `manipulate` (and run after any other header action).
    if (!terminated && !hr.element_rules.empty()) emit_element_rules(hr);

    if (!terminated) b_.CreateBr(cont);
    b_.SetInsertPoint(cont);
}

void Emitter::build_module_info(const std::string& module_name) {
    // Regex table: [N x %HmrRegexEntry], referenced by hmr_module_info.
    StructType* regex_entry_ty = StructType::create(ctx_, {ptr_ty_, i32_ty_}, "HmrRegexEntry");
    llvm::Constant* regexes_ptr = ConstantPointerNull::get(ptr_ty_);
    if (!regexes_.empty()) {
        std::vector<llvm::Constant*> entries;
        entries.reserve(regexes_.size());
        for (const auto& [pat, flags] : regexes_) {
            IrStr s = literal(pat);  // interned NUL-terminated pattern
            entries.push_back(ConstantStruct::get(
                regex_entry_ty, {cast<llvm::Constant>(s.data), ci32(flags)}));
        }
        ArrayType* arr_ty = ArrayType::get(regex_entry_ty, entries.size());
        auto* table = new GlobalVariable(mod_, arr_ty, /*isConstant=*/true,
                                         GlobalValue::PrivateLinkage,
                                         ConstantArray::get(arr_ty, entries),
                                         "hmr.regexes");
        table->setUnnamedAddr(GlobalValue::UnnamedAddr::Global);
        regexes_ptr = table;
    }

    StructType* mod_info_ty = StructType::create(
        ctx_, {i32_ty_, ptr_ty_, i32_ty_, i32_ty_, ptr_ty_}, "HmrModuleInfo");
    IrStr name_str = literal(module_name);
    llvm::Constant* init = ConstantStruct::get(
        mod_info_ty,
        {ci32(HMR_ABI_VERSION), cast<llvm::Constant>(name_str.data),
         ci32(static_cast<uint32_t>(slots_.size())),
         ci32(static_cast<uint32_t>(regexes_.size())), regexes_ptr});

    auto* info = new GlobalVariable(mod_, mod_info_ty, /*isConstant=*/true,
                                    GlobalValue::ExternalLinkage, init,
                                    "hmr_module_info");
    info->setAlignment(Align(8));
}

Result<void> Emitter::emit(const Ruleset& rs, const opt::DecisionPlan& plan,
                           IrModuleStats* stats) {
    (void)plan;  // emission preserves original order; plan is reported as stats

    FunctionType* apply_ty = FunctionType::get(i32_ty_, {ptr_ty_, ptr_ty_}, false);
    apply_fn_ = Function::Create(apply_ty, GlobalValue::ExternalLinkage, "hmr_apply", mod_);
    apply_fn_->getArg(0)->setName("msg");
    apply_fn_->getArg(1)->setName("ctx");
    msg_ = apply_fn_->getArg(0);
    ctx_arg_ = apply_fn_->getArg(1);

    BasicBlock* entry = BasicBlock::Create(ctx_, "entry", apply_fn_);
    ret_ok_bb_ = BasicBlock::Create(ctx_, "ret.ok", apply_fn_);
    b_.SetInsertPoint(entry);

    for (std::size_t i = 0; i < rs.header_rules.size(); ++i)
        emit_header_rule(rs.header_rules[i], i);

    b_.CreateBr(ret_ok_bb_);

    b_.SetInsertPoint(ret_ok_bb_);
    b_.CreateRet(ci32(HMR_OK));

    if (reject_bb_) {
        b_.SetInsertPoint(reject_bb_);
        b_.CreateRet(ci32(HMR_REJECTED));
    }

    const std::string module_name = rs.name.empty() ? "hmr" : rs.name;
    build_module_info(module_name);

    std::string err;
    raw_string_ostream os(err);
    if (verifyModule(mod_, &os)) {
        os.flush();
        return make_error("internal: generated IR failed verification:\n" + err);
    }

    if (stats) {
        stats->module_name = module_name;
        stats->num_slots = static_cast<unsigned>(slots_.size());
        stats->num_regexes = static_cast<unsigned>(regexes_.size());
        stats->num_header_rules = static_cast<unsigned>(rs.header_rules.size());
    }
    return {};
}

}  // namespace

Result<std::string> IrGenerator::generate_ir(const ast::Ruleset& rs,
                                             const opt::DecisionPlan& plan,
                                             const IrGenOptions& opts,
                                             IrModuleStats* stats) {
    LLVMContext context;
    Module mod(opts.module_id, context);
    mod.setTargetTriple(opts.target_triple.empty()
                            ? llvm::sys::getDefaultTargetTriple()
                            : opts.target_triple);

    Emitter emitter(context, mod, opts);
    if (auto r = emitter.emit(rs, plan, stats); !r)
        return std::unexpected(r.error());

    std::string out;
    llvm::raw_string_ostream os(out);
    mod.print(os, nullptr);
    os.flush();
    return out;
}

}  // namespace hmr::codegen
