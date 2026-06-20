// SPDX-License-Identifier: MIT
//
// IrGenerator.cpp — direct AST → LLVM IR lowering via IRBuilder.
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

#include "hmr/codegen/IrGenerator.hpp"

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

#include "hmr/ast/AstFactory.hpp"
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

// Map a built-in variable name ($LOCAL_IP, ...) to its stable HmrVarId. Unknown
// names resolve to HMR_VAR_NONE, which the runtime reports as the empty string.
HmrVarId variableId(std::string_view name) {
    std::string up;
    up.reserve(name.size());
    for (char c : name) up.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(c))));
    static const std::map<std::string, HmrVarId> kMap = {
        {"LOCAL_IP", HMR_VAR_LOCAL_IP},     {"REMOTE_IP", HMR_VAR_REMOTE_IP},
        {"LOCAL_PORT", HMR_VAR_LOCAL_PORT}, {"REMOTE_PORT", HMR_VAR_REMOTE_PORT},
        {"TRUNK_GROUP", HMR_VAR_TRUNK_GROUP}, {"REALM", HMR_VAR_REALM},
        {"INTERFACE", HMR_VAR_INTERFACE},   {"METHOD", HMR_VAR_METHOD},
        {"RURI_USER", HMR_VAR_RURI_USER},   {"RURI_HOST", HMR_VAR_RURI_HOST},
        {"TO_USER", HMR_VAR_TO_USER},       {"TO_HOST", HMR_VAR_TO_HOST},
        {"FROM_USER", HMR_VAR_FROM_USER},   {"FROM_HOST", HMR_VAR_FROM_HOST},
    };
    auto it = kMap.find(up);
    return it == kMap.end() ? HMR_VAR_NONE : it->second;
}

// Map an element-rule type to a URI element selector. Returns nullopt for
// element kinds the v1 generator does not yet manipulate (params, etc.).
std::optional<uint32_t> uriElement(ElementType t) {
    switch (t) {
        case ElementType::HeaderValue: return HMR_URI_WHOLE;
        case ElementType::UriDisplay:  return HMR_URI_DISPLAY;
        case ElementType::UriUser:     return HMR_URI_USER;
        case ElementType::UriHost:     return HMR_URI_HOST;
        case ElementType::UriPort:     return HMR_URI_PORT;
        default:                       return std::nullopt;
    }
}

bool isCaseInsensitive(ComparisonType c) {
    return c == ComparisonType::CaseInsensitive ||
           c == ComparisonType::ReferCaseInsensitive;
}
bool isPattern(ComparisonType c) { return c == ComparisonType::PatternRule; }

