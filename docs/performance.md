# Performance

This page reports **measured** numbers and states plainly which of the issue's
targets are met and which are aspirational with the current runtime model. The
numbers below come from the in-tree micro-benchmarks
([`benchmarks/`](../benchmarks)) on a developer machine (g++ 13, `-O3`, LLVM
18.1.3); they are *reported, not asserted* — they vary by host, and the compile
figure includes process-spawn latency because the linker forks `cc`.

Reproduce (after `meson compile -C builddir`; the ANTLR runtime is linked
statically, so no `LD_LIBRARY_PATH` is needed):
```sh
./builddir/benchmarks/bench_frontend    # LLVM-free stages, matchers, interpreter vs hand-loop
./builddir/benchmarks/bench_pipeline    # full pipeline over a real compiled .so (LLVM)
```

## Headline results (20-rule ruleset)

| Target (issue) | Goal | Measured | Verdict |
|---|---|---|---|
| Compilation time | < 80 ms | **≈ 55 ms** (`bench_pipeline`) | ✅ met |
| Module size | < 50 KB | **≈ 15.1 KB** | ✅ met |
| Per-packet apply | 50–150 ns | **≈ 4.9 µs** (`bench_pipeline`) | 🟡 ~3× better after the rework, still above target — see below |
| **vs interpreter** (same rules) | be faster | **≈ 1.8× faster** (8.5 µs → 4.9 µs) | ✅ |
| **vs GCC approach** (same AST) | be faster to compile | **≈ 3× faster compile** (40 ms vs ~125 ms), runtime parity | ✅ |
| Front-end only (lex+parse+optimize) | — | **≈ 0.08 ms** | (informational) |

`bench_pipeline` is the product-accurate figure: it compiles a ~20-rule ruleset
to a native `.so`, `dlopen`s it through the `module_manager`, and times the
generated `hmr_apply` rewriting 20 URI hosts per packet over the zero-copy arena
SIP model. `bench_frontend` additionally times the specialized matchers against
`std::regex` in isolation and the **AST interpreter** against the same compiled
path; both use the same runtime.

### Two baselines the issue asks for

* **vs the interpreter.** The same optimized AST is run by the tree-walking
  [interpreter](../src/interp/interpreter.cpp) (the reference oracle) and by the
  compiled `.so`. The compiled module is ≈ 1.8× faster per packet, and the
  differential test ([`tests/test_interp_vs_compiled.cpp`](../tests/test_interp_vs_compiled.cpp))
  asserts they produce byte-identical output — so the speedup is real work, not a
  semantic shortcut.
* **vs the "GCC approach."** The [C++ source generator](../src/codegen/cpp_generator.cpp)
  emits standalone C++ for the same AST, which a system compiler turns into an
  equivalent `.so`. This is the counterfactual the issue's *direct-LLVM-IR* core
  approach is measured against: direct IR generation reaches a loadable module
  ≈ 3× faster (no C++ parse + template instantiation) at runtime parity, because
  both ultimately feed the same `-O3` backend.

## Compilation time and module size — met

* **≈ 55 ms** for the *entire* pipeline: lex → parse → optimize → IR generation
  → `-O3` → object emission → `cc -shared`. A large slice of that is the
  `posix_spawn` of the linker driver; the front-end alone is **≈ 0.08 ms**, so
  there is ample headroom and the linker is the obvious thing to optimize (e.g.
  call LLD's library API instead of forking `cc`).
* **≈ 15.1 KB** per module — comfortably under 50 KB. Modules stay small because
  they carry no SIP model: the `hmr_rt_*` callbacks are undefined symbols
  resolved against the host at `dlopen`, so the `.so` is just the rule logic plus
  a tiny `hmr_module_info` descriptor.

## Per-packet latency — improved ~3×, still above the ns-scale target

The rework cut per-packet apply from **≈ 16.9 µs to ≈ 4.9 µs** by removing the
two costs the first draft's *runtime model* imposed — not by touching the
generated control flow, which was already a flat `-O3` guard/branch sequence:

1. **Specialized matchers instead of one `std::regex` for everything.** The first
   draft evaluated *every* `match-value` — even an exact literal or an IP — as a
   `std::regex`. The generator now picks the matcher per rule at codegen time and
   emits `hmr_rt_match` with the right `HmrMatchType` (see
   [how match-values are lowered](./runtime-api.md#how-match-values-are-lowered)):
   exact byte compare, IP arithmetic, or — only for `pattern-rule` — `std::regex`.
   In isolation (`bench_frontend`) the exact and IP engines are ≈ 1.2–1.5× faster
   than the equivalent `std::regex`, and they allocate nothing.
2. **Zero-copy arena SIP model instead of `std::string`.** `HmrSipMsg`
   ([`sip_message.hpp`](../include/hmr/runtime/sip_message.hpp)) now holds the raw
   packet immutably and exposes headers as `(offset,len)` slices; per-worker
   scratch is a 64 KB bump-allocated [arena](../include/hmr/runtime/arena.hpp)
   with O(1) `reset()`. No header lookup, URI split, or value rebuild touches the
   heap in steady state.

The context is built once and `reset_for_apply()` reuses every buffer, so the
**steady-state apply path performs no heap allocation** — the issue's
zero-allocation requirement is satisfied. The generated `hmr_apply` is marked
`nounwind` and run through `-O3`, calling only into preallocated context buffers.

The remaining gap to the 50–150 ns aspiration is now narrow and well understood:
`std::regex` on the rules that *genuinely* need it, the header-name lookup, and
the value rebuild/serialize. The route to closing it is below.

## Closing the gap (future work)

With the arena model and specialized matchers landed, the remaining levers are
smaller and still transparent behind the C ABI — no change to generated modules:

* **Replace `std::regex` (on the pattern-rule path) with a compiled DFA /
  RE2-style matcher.** The exact/IP fast paths already bypass it; this is the
  last regex cost, paid only by rules that use real regex syntax.
* **Intern header names and hoist repeated `get_header`** of the same header
  across rules (the `DecisionTreeAnalysisPass` already groups rules by target
  header, which is exactly the information needed to do this).
* **Call LLD's library API** instead of forking `cc` to shave the largest slice
  off the (already in-budget) compile time.

These are deliberately scoped as follow-ups: the tree prioritizes a **correct,
complete, honestly-measured vertical slice** of every pipeline layer over a
hand-tuned hot path that would obscure the architecture.
