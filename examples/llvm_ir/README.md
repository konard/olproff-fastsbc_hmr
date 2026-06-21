# LLVM IR examples — before / after optimization

These `.ll` files show the **textual LLVM IR** the compiler generates directly
from the HMR AST (no intermediate C++, no Clang frontend), and the same IR after
the backend runs its optimization pipeline. They are the concrete artifact
behind [`docs/llvm-ir-examples.md`](../../docs/llvm-ir-examples.md), which walks
through the diff and explains every transform.

| File | Stage | Produced by |
|---|---|---|
| `minimal_ruleset.before.ll` | generated IR | `IrGenerator` (Visitor + `IRBuilder`) |
| `minimal_ruleset.after.ll`  | optimized IR | `HmrAttributePass` + LLVM `-O3` |
| `multiple_headers.before.ll`| generated IR | `IrGenerator` |
| `multiple_headers.after.ll` | optimized IR | `HmrAttributePass` + LLVM `-O3` |

## Regenerate

The files are exactly what `hmrc dump-ir` prints — nothing is hand-edited:

```sh
# before optimization (straight from the IR generator)
hmrc dump-ir tests/hmr_samples/minimal_ruleset.hmr  > examples/llvm_ir/minimal_ruleset.before.ll

# after optimization (HmrAttributePass + -O3 module pipeline)
hmrc dump-ir tests/hmr_samples/minimal_ruleset.hmr --opt > examples/llvm_ir/minimal_ruleset.after.ll
```

`dump-ir` requires an LLVM build (it shares the backend with `compile`).

> The `.after.ll` files were generated on `x86_64-pc-linux-gnu`; the `-O3`
> attribute inference and the emitted `target datalayout` are host-specific, so
> regenerating on another target will differ in those details (the structural
> changes below are the same everywhere).

## What to look for

* **`nounwind` on `hmr_apply` and every `hmr_rt_*` import** — added by our custom
  `HmrAttributePass`, which runs ahead of the standard pipeline at *every* opt
  level. This is the "custom LLVM pass" example in the IR.
* **Basic-block folding** — the generator emits one block per rule joined by
  `br label %rule.cont*`; `-O3` collapses the straight-line chain so only genuine
  conditional branches (regex guards) survive.
* **`tail call` + argument attributes** (`nonnull`, `noundef`, `nocapture
  readnone`) — inferred by `-O3`, letting the runtime callbacks tail-chain.
