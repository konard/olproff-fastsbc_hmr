# Documentation

* [architecture.md](./architecture.md) — the layered design, the compilation
  pipeline, the optimizer pass set, and where each GoF/SOLID pattern lives.
* [user-guide.md](./user-guide.md) — building (with and without LLVM), the
  `hmrc` driver, writing HMR, and loading a compiled module from a host.
* [runtime-api.md](./runtime-api.md) — the stable C ABI: what a module exports,
  the `hmr_rt_*` callback surface, and the host's responsibilities.
* [compatibility-matrix.md](./compatibility-matrix.md) — honest per-construct
  support: ✅ lowered, 🟡 parsed-but-approximated, ⬜ recognized-only.
* [performance.md](./performance.md) — measured benchmark numbers and a candid
  account of which targets are met and which are future work.
* [llvm-ir-examples.md](./llvm-ir-examples.md) — the generated LLVM IR before and
  after optimization, the custom `HmrAttributePass`, and `hmrc dump-ir`.
* [oracle-hmr-reference.md](./oracle-hmr-reference.md) — the DSL reference
  compiled from Oracle SBC documentation (the authority for keywords/grammar).
