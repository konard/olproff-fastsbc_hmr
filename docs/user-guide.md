# User guide

This guide covers building the project, compiling an HMR ruleset to a native
module with the `hmrc` driver, and loading that module from a host.

## Prerequisites

| Tool | Version | Needed for |
|---|---|---|
| C++ compiler | g++ 13+ / clang 17+ (C++23) | everything |
| CMake | ≥ 3.24 | build system |
| Ninja or Make | any | build |
| LLVM | 17 or 18 (`llvm-dev`) | the code generator / `compile` |
| `lld` | matching LLVM | optional faster linking |
| `cc` (gcc/clang driver) | any | linking the `.so` (`-shared`) |

The front-end (lexer/parser/optimizer/runtime and the front-end `hmrc`
subcommands) builds with **no LLVM at all**. LLVM is only required for the
`compile` subcommand and the codegen/backend/module layers.

## Building

### Full build (with LLVM)
```sh
cmake -S . -B build -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DLLVM_DIR="$(llvm-config-18 --cmakedir)" \
  -DHMR_ENABLE_LLVM=ON
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

### Front-end-only build (no LLVM)
```sh
cmake -S . -B build -G Ninja -DHMR_ENABLE_LLVM=OFF
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

If `-DHMR_ENABLE_LLVM=ON` is set but `find_package(LLVM)` fails, the build
prints a warning and **degrades gracefully** to the front-end-only configuration
rather than failing.

### Build options

| Option | Default | Effect |
|---|---|---|
| `HMR_ENABLE_LLVM` | `ON` | build the LLVM-backed codegen/backend/module/pipeline |
| `HMR_BUILD_TESTS` | `ON` | build and register the test suite |
| `HMR_BUILD_BENCHMARKS` | `ON` | build the micro-benchmarks |
| `HMR_WARNINGS_AS_ERRORS` | `OFF` | add `-Werror` (CI turns this on) |

## Using `hmrc`

The driver is a Facade over the pipeline. Front-end subcommands work in any
build; `compile` requires an LLVM build.

```
hmrc parse         <file>            # lex + parse, report diagnostics/warnings
hmrc dump-ast      <file>            # parse and pretty-print the AST back to HMR
hmrc optimize      <file>            # run the optimizer, print its report + AST
hmrc check-samples <dir>            # every *.hmr must parse; invalid_* must fail
hmrc compile       <file> [-o out.so]   # full pipeline → native module (LLVM only)
```

Examples:
```sh
# Inspect what the parser/optimizer make of a ruleset:
./build/hmrc optimize tests/hmr_samples/topology_hiding.hmr

# Compile a ruleset to a loadable module:
./build/hmrc compile tests/hmr_samples/topology_hiding.hmr -o topo.so
```

`check-samples` is what CI uses to gate the sample corpus: files named
`invalid_*.hmr` are expected to be rejected, all others to parse.

## Writing HMR

The DSL is indentation-structured (Python-style). A minimal ruleset:

```hmr
sip-manipulation TopologyHiding
        header-rule
                name           hideServer
                header-name    Server
                action         delete-header
                msg-type       any
```

* Block keywords (`sip-manipulation`, `header-rule`, `element-rule`) stand alone
  on their line; `name` is a child attribute (an optional inline name after the
  keyword is also accepted).
* A `match-value` is **always a regular expression** (see
  [runtime-api.md](./runtime-api.md#why-match-values-are-always-regexes)).
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
`ENABLE_EXPORTS` / `-rdynamic`, pulling in `hmr_core`'s `Runtime.cpp`). The host
then loads the module through the `ModuleManager`:

```cpp
#include "hmr/module/ModuleManager.hpp"
#include "hmr/runtime/Runtime.hpp"

hmr::module::ModuleManager mgr;
auto loaded = mgr.load("topo.so");          // dlopen + ABI check
auto& mod   = **loaded;

auto ctx = hmr::runtime::makeContext(mod.info());
ctx.setVar(HMR_VAR_LOCAL_IP, "203.0.113.5");

ctx.resetForApply();
int verdict = mod.apply(&msg, &ctx);        // HMR_OK / HMR_REJECTED / HMR_ERROR
```

A complete, buildable host (with its own `CMakeLists.txt`) is in
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
account of which issue targets are met (compile time, module size) and which are
aspirational with the current `std::string`/`std::regex` runtime model
(per-packet latency).
