# User guide

This guide covers building the project, compiling an HMR ruleset to a native
module with the `hmrc` driver, and loading that module from a host.

## Prerequisites

| Tool | Version | Needed for |
|---|---|---|
| C++ compiler | g++ 13+ / clang 17+ (C++23) | everything |
| [Meson](https://mesonbuild.com) | ≥ 1.1 | build system |
| Ninja | any | build backend |
| ANTLR4 | 4.13.x tool jar + C++ runtime | the generated lexer/parser (fetched by `scripts/setup_antlr.sh`) |
| JRE | any | running the ANTLR4 tool jar |
| LLVM | 17 or 18 (`llvm-dev`) | the code generator / `compile` |
| `lld` | matching LLVM | optional faster linking |
| `cc` (gcc/clang driver) | any | linking the `.so` (`-shared`) |

The front-end (lexer/parser/optimizer/runtime and the front-end `hmrc`
subcommands) builds with **no LLVM at all**. LLVM is only required for the
`compile` subcommand and the codegen/backend/module layers.

## Building

ANTLR4 is not packaged on most distros, so a one-time helper fetches a pinned
tool jar + C++ runtime and prints the three paths Meson needs:

```sh
eval "$(scripts/setup_antlr.sh)"   # exports ANTLR_JAR / ANTLR_INC / ANTLR_LIBDIR
```

### Full build (with LLVM)
```sh
meson setup builddir \
  -Dantlr_jar="$ANTLR_JAR" -Dantlr_inc="$ANTLR_INC" -Dantlr_libdir="$ANTLR_LIBDIR"
meson compile -C builddir
meson test    -C builddir
```

The ANTLR4 C++ runtime is linked **statically** (`antlr_libdir` is searched for
`libantlr4-runtime.a`), so the built binaries carry no ANTLR shared-library
dependency and need no `LD_LIBRARY_PATH` at run time.

### Front-end-only build (no LLVM)
```sh
meson setup builddir-fe -Denable_llvm=false \
  -Dantlr_jar="$ANTLR_JAR" -Dantlr_inc="$ANTLR_INC" -Dantlr_libdir="$ANTLR_LIBDIR"
meson compile -C builddir-fe
meson test    -C builddir-fe
```

With `-Denable_llvm=false` (or when no `llvm-config` is found) the build
**degrades gracefully**: the four LLVM-gated libraries are skipped while the
front end, runtime, optimizer, interpreter, C++ generator, and the front-end
subcommands of `hmrc` still build and pass their tests.

### Build options

| Option | Default | Effect |
|---|---|---|
| `enable_llvm` | `true` | build the LLVM-backed codegen/backend/module/pipeline |
| `build_tests` | `true` | build and register the test suite |
| `build_benchmarks` | `true` | build the micro-benchmarks |
| `build_examples` | `true` | build the host-integration example |
| `antlr_jar` / `antlr_inc` / `antlr_libdir` | — | paths from `scripts/setup_antlr.sh` |

Reconfigure an existing build dir with `meson configure builddir -Denable_llvm=false`.

## Using `hmrc`

The driver is a Facade over the pipeline. Front-end subcommands work in any
build; `compile` requires an LLVM build.

```
hmrc parse         <file>            # lex + parse, report diagnostics/warnings
hmrc dump-ast      <file>            # parse and pretty-print the AST back to HMR
hmrc optimize      <file>            # run the optimizer, print its report + AST
hmrc check-samples <dir>            # every *.hmr must parse; invalid_* must fail
hmrc dump-ir       <file> [--opt]    # emit LLVM IR, before / after -O3 (LLVM only)
hmrc compile       <file> [-o out.so]   # full pipeline → native module (LLVM only)
```

Examples:
```sh
# Inspect what the parser/optimizer make of a ruleset:
./builddir/hmrc optimize tests/hmr_samples/topology_hiding.hmr

# See the generated LLVM IR, then the same IR after the -O3 pipeline:
./builddir/hmrc dump-ir tests/hmr_samples/minimal_ruleset.hmr
./builddir/hmrc dump-ir tests/hmr_samples/minimal_ruleset.hmr --opt

# Compile a ruleset to a loadable module:
./builddir/hmrc compile tests/hmr_samples/topology_hiding.hmr -o topo.so
```

The before/after IR is walked through in
[llvm-ir-examples.md](./llvm-ir-examples.md).

`check-samples` is what CI uses to gate the sample corpus: files named
`invalid_*.hmr` are expected to be rejected, all others to parse.

## Writing HMR

The DSL is **keyword-delimited** — block structure comes from the Oracle HMR
keywords, not from indentation (whitespace is purely cosmetic, matching how real
Oracle ACLI dumps are laid out). A minimal ruleset:

```hmr
sip-manipulation TopologyHiding
        header-rule
                name           hideServer
                header-name    Server
                action         delete-header
                msg-type       any
```

* Block keywords (`sip-manipulation`, `header-rule`, `element-rule`) and the
  attribute keywords (`name` / `header-name`) start a block; `name` is a child
  attribute (an optional inline name after the keyword is also accepted).
* A `match-value` is lowered to the matcher its comparison-type / match-val-type
  calls for — an exact byte compare, an IP matcher, or a regex only for
  `pattern-rule` (see
  [how match-values are lowered](./runtime-api.md#how-match-values-are-lowered)).
* `new-value` supports literals, `$VAR` built-ins, `$N` capture back-references,
  and `+` concatenation.

The authoritative surface-syntax/keyword reference is
[oracle-hmr-reference.md](./oracle-hmr-reference.md); the formal grammar is
[`grammar/Hmr.g4`](../grammar/Hmr.g4); worked rulesets live in
[`tests/hmr_samples/`](../tests/hmr_samples): a minimal ruleset, a broad
`full_example`, focused samples for regex captures, IP matching, multiple
headers, nested elements and every built-in variable, three realistic rulesets
(topology hiding, From anonymization, PAI normalization), and `invalid_*.hmr`
negatives. The feature support table is in
[compatibility-matrix.md](./compatibility-matrix.md).

## Loading a module from a host

A compiled module resolves its `hmr_rt_*` callbacks against the **host**
process, so the host must export those symbols (link the host with
`-rdynamic`/`export_dynamic`, pulling in the `hmr_runtime` library). The host
then loads the module through the `ModuleManager`:

```cpp
#include "hmr/module/module_manager.hpp"
#include "hmr/runtime/context.hpp"

hmr::module::ModuleManager mgr;
auto loaded = mgr.load("topo.so");          // dlopen + ABI check
auto& mod   = **loaded;

auto ctx = hmr::runtime::make_context(mod.info());
ctx.set_var(HMR_VAR_LOCAL_IP, "203.0.113.5");

ctx.reset_for_apply();
int verdict = mod.apply(&msg, &ctx);        // HMR_OK / HMR_REJECTED / HMR_ERROR
```

A complete, buildable host (wired into the Meson build) is in
[`examples/host_integration/`](../examples/host_integration). The full callback
contract is documented in [runtime-api.md](./runtime-api.md).

## Hot module replacement

`ModuleManager` publishes the active module through an atomic
`shared_ptr<const LoadedModule>`. Calling `load()` again **hot-swaps** the
module: in-flight packets keep running against their `shared_ptr` snapshot while
new packets pick up the replacement; the old `.so` is `dlclose`d once its last
reader drops it (RCU-style, no reader lock on the apply path). Subscribe to
(re)load events via `ModuleManager::subscribe` (GoF Observer).

## Performance expectations

See [performance.md](./performance.md) for measured numbers and an honest
account of which issue targets are met (compile time, module size, faster than
both the interpreter and the GCC approach) and which remain aspirational
(per-packet latency, now ~3× better after the arena + specialized-matcher rework
but still above the ns-scale target).
