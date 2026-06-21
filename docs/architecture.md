# Architecture

`fastsbc_hmr` is a compiler that lowers the Oracle SBC *Header Manipulation
Rules* (HMR) DSL **directly to LLVM IR** and emits a native, loadable `.so`
module. There is no intermediate C++ source and no Clang front-end on the
critical path: the AST is walked by an `IRBuilder`-driven visitor that produces
IR text, which the LLVM backend optimizes (`-O3`) and lowers to a relocatable
object, which a linker driver turns into a shared object the runtime `dlopen`s.

```
 HMR text
    │  Lexer (indentation-aware)            src/parser/Lexer.cpp
    ▼
 Token stream
    │  Parser (recursive descent)           src/parser/Parser.cpp
    ▼
 AST (Ruleset)                              include/hmr/ast/Ast.hpp
    │  Optimizer (CRTP pass chain)          src/optimizer/Optimizer.cpp
    ▼
 Optimized AST + DecisionPlan
    │  IrGenerator (Visitor + IRBuilder)    src/codegen/IrGenerator.cpp
    ▼
 LLVM IR (textual std::string)
    │  Backend (custom pass + O3 + emit)    src/backend/Backend.cpp
    ▼
 Relocatable object (.o, in memory)
    │  Linker (cc -shared driver)           src/backend/Linker.cpp
    ▼
 Shared object (.so)
    │  ModuleManager (dlopen, RCU reload)   src/module/ModuleManager.cpp
    ▼
 Loaded module → hmr_apply(SipMsg*, Context*)
```

The whole chain is hidden behind a single Facade,
[`hmr::pipeline::Compiler`](../include/hmr/pipeline/Compiler.hpp).

## Layering

The build is split into two static libraries so the LLVM dependency is
contained and the inner development loop stays fast:

| Library | Sources | Depends on |
|---|---|---|
| `hmr_core` | parser, AST, optimizer, runtime model | nothing heavy (STL only) |
| `hmr_codegen` | IR generator, backend, linker, module manager, pipeline Facade | `hmr_core` + LLVM + `dl` |

`hmr_core` has **no LLVM dependency** and is fully unit-testable on its own.
When LLVM is absent (`-DHMR_ENABLE_LLVM=OFF`, or `find_package(LLVM)` fails) the
build degrades gracefully to the front-end only: the lexer, parser, optimizer,
runtime model, and the front-end subcommands of `hmrc` still build and pass
their tests. The CI `frontend-only` job exercises exactly this path.

The seam between `hmr_codegen`'s code generator and its backend is deliberately
**textual LLVM IR** (a `std::string`). The IR generator never touches an LLVM
`Module` object that the backend also holds; it hands over IR text and the
backend re-parses it. This keeps the generator independently testable (snapshot
the IR string) and the boundary trivially serializable.

## Stages

### Lexer — `src/parser/Lexer.cpp`
HMR is an off-side-rule language: block structure comes from leading
whitespace, exactly like Python. The lexer is a hand-written denter that emits
synthetic `INDENT` / `DEDENT` tokens around a column stack (tab stop = 8,
`Lexer::kTabWidth`). Blank and comment-only lines never affect indentation; a
`#` starts a comment only at a token boundary. All keywords are *contextual* —
every bareword is a `WORD` token and the parser decides meaning by position, so
a rule may legally be named `add` or use `delete` as a value.

