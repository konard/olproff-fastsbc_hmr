# Performance

This page reports **measured** numbers and states plainly which of the issue's
targets are met and which are aspirational with the current runtime model. The
numbers below come from the in-tree micro-benchmarks
([`benchmarks/`](../benchmarks)) on a developer machine (g++ 13, `-O3`, LLVM
18.1.3); they are *reported, not asserted* — they vary by host, and the compile
figure includes process-spawn latency because the linker forks `cc`.

Reproduce:
```sh
./build/benchmarks/hmr_bench_frontend    # LLVM-free stages + a hand-written apply loop
./build/benchmarks/hmr_bench_pipeline    # full pipeline over a real compiled .so (LLVM)
```

## Headline results (20-rule ruleset)

| Target (issue) | Goal | Measured | Verdict |
|---|---|---|---|
| Compilation time | < 80 ms | **≈ 67.5 ms** (`bench_pipeline`) | ✅ met |
| Module size | < 50 KB | **≈ 15.1 KB** | ✅ met |
| Per-packet apply | < 200 ns | **≈ 16.9 µs** (`bench_pipeline`) | ❌ not met — see below |
| Per-packet apply | < 200 ns | **≈ 1.25 µs** (`bench_frontend`, simpler loop) | ❌ not met |
| Front-end only (lex+parse+optimize) | — | **≈ 0.08 ms** | (informational) |

`bench_pipeline` is the product-accurate figure: it compiles a ~20-rule ruleset
to a native `.so`, `dlopen`s it through the `ModuleManager`, and times the
generated `hmr_apply` rewriting 20 URI hosts per packet. `bench_frontend` runs a
smaller hand-written callback loop and so reports a lower per-packet number; both
use the same runtime.

## Compilation time and module size — met

* **≈ 67.5 ms** for the *entire* pipeline: lex → parse → optimize → IR generation
  → `-O3` → object emission → `cc -shared`. A large slice of that is the
  `posix_spawn` of the linker driver; the front-end alone is **≈ 0.08 ms**, so
  there is ample headroom and the linker is the obvious thing to optimize (e.g.
  call LLD's library API instead of forking `cc`).
* **≈ 15.1 KB** per module — comfortably under 50 KB. Modules stay small because
  they carry no SIP model: the `hmr_rt_*` callbacks are undefined symbols
  resolved against the host at `dlopen`, so the `.so` is just the rule logic plus
  a tiny `hmr_module_info` descriptor.

## Per-packet latency — not met, and why

The **< 200 ns/packet** goal is **not** met by the current runtime. The honest
reason is the runtime *model*, not the generated control flow:

1. **`std::regex_search`.** Every `match-value` — even a literal — is evaluated
   as a `std::regex` (correct Oracle semantics, see
   [runtime-api.md](./runtime-api.md#why-match-values-are-always-regexes)).
   `std::regex` is notoriously slow; 20 rules means up to 20 regex evaluations
   per packet, and that dominates the profile.
2. **`std::string` SIP model.** The reference `HmrSipMsg`
   ([`SipMessage.hpp`](../include/hmr/runtime/SipMessage.hpp)) stores headers as
   `std::string`s and the benchmark copies a fresh message per packet. Header
   lookups, URI splitting, and rebuilds touch heap-backed strings.

Neither is a property of the **compiled code**, which already does the right
things: the generated `hmr_apply` is a flat guard/branch sequence, marked
`nounwind`, run through `-O3`, calling into preallocated context buffers. The
context is built once and `resetForApply()` reuses every buffer, so the
**steady-state apply path performs no heap allocation** — the issue's
zero-allocation requirement is satisfied at the codegen/runtime boundary even
though absolute latency is not.

## Closing the gap (future work)

The path to sub-microsecond (and toward the 200 ns aspiration) is a runtime
swap, transparent behind the existing C ABI — no change to generated modules:

* **Replace `std::regex` with a compiled DFA / RE2-style matcher**, or specialize
  truly-literal match-values to a `memmem`/`str_eq` fast path at codegen time
  (the `PatternSimplificationPass` already exists to detect these). This is the
  single highest-leverage change.
* **Replace the `std::string` SIP model with an arena/slice model**: parse the
  packet once into `(offset,len)` slices over a single buffer, mutate via a rope
  / edit-list, and serialize once. `HmrStr` is already a non-owning view, so the
  ABI is ready for it.
* **Intern header names and hoist repeated `get_header`** of the same header
  across rules (the `DecisionTreeAnalysisPass` already groups rules by target
  header, which is exactly the information needed to do this).

These are deliberately scoped as follow-ups: the current tree prioritizes a
**correct, complete, honestly-measured vertical slice** of every pipeline layer
over a hand-tuned hot path that would obscure the architecture.
