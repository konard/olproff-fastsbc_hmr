# LLVM IR examples — before / after optimization

The compiler emits **textual LLVM IR straight from the HMR AST** — an AST
`Visitor` driving an `IRBuilder`, with no intermediate C++ and no Clang
frontend. The backend then parses that IR, runs a custom analysis/transform pass
followed by LLVM's standard `-O3` module pipeline, and lowers the result to a
native object.

This page shows both ends of that process for two samples. Reproduce any of it
with the `dump-ir` driver subcommand:

```sh
hmrc dump-ir <file>          # IR straight from the generator (before)
hmrc dump-ir <file> --opt    # after HmrAttributePass + -O3        (after)
```

The raw `.ll` files are committed under
[`examples/llvm_ir/`](../examples/llvm_ir). A unit test
(`Codegen.OptimizeIrRunsCustomPassAndOptPipeline`) pins the transforms described
below so they can't silently regress.

---

## Example 1 — `minimal_ruleset.hmr`

A single rule that appends one header:

```hmr
sip-manipulation MinimalRuleset
        header-rule
                name           addSupported
                header-name    Supported
                action         add
                new-value      100rel
```

### Before — generated IR

```llvm
define i32 @hmr_apply(ptr %msg, ptr %ctx) {
entry:
  %0 = call i32 @hmr_rt_add_header(ptr %msg, ptr @.hmrstr, i32 9, ptr @.hmrstr.1, i32 6)
  br label %rule.cont

ret.ok:                                           ; preds = %rule.cont
  ret i32 0

rule.cont:                                        ; preds = %entry
  br label %ret.ok
}

declare i32 @hmr_rt_add_header(ptr, ptr, i32, ptr, i32)
```

The generator emits one basic block per rule, threaded together with
`br label %rule.cont*` and terminated at a shared `ret.ok`. It deliberately does
no peephole work — that is the optimizer's job.

### After — `--opt` (HmrAttributePass + `-O3`)

```llvm
; Function Attrs: nounwind
define noundef i32 @hmr_apply(ptr %msg, ptr nocapture readnone %ctx) local_unnamed_addr #0 {
entry:
  %0 = tail call i32 @hmr_rt_add_header(ptr %msg, ptr nonnull @.hmrstr, i32 9, ptr nonnull @.hmrstr.1, i32 6)
  ret i32 0
}

; Function Attrs: nounwind
declare i32 @hmr_rt_add_header(ptr, ptr, i32, ptr, i32) local_unnamed_addr #0

attributes #0 = { nounwind }
```

What changed:

* **`nounwind`** on `hmr_apply` and the `hmr_rt_add_header` import — added by the
  custom **`HmrAttributePass`** (see below). Our C ABI never propagates
  exceptions across the module boundary, so the optimizer may drop unwind edges.
* **Block chain folded away** — `entry → rule.cont → ret.ok` collapses to a
  single block ending in `ret i32 0`.
* **`tail call`**, plus `nonnull` / `noundef` / `nocapture readnone` — inferred
  by `-O3`.

---

## Example 2 — `multiple_headers.hmr` (block folding)

Five rules over different headers, including a `manipulate` with a regex guard.
The interesting part is how the straight-line prefix collapses while the *real*
branch (the regex match) is preserved. Generated IR — note the
block-per-rule chain:

```llvm
define i32 @hmr_apply(ptr %msg, ptr %ctx) {
entry:
  %0 = call i32 @hmr_rt_add_header(...)
  br label %rule.cont
rule.cont:
  %1 = call i32 @hmr_rt_add_header(...)
  br label %rule.cont1
rule.cont1:
  %2 = call i32 @hmr_rt_delete_header(...)
  br label %rule.cont2
rule.cont2:
  %3 = call { ptr, i32 } @hmr_rt_get_header(...)
  ; ... extract header value ...
  %6 = call i32 @hmr_rt_regex_match(ptr %ctx, i32 0, ptr %4, i32 %5)
  %7 = icmp ne i32 %6, 0
  br i1 %7, label %match.ok, label %el.cont
  ; ... more chained blocks ...
}
```

After `--opt`, the unconditional `add / add / delete / get / regex` prefix is one
straight-line `entry` block; only the genuine `match.ok` / `el.cont` conditional
from the regex guard remains:

```llvm
; Function Attrs: nounwind
define noundef i32 @hmr_apply(ptr %msg, ptr %ctx) local_unnamed_addr #0 {
entry:
  %0  = tail call i32 @hmr_rt_add_header(...)        ; advertiseAllow
  %1  = tail call i32 @hmr_rt_add_header(...)        ; advertiseSupported
  %2  = tail call i32 @hmr_rt_delete_header(...)     ; dropServer
  %3  = tail call { ptr, i32 } @hmr_rt_get_header(...)
  %6  = tail call i32 @hmr_rt_regex_match(ptr %ctx, i32 0, ptr %4, i32 %5)
  %.not = icmp eq i32 %6, 0
  br i1 %.not, label %el.cont, label %match.ok

el.cont:                                            ; preds = %match.ok, %entry
  %7  = tail call { ptr, i32 } @hmr_rt_get_var(ptr %ctx, i32 6)   ; $REALM
  %10 = tail call i32 @hmr_rt_add_header(...)        ; tagIngressRealm
  ret i32 0

match.ok:                                           ; preds = %entry
  %11 = tail call i32 @hmr_rt_set_header(...)        ; rewriteUserAgent
  br label %el.cont
}
```

Eight generated blocks become three; every call is a `tail call`; every function
carries `nounwind`. The full files are
[`multiple_headers.before.ll`](../examples/llvm_ir/multiple_headers.before.ll)
and [`multiple_headers.after.ll`](../examples/llvm_ir/multiple_headers.after.ll).

> Optimization is not always *smaller*: a sample whose regex guards get inlined
> (e.g. `regex_captures.hmr`) can grow a few lines at `-O3` as the inliner trades
> code size for a branch-free fast path. The win is in instruction count and
> spilling on the hot path, not line count.

---

## The custom pass — `HmrAttributePass`

The `nounwind` you see above is not stock `-O3`; it comes from a small in-tree
pass that runs *before* the standard pipeline at every optimization level
([`src/backend/Backend.cpp`](../src/backend/Backend.cpp)):

```cpp
struct HmrAttributePass : PassInfoMixin<HmrAttributePass> {
    PreservedAnalyses run(Module& m, ModuleAnalysisManager&) {
        bool changed = false;
        for (Function& f : m) {
            StringRef n = f.getName();
            if (n == "hmr_apply" || n.starts_with("hmr_rt_")) {
                if (!f.hasFnAttribute(Attribute::NoUnwind)) {
                    f.addFnAttr(Attribute::NoUnwind);
                    changed = true;
                }
            }
        }
        return changed ? PreservedAnalyses::none() : PreservedAnalyses::all();
    }
};
```

Marking the entry point and the runtime callbacks `nounwind` lets the rest of the
pipeline delete landing pads and unwind edges from `hmr_apply`, shrinking the
per-packet fast path. Because the pass runs at `-O0` too, you can see its effect
in isolation: `hmrc dump-ir <file> --opt` always shows `nounwind`, but `tail
call` and the block folding appear only when the `-O<n>` pipeline runs (`n > 0`).
This split — custom pass always, standard pipeline by level — is exactly what the
`Codegen.OptimizeIrRunsCustomPassAndOptPipeline` test asserts.

See [architecture.md](./architecture.md#backend--srcbackendbackendcpp) for where
this sits in the pipeline and [performance.md](./performance.md) for what it buys
on the hot path.