### Parser — `src/parser/Parser.cpp`
A recursive-descent parser that mirrors, rule-for-rule, the reference grammar in
[`grammar/Hmr.g4`](../grammar/Hmr.g4). It builds the `Ruleset` AST via an
[AST factory](../include/hmr/ast/AstFactory.hpp) (GoF **Factory**), validates
enum spellings, and collects non-fatal **warnings** (unknown attributes are
warn-and-ignore, matching Oracle's lenient config loader) separately from fatal
diagnostics.

### Optimizer — `src/optimizer/Optimizer.cpp`
A chain of CRTP passes (`PassBase<Derived>`, GoF **Strategy** specialized at
compile time for zero-cost dispatch) run to a fixpoint, followed by one analysis
pass. See [the pass list](#optimizer-passes) below.

### IR generator — `src/codegen/IrGenerator.cpp`
An AST **Visitor** driving an LLVM `IRBuilder`. It emits one `hmr_apply`
function plus a `hmr_module_info` descriptor. Every comparison — even a literal
`match-value` — is lowered to a precompiled-regex guard (`hmr_rt_regex_match`),
because in Oracle HMR a `match-value` is *always* a regular expression. String
literals, `$VAR` interpolations, and `$N` captures in `new-value` are lowered to
a sequence of value-builder callbacks.

### Backend — `src/backend/Backend.cpp`
Parses the IR text, runs a custom `HmrAttributePass` (`PassInfoMixin`, marks
`hmr_apply` and the `hmr_rt_*` callbacks `nounwind`) ahead of the standard
`PassBuilder` `-O<n>` pipeline (new pass manager), then emits a PIC relocatable
object via the legacy codegen `PassManager` (`addPassesToEmitFile`). Targets are
initialized once under `std::call_once`. The same parse-and-optimize front half
is exposed as `Backend::optimizeIR()`, which stops before codegen and returns the
optimized IR as text — that powers `hmrc dump-ir --opt` and the worked
before/after walkthrough in [llvm-ir-examples.md](./llvm-ir-examples.md).

### Linker — `src/backend/Linker.cpp`
Writes the object to a temp file and drives `cc -shared` via `posix_spawnp` to
produce the `.so` (override with `HMR_CC` or `LinkOptions::driver`). Using the
system `cc` driver — rather than calling LLD's library API directly — is an
**honest, documented deviation** from the issue's "LLD" mention: the driver
handles crt objects and library paths for us, and you can route it through LLD
with `driver = "clang"` + `-fuse-ld=lld`.

### Module manager — `src/module/ModuleManager.cpp`
`dlopen`s the module, resolves `hmr_apply` + `hmr_module_info`, checks
`abi_version == HMR_ABI_VERSION`, and publishes it through an
`std::atomic<std::shared_ptr<const LoadedModule>>`. Hot replacement is **RCU
style**: a reload installs a new module with a release store while in-flight
packets keep running against the old `shared_ptr` snapshot; the old module is
`dlclose`d by its destructor once the last reader drops it. Subscribers
(GoF **Observer**) are notified of `Loaded` / `Reloaded` / `Unloaded`.

## Optimizer passes

| # | Pass | Kind | Effect |
|---|---|---|---|
| 1 | `DeduplicationPass` | transform | drop header-rules identical to an earlier one |
| 2 | `DeadCodeEliminationPass` | transform | drop rules that can never fire / do nothing |
| 3 | `PatternSimplificationPass` | transform | lower trivial regexes toward plain comparisons |
| 4 | `RuleMergingPass` | transform | fuse compatible adjacent rules / element-rules |
| 5 | `DecisionTreeAnalysisPass` | analysis | group rules by target header into a `DecisionPlan` |

Passes 1–4 mutate the AST and re-run until no pass reports a change (bounded by
`maxIterations`, default 4). Pass 5 leaves semantics untouched and produces the
`DecisionPlan` the generator uses to lay out the dispatch.

## Design patterns

The codebase is intentionally a small catalogue of the GoF/SOLID patterns the
issue calls for, used where they earn their keep:

* **Facade** — `pipeline::Compiler` is the single entry point over six
  subsystems.
* **Visitor** — `ast::AstVisitor` walks the AST for printing and IR generation.
* **Strategy (CRTP)** — `opt::PassBase<Derived>` gives zero-cost,
  statically-dispatched optimizer passes.
* **Factory** — `ast::AstFactory` centralizes node construction and validation.
* **Builder** — the runtime value-builder API (`hmr_rt_val_*`) assembles
  `new-value` strings incrementally.
* **Observer** — `module::ModuleManager` notifies subscribers on (re)load.

## Stable ABI boundary

Generated modules talk to the host only through the C ABI in
[`include/hmr/runtime/hmr_runtime.h`](../include/hmr/runtime/hmr_runtime.h)
(`HMR_ABI_VERSION = 1`). The `.so` references the `hmr_rt_*` callbacks as
**undefined symbols** resolved at `dlopen` against the host process — so the host
owns the SIP model and the module stays tiny (~15 KB) and toolchain-independent.
See [runtime-api.md](./runtime-api.md) for the full surface.
