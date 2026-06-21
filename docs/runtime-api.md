# Runtime API & C ABI

A compiled ruleset is a native shared object whose only contract with the host
is the C ABI declared in
[`include/hmr/runtime/hmr_runtime.h`](../include/hmr/runtime/hmr_runtime.h).
The header is pure C, versioned by `HMR_ABI_VERSION` (currently `1`), and is the
*single* file shared between generated code and the host. The generated `.so`
references the `hmr_rt_*` callbacks as **undefined symbols** and the host
resolves them at `dlopen` time — so the host owns the SIP model and the module
carries none of it.

## What a module exports

Every generated `.so` exports exactly two symbols:

| Symbol | Type | Meaning |
|---|---|---|
| `hmr_apply` | `int (*)(HmrSipMsg*, HmrContext*)` | apply the ruleset to one message |
| `hmr_module_info` | `const HmrModuleInfo` | static descriptor (ABI version, slots, regex table) |

```c
typedef struct HmrModuleInfo {
    uint32_t             abi_version;   /* must equal HMR_ABI_VERSION */
    const char*          name;          /* ruleset name */
    uint32_t             num_slots;     /* store/load slots to allocate */
    uint32_t             num_regexes;   /* length of the regex table */
    const HmrRegexEntry* regexes;       /* {pattern, flags} table (bit0 = icase) */
} HmrModuleInfo;
```

`hmr_apply` returns an `HmrVerdict`: `HMR_OK` (0, forward the possibly-mutated
message), `HMR_REJECTED` (1, drop — see `hmr_rt_reject`), or `HMR_ERROR` (2).

## The two opaque handles

```c
typedef struct HmrSipMsg  HmrSipMsg;   /* the SIP message being manipulated  */
typedef struct HmrContext HmrContext;  /* per-apply scratch + built-in state */
```

Their layout lives in the C++ runtime
([`sip_message.hpp`](../include/hmr/runtime/sip_message.hpp),
[`context.hpp`](../include/hmr/runtime/context.hpp)); generated code only passes
them through. **One `HmrContext` is bound to one worker thread** — all callbacks
are reentrant across distinct contexts, so an N-thread SBC uses N contexts with
zero shared mutable state.

`HmrStr` is a **non-owning** `(pointer, length)` view. Strings the runtime
returns are valid only until the next mutating call on the same object; the
module must never free or retain one across calls.

```c
typedef struct HmrStr { const char* data; uint32_t len; } HmrStr;
```

## Callback surface

All callbacks operate on caller-owned, preallocated storage — **a module never
allocates** in steady state.

### Header access
```c
HmrStr   hmr_rt_get_header(HmrSipMsg*, HmrStr name);    /* empty if absent */
int      hmr_rt_set_header(HmrSipMsg*, HmrStr name, HmrStr value);
int      hmr_rt_add_header(HmrSipMsg*, HmrStr name, HmrStr value);
int      hmr_rt_delete_header(HmrSipMsg*, HmrStr name);
HmrStr   hmr_rt_get_method(const HmrSipMsg*);
int      hmr_rt_is_request(const HmrSipMsg*);
uint32_t hmr_rt_status_code(const HmrSipMsg*);
```
Header names are matched case-insensitively (RFC 3261).

### Matching
```c
int hmr_rt_match(HmrContext*, uint32_t match_type, HmrStr subject,
                 HmrStr pattern, uint32_t regex_id);
int hmr_rt_str_eq(HmrStr a, HmrStr b, int case_insensitive);
int hmr_rt_regex_match(HmrContext*, uint32_t regex_id, HmrStr subject);
```
`hmr_rt_match` is the unified match-val-type dispatch the generator emits for
every `match-value`. `match_type` is an `HmrMatchType` selecting the engine:

| `HmrMatchType` | Engine | Uses |
|---|---|---|
| `HMR_MATCH_EXACT` / `_CI` | byte compare (ASCII case-folded for `_CI`) | `pattern` |
| `HMR_MATCH_REGEX` | precompiled `std::regex`, records captures | `regex_id` |
| `HMR_MATCH_IP` | canonical IP equality (v4 & v6) | `pattern` |
| `HMR_MATCH_IP_MASK` | CIDR / dotted-netmask subnet membership | `pattern` |
| `HMR_MATCH_IP_RANGE` | inclusive low–high range | `pattern` |
| `HMR_MATCH_FQDN` | case-insensitive domain compare | `pattern` |