// ---------------------------------------------------------------------------
// Emitter — builds the module via IRBuilder, walking the AST in Visitor order.
// ---------------------------------------------------------------------------
class Emitter {
public:
    Emitter(LLVMContext& ctx, Module& mod, const IrGenOptions& opts)
        : ctx_(ctx), mod_(mod), b_(ctx), opts_(opts) {
        ptrTy_ = PointerType::getUnqual(ctx_);
        i32Ty_ = Type::getInt32Ty(ctx_);
        voidTy_ = Type::getVoidTy(ctx_);
        hmrStrTy_ = StructType::get(ctx_, {ptrTy_, i32Ty_});
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
    IrStr rtGetHeader(IrStr name) {
        return unpack(b_.CreateCall(
            rt("hmr_rt_get_header", hmrStrTy_, {ptrTy_, ptrTy_, i32Ty_}),
            {msg_, name.data, name.len}));
    }
    void rtSetHeader(IrStr name, IrStr val) {
        b_.CreateCall(rt("hmr_rt_set_header", i32Ty_,
                         {ptrTy_, ptrTy_, i32Ty_, ptrTy_, i32Ty_}),
                      {msg_, name.data, name.len, val.data, val.len});
    }
    void rtAddHeader(IrStr name, IrStr val) {
        b_.CreateCall(rt("hmr_rt_add_header", i32Ty_,
                         {ptrTy_, ptrTy_, i32Ty_, ptrTy_, i32Ty_}),
                      {msg_, name.data, name.len, val.data, val.len});
    }
    void rtDeleteHeader(IrStr name) {
        b_.CreateCall(rt("hmr_rt_delete_header", i32Ty_, {ptrTy_, ptrTy_, i32Ty_}),
                      {msg_, name.data, name.len});
    }
    IrStr rtGetMethod() {
        return unpack(b_.CreateCall(rt("hmr_rt_get_method", hmrStrTy_, {ptrTy_}),
                                    {msg_}));
    }
    llvm::Value* rtIsRequest() {
        return b_.CreateCall(rt("hmr_rt_is_request", i32Ty_, {ptrTy_}), {msg_});
    }
    llvm::Value* rtStrEq(IrStr a, IrStr b, bool ci) {
        return b_.CreateCall(
            rt("hmr_rt_str_eq", i32Ty_, {ptrTy_, i32Ty_, ptrTy_, i32Ty_, i32Ty_}),
            {a.data, a.len, b.data, b.len, ci32(ci ? 1 : 0)});
    }
    llvm::Value* rtRegexMatch(unsigned id, IrStr subj) {
        return b_.CreateCall(
            rt("hmr_rt_regex_match", i32Ty_, {ptrTy_, i32Ty_, ptrTy_, i32Ty_}),
            {ctx_arg_, ci32(id), subj.data, subj.len});
    }
    IrStr rtGetCapture(unsigned idx) {
        return unpack(b_.CreateCall(
            rt("hmr_rt_get_capture", hmrStrTy_, {ptrTy_, i32Ty_}),
            {ctx_arg_, ci32(idx)}));
    }
    IrStr rtGetVar(unsigned id) {
        return unpack(b_.CreateCall(rt("hmr_rt_get_var", hmrStrTy_, {ptrTy_, i32Ty_}),
                                    {ctx_arg_, ci32(id)}));
    }
    void rtStore(unsigned slot, IrStr v) {
        b_.CreateCall(rt("hmr_rt_store", voidTy_, {ptrTy_, i32Ty_, ptrTy_, i32Ty_}),
                      {ctx_arg_, ci32(slot), v.data, v.len});
    }
    IrStr rtLoad(unsigned slot) {
        return unpack(b_.CreateCall(rt("hmr_rt_load", hmrStrTy_, {ptrTy_, i32Ty_}),
                                    {ctx_arg_, ci32(slot)}));
    }
    void rtValReset() {
        b_.CreateCall(rt("hmr_rt_val_reset", voidTy_, {ptrTy_}), {ctx_arg_});
    }
    void rtValAppendLit(IrStr lit) {
        b_.CreateCall(rt("hmr_rt_val_append_lit", voidTy_, {ptrTy_, ptrTy_, i32Ty_}),
                      {ctx_arg_, lit.data, lit.len});
    }
    void rtValAppendVar(unsigned id) {
        b_.CreateCall(rt("hmr_rt_val_append_var", voidTy_, {ptrTy_, i32Ty_}),
                      {ctx_arg_, ci32(id)});
    }
    void rtValAppendCapture(unsigned idx) {
        b_.CreateCall(rt("hmr_rt_val_append_capture", voidTy_, {ptrTy_, i32Ty_}),
                      {ctx_arg_, ci32(idx)});
    }
    void rtValAppendSlot(unsigned slot) {
        b_.CreateCall(rt("hmr_rt_val_append_slot", voidTy_, {ptrTy_, i32Ty_}),
                      {ctx_arg_, ci32(slot)});
    }
    IrStr rtValFinish() {
        return unpack(b_.CreateCall(rt("hmr_rt_val_finish", hmrStrTy_, {ptrTy_}),
                                    {ctx_arg_}));
    }
    IrStr rtUriGet(IrStr hv, uint32_t elem) {
        return unpack(b_.CreateCall(
            rt("hmr_rt_uri_get", hmrStrTy_, {ptrTy_, ptrTy_, i32Ty_, i32Ty_}),
            {ctx_arg_, hv.data, hv.len, ci32(elem)}));
    }
    IrStr rtUriSet(IrStr hv, uint32_t elem, IrStr nv) {
        return unpack(b_.CreateCall(
            rt("hmr_rt_uri_set", hmrStrTy_,
               {ptrTy_, ptrTy_, i32Ty_, i32Ty_, ptrTy_, i32Ty_}),
            {ctx_arg_, hv.data, hv.len, ci32(elem), nv.data, nv.len}));
    }
    void rtLog(IrStr m) {
        b_.CreateCall(rt("hmr_rt_log", voidTy_, {ptrTy_, ptrTy_, i32Ty_}),
                      {ctx_arg_, m.data, m.len});
    }
    void rtReject(unsigned code, IrStr reason) {
        b_.CreateCall(rt("hmr_rt_reject", voidTy_, {ptrTy_, i32Ty_, ptrTy_, i32Ty_}),
                      {ctx_arg_, ci32(code), reason.data, reason.len});
    }

    // --- tables ------------------------------------------------------------
    unsigned addRegex(std::string pattern, bool ci) {
        uint32_t flags = ci ? 1u : 0u;
        for (unsigned i = 0; i < regexes_.size(); ++i)
            if (regexes_[i].first == pattern && regexes_[i].second == flags)
                return i;
        regexes_.emplace_back(std::move(pattern), flags);
        return static_cast<unsigned>(regexes_.size() - 1);
    }
    unsigned slotFor(const std::string& name) {
        auto [it, ins] = slots_.try_emplace(name, static_cast<unsigned>(slots_.size()));
        return it->second;
    }
    unsigned slotForRef(const std::string& path) {
        std::string base = path.substr(0, path.find('.'));
        return slotFor(base);
    }

    // --- control-flow helpers ---------------------------------------------
    BasicBlock* block(const Twine& name) {
        return BasicBlock::Create(ctx_, name, applyFn_);
    }
    // Branch to `okBB` when `condI1` holds, else to `skip`; continue at okBB.
    void guard(llvm::Value* condI1, const Twine& okName, BasicBlock* skip) {
        BasicBlock* ok = block(okName);
        b_.CreateCondBr(condI1, ok, skip);
        b_.SetInsertPoint(ok);
    }
    llvm::Value* truthy(llvm::Value* i32v) { return b_.CreateICmpNE(i32v, ci32(0)); }
    BasicBlock* rejectBlock() {
        if (!rejectBB_) rejectBB_ = BasicBlock::Create(ctx_, "reject", applyFn_);
        return rejectBB_;
    }

    // --- value & guard emission -------------------------------------------
    IrStr emitValue(const Value& v);
    void emitMatchGuard(ComparisonType cmp, const Value& mv, IrStr subject,
                        BasicBlock* skip);

    // --- rule emission (Visitor-style) ------------------------------------
    void emitHeaderRule(const HeaderRule& hr, std::size_t idx);
    bool emitHeaderAction(const HeaderRule& hr, std::size_t idx, IrStr hv,
                          bool haveHv);  // returns true if it terminated the block
    void emitElementRules(const HeaderRule& hr);

    void buildModuleInfo(const std::string& moduleName);

    LLVMContext& ctx_;
    Module& mod_;
    IRBuilder<> b_;
    const IrGenOptions& opts_;

    PointerType* ptrTy_;
    Type* i32Ty_;
    Type* voidTy_;
    StructType* hmrStrTy_;

    Function* applyFn_ = nullptr;
    llvm::Value* msg_ = nullptr;
    llvm::Value* ctx_arg_ = nullptr;
    BasicBlock* retOkBB_ = nullptr;
    BasicBlock* rejectBB_ = nullptr;

    std::map<std::string, GlobalVariable*> strs_;
    std::vector<std::pair<std::string, uint32_t>> regexes_;
    std::map<std::string, unsigned> slots_;
};

IrStr Emitter::emitValue(const Value& v) {
    if (v.empty()) return literal("");
    if (v.isPureLiteral()) return literal(v.literalText());

    if (v.isSingleRef()) {
        const auto& ref = v.segments().front().ref;
        switch (ref.kind) {
            case RefKind::Variable:
                return rtGetVar(static_cast<unsigned>(variableId(ref.name)));
            case RefKind::Capture:
                return rtGetCapture(static_cast<unsigned>(std::max(0, ref.captureIndex)));
            case RefKind::RuleRef:
                return rtLoad(slotForRef(ref.name));
        }
    }

    // Multi-segment expression: build into the context scratch buffer.
    rtValReset();
    for (const ValueSegment& seg : v.segments()) {
        if (!seg.isRef) {
            if (!seg.literal.empty()) rtValAppendLit(literal(seg.literal));
            continue;
        }
        switch (seg.ref.kind) {
            case RefKind::Variable:
                rtValAppendVar(static_cast<unsigned>(variableId(seg.ref.name)));
                break;
            case RefKind::Capture:
                rtValAppendCapture(static_cast<unsigned>(std::max(0, seg.ref.captureIndex)));
                break;
            case RefKind::RuleRef:
                rtValAppendSlot(slotForRef(seg.ref.name));
                break;
        }
    }
    return rtValFinish();
}

void Emitter::emitMatchGuard(ComparisonType cmp, const Value& mv, IrStr subject,
                             BasicBlock* skip) {
    if (mv.empty()) return;  // no match-value → always applies

    // A $reference / interpolated match-value (boolean back-reference) is not yet
    // evaluated as a guard; treat as always-true so the rule still fires
    // (documented v1 gap).
    if (!mv.isRegexLiteral() && !mv.isPureLiteral()) return;

    // Oracle HMR match-values are *regular expressions* for every comparison
    // type — `case-sensitive`/`case-insensitive` merely toggle the regex case
    // flag, while `pattern-rule` additionally exposes captures via $0/$1. We
    // therefore lower every literal match-value to a precompiled regex evaluated
    // with std::regex_search, which is exactly what the runtime does. Comparing
    // literally (str_eq) was wrong: a pattern such as `internal\.local` would
    // never match the host `internal.local` because of the escaping backslash,
    // and an IP like `10\.0\.0\.1` would never match `10.0.0.1`.
    const bool ci = isCaseInsensitive(cmp);
    const std::string pattern = mv.isRegexLiteral() ? mv.literalText() : mv.raw();
    unsigned id = addRegex(pattern, ci);
    llvm::Value* cond = truthy(rtRegexMatch(id, subject));
    if (mv.negated()) cond = b_.CreateNot(cond);
    guard(cond, "match.ok", skip);
}

bool Emitter::emitHeaderAction(const HeaderRule& hr, std::size_t idx, IrStr hv,
                               bool haveHv) {
    const IrStr name = literal(hr.headerName);
    switch (hr.action) {
        case HeaderAction::Add:
            rtAddHeader(name, emitValue(hr.newValue));
            return false;
        case HeaderAction::Replace:
        case HeaderAction::Manipulate /*header-level replace via new-value*/:
            if (!hr.newValue.empty() && hr.elementRules.empty()) {
                rtSetHeader(name, emitValue(hr.newValue));
                return false;
            }
            return false;  // pure manipulate → handled by element rules
        case HeaderAction::Delete:
        case HeaderAction::DeleteHeader:
            rtDeleteHeader(name);
            return false;
        case HeaderAction::Store: {
            unsigned slot = slotFor(hr.name.empty() ? ("rule" + std::to_string(idx)) : hr.name);
            if (isPattern(hr.comparison))
                rtStore(slot, rtGetCapture(0));
            else
                rtStore(slot, haveHv ? hv : rtGetHeader(name));
            return false;
        }
        case HeaderAction::Log:
            rtLog(hr.newValue.empty() ? literal(hr.name) : emitValue(hr.newValue));
            return false;
        case HeaderAction::Reject:
            rtReject(403u, hr.newValue.empty() ? literal(hr.name)
                                               : emitValue(hr.newValue));
            b_.CreateBr(rejectBlock());
            return true;
        default:
            return false;  // None / DeleteElement / etc. — no header-level act
    }
}

void Emitter::emitElementRules(const HeaderRule& hr) {
    const IrStr name = literal(hr.headerName);
    for (const ElementRule& er : hr.elementRules) {
        std::optional<uint32_t> elem = uriElement(er.type);
        if (!elem) continue;  // unsupported element kind: skip (no-op)

        BasicBlock* cont = block("el.cont");
        // Re-read the header each time: a previous element rule may have
        // mutated it, so its old view would be stale.
        IrStr hv = rtGetHeader(name);
        IrStr ev = (*elem == HMR_URI_WHOLE) ? hv : rtUriGet(hv, *elem);

        emitMatchGuard(er.comparison, er.matchValue, ev, cont);

        bool terminated = false;
        switch (er.action) {
            case ElementAction::Replace: {
                IrStr nv = emitValue(er.newValue);
                if (*elem == HMR_URI_WHOLE)
                    rtSetHeader(name, nv);
                else
                    rtSetHeader(name, rtUriSet(hv, *elem, nv));
                break;
            }
            case ElementAction::DeleteElement:
                if (*elem == HMR_URI_WHOLE)
                    rtDeleteHeader(name);
                else
                    rtSetHeader(name, rtUriSet(hv, *elem, literal("")));
                break;
            case ElementAction::Store: {
                unsigned slot = slotFor(er.name.empty() ? (hr.name + ".el") : er.name);
                rtStore(slot, ev);
                break;
            }
            case ElementAction::Reject:
                rtReject(403u, er.newValue.empty() ? literal(er.name)
                                                   : emitValue(er.newValue));
                b_.CreateBr(rejectBlock());
                terminated = true;
                break;
            default:
                break;  // Add/None/etc. on elements: not in v1
        }
        if (!terminated) b_.CreateBr(cont);
        b_.SetInsertPoint(cont);
    }
}

void Emitter::emitHeaderRule(const HeaderRule& hr, std::size_t idx) {
    BasicBlock* cont = block("rule.cont");

    // Guard: message type.
    if (hr.msgType == MsgType::Request)
        guard(truthy(rtIsRequest()), "is.req", cont);
    else if (hr.msgType == MsgType::Reply)
        guard(b_.CreateNot(truthy(rtIsRequest())), "is.reply", cont);

    // Guard: method whitelist (case-insensitive OR across the listed methods).
    if (hr.hasMethodFilter()) {
        IrStr method = rtGetMethod();
        llvm::Value* acc = ConstantInt::getFalse(ctx_);
        for (const std::string& m : hr.methods)
            acc = b_.CreateOr(acc, truthy(rtStrEq(method, literal(m), /*ci=*/true)));
        guard(acc, "method.ok", cont);
    }

    // Fetch the header value if the match-guard or action needs it.
    const bool needHv = !hr.matchValue.empty() || hr.action == HeaderAction::Store;
    IrStr hv{nullptr, nullptr};
    bool haveHv = false;
    if (needHv) {
        hv = rtGetHeader(literal(hr.headerName));
        haveHv = true;
    }

    // Guard: header-level match-value.
    if (!hr.matchValue.empty())
        emitMatchGuard(hr.comparison, hr.matchValue, hv, cont);

    bool terminated = emitHeaderAction(hr, idx, hv, haveHv);

    // Element rules drive `manipulate` (and run after any other header action).
    if (!terminated && !hr.elementRules.empty()) emitElementRules(hr);

    if (!terminated) b_.CreateBr(cont);
    b_.SetInsertPoint(cont);
}

void Emitter::buildModuleInfo(const std::string& moduleName) {
    // Regex table: [N x %HmrRegexEntry], referenced by hmr_module_info.
    StructType* regexEntryTy = StructType::create(ctx_, {ptrTy_, i32Ty_}, "HmrRegexEntry");
    llvm::Constant* regexesPtr = ConstantPointerNull::get(ptrTy_);
    if (!regexes_.empty()) {
        std::vector<llvm::Constant*> entries;
        entries.reserve(regexes_.size());
        for (const auto& [pat, flags] : regexes_) {
            IrStr s = literal(pat);  // interned NUL-terminated pattern
            entries.push_back(ConstantStruct::get(
                regexEntryTy, {cast<llvm::Constant>(s.data), ci32(flags)}));
        }
        ArrayType* arrTy = ArrayType::get(regexEntryTy, entries.size());
        auto* table = new GlobalVariable(mod_, arrTy, /*isConstant=*/true,
                                         GlobalValue::PrivateLinkage,
                                         ConstantArray::get(arrTy, entries),
                                         "hmr.regexes");
        table->setUnnamedAddr(GlobalValue::UnnamedAddr::Global);
        regexesPtr = table;
    }

    StructType* modInfoTy = StructType::create(
        ctx_, {i32Ty_, ptrTy_, i32Ty_, i32Ty_, ptrTy_}, "HmrModuleInfo");
    IrStr nameStr = literal(moduleName);
    llvm::Constant* init = ConstantStruct::get(
        modInfoTy,
        {ci32(HMR_ABI_VERSION), cast<llvm::Constant>(nameStr.data),
         ci32(static_cast<uint32_t>(slots_.size())),
         ci32(static_cast<uint32_t>(regexes_.size())), regexesPtr});

    auto* info = new GlobalVariable(mod_, modInfoTy, /*isConstant=*/true,
                                    GlobalValue::ExternalLinkage, init,
                                    "hmr_module_info");
    info->setAlignment(Align(8));
}

Result<void> Emitter::emit(const Ruleset& rs, const opt::DecisionPlan& plan,
                           IrModuleStats* stats) {
    (void)plan;  // emission preserves original order; plan is reported as stats

    FunctionType* applyTy = FunctionType::get(i32Ty_, {ptrTy_, ptrTy_}, false);
    applyFn_ = Function::Create(applyTy, GlobalValue::ExternalLinkage, "hmr_apply", mod_);
    applyFn_->getArg(0)->setName("msg");
    applyFn_->getArg(1)->setName("ctx");
    msg_ = applyFn_->getArg(0);
    ctx_arg_ = applyFn_->getArg(1);

    BasicBlock* entry = BasicBlock::Create(ctx_, "entry", applyFn_);
    retOkBB_ = BasicBlock::Create(ctx_, "ret.ok", applyFn_);
    b_.SetInsertPoint(entry);

    for (std::size_t i = 0; i < rs.headerRules.size(); ++i)
        emitHeaderRule(rs.headerRules[i], i);

    b_.CreateBr(retOkBB_);

    b_.SetInsertPoint(retOkBB_);
    b_.CreateRet(ci32(HMR_OK));

    if (rejectBB_) {
        b_.SetInsertPoint(rejectBB_);
        b_.CreateRet(ci32(HMR_REJECTED));
    }

    const std::string moduleName = rs.name.empty() ? "hmr" : rs.name;
    buildModuleInfo(moduleName);

    std::string err;
    raw_string_ostream os(err);
    if (verifyModule(mod_, &os)) {
        os.flush();
        return make_error("internal: generated IR failed verification:\n" + err);
    }

    if (stats) {
        stats->moduleName = moduleName;
        stats->numSlots = static_cast<unsigned>(slots_.size());
        stats->numRegexes = static_cast<unsigned>(regexes_.size());
        stats->numHeaderRules = static_cast<unsigned>(rs.headerRules.size());
    }
    return {};
}

}  // namespace

Result<std::string> IrGenerator::generateIR(const ast::Ruleset& rs,
                                            const opt::DecisionPlan& plan,
                                            const IrGenOptions& opts,
                                            IrModuleStats* stats) {
    LLVMContext context;
    Module mod(opts.moduleId, context);
    mod.setTargetTriple(opts.targetTriple.empty()
                            ? llvm::sys::getDefaultTargetTriple()
                            : opts.targetTriple);

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