Only `HMR_MATCH_REGEX` consults the precompiled regex table (and records capture
groups for later `$N` back-references); the other engines compare `pattern`
directly and allocate nothing. `hmr_rt_str_eq` and `hmr_rt_regex_match` remain
the lower-level primitives `hmr_rt_match` is built on. See
[How match-values are lowered](#how-match-values-are-lowered).

### Captures, variables, slots
```c
HmrStr hmr_rt_get_capture(const HmrContext*, uint32_t index);  /* $0..$N */
HmrStr hmr_rt_get_var(const HmrContext*, uint32_t var_id);     /* $LOCAL_IP… */
void   hmr_rt_store(HmrContext*, uint32_t slot, HmrStr value);
HmrStr hmr_rt_load(const HmrContext*, uint32_t slot);
```
Built-in variables use stable numeric IDs (`HmrVarId`: `HMR_VAR_LOCAL_IP`,
`HMR_VAR_REMOTE_IP`, …, `HMR_VAR_FROM_HOST`), so generated code references a
variable with an immediate operand instead of a string lookup. Unknown
variables resolve to the empty string.

### Value builder (GoF Builder)
```c
void   hmr_rt_val_reset(HmrContext*);
void   hmr_rt_val_append_lit(HmrContext*, HmrStr literal);
void   hmr_rt_val_append_var(HmrContext*, uint32_t var_id);
void   hmr_rt_val_append_capture(HmrContext*, uint32_t index);
void   hmr_rt_val_append_slot(HmrContext*, uint32_t slot);
HmrStr hmr_rt_val_finish(HmrContext*);
```
A `new-value` like `sip:$TRUNK_GROUP@$LOCAL_IP` lowers to `val_reset`,
`val_append_lit("sip:")`, `val_append_var(TRUNK_GROUP)`,
`val_append_lit("@")`, `val_append_var(LOCAL_IP)`, `val_finish`. The builder
writes into a reusable scratch buffer owned by the context, so repeated
applications do not allocate.

### URI elements
```c
HmrStr hmr_rt_uri_get(HmrContext*, HmrStr header_value, uint32_t element_type);
HmrStr hmr_rt_uri_set(HmrContext*, HmrStr header_value, uint32_t element_type,
                      HmrStr new_value);
```
`element_type` is an `HmrUriElement`: `HMR_URI_WHOLE`/`DISPLAY`/`USER`/`HOST`/
`PORT`. `uri_set` rewrites one component and returns the rebuilt header value.
Its output uses a buffer (`uriScratch`) kept **separate** from the value-builder
scratch, so a `$`-interpolated `new-value` can be passed straight in without the
rebuild clobbering its own input.

### Diagnostics / control
```c
void hmr_rt_log(HmrContext*, HmrStr message);
void hmr_rt_reject(HmrContext*, uint32_t status_code, HmrStr reason);
```

## Host responsibilities (C++ runtime)

The host builds an `HmrContext` from a loaded module's descriptor and binds the
built-in variables it knows before each apply:

```cpp
#include "hmr/runtime/context.hpp"

auto ctx = hmr::runtime::make_context(mod.info()); // compiles regexes, sizes slots
ctx.set_var(HMR_VAR_LOCAL_IP,    "203.0.113.5");
ctx.set_var(HMR_VAR_TRUNK_GROUP, "tg-42");
ctx.set_var(HMR_VAR_REALM,       "core.example.net");

for (each packet) {
    ctx.reset_for_apply();            // clears captures/scratch/verdict, keeps capacity
    int verdict = mod.apply(&msg, &ctx);
}
```

`make_context` compiles the module's regex table once (throws `std::regex_error`
on a bad pattern) and sizes the store/load slots. `reset_for_apply()` clears
per-packet state while retaining buffer capacity — the source of the
zero-allocation steady state. See [examples/host_integration](../examples/host_integration)
for a complete, buildable host.

## How match-values are lowered

Oracle HMR supports several kinds of value comparison, and routing all of them
through `std::regex` — as the first draft did — is both slow and semantically
wrong (an exact comparison must be a byte compare; an address comparison must
understand IP arithmetic). The IR generator therefore picks the matcher per rule
at codegen time and emits a single `hmr_rt_match` with the chosen `HmrMatchType`:

* **`pattern-rule`** comparison-type → `HMR_MATCH_REGEX`: an `add_regex` table
  entry (case-insensitive flag set for the `*-insensitive` comparisons) plus the
  capture-recording regex engine. Literals that genuinely need regex semantics
  (anchors, character classes) still take this path.
* **`ip`** match-val-type → `HMR_MATCH_IP` / `HMR_MATCH_IP_MASK` /
  `HMR_MATCH_IP_RANGE`, chosen from the pattern's shape (a bare address,
  `addr/prefix`, or `lo-hi`).
* **`fqdn`** match-val-type → `HMR_MATCH_FQDN`.
* **everything else** → `HMR_MATCH_EXACT` / `HMR_MATCH_EXACT_CI`, a plain byte
  compare folded per the comparison-type.

The specialized matchers live in
[`matchers.hpp`](../include/hmr/runtime/matchers.hpp) and are unit-tested in
isolation ([`tests/test_matchers.cpp`](../tests/test_matchers.cpp)); each is
pure, `noexcept`, and never allocates — malformed input simply does not match.

## ABI stability

`HMR_ABI_VERSION` is bumped on any incompatible change to the structs or
callback signatures. `ModuleManager::load` refuses a module whose
`hmr_module_info.abi_version` does not equal the host's `HMR_ABI_VERSION`, so a
stale `.so` fails fast at load instead of corrupting state at apply.
