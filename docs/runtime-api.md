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
([`SipMessage.hpp`](../include/hmr/runtime/SipMessage.hpp),
[`Runtime.hpp`](../include/hmr/runtime/Runtime.hpp)); generated code only passes
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
int hmr_rt_str_eq(HmrStr a, HmrStr b, int case_insensitive);
int hmr_rt_regex_match(HmrContext*, uint32_t regex_id, HmrStr subject);
```
`hmr_rt_regex_match` evaluates the precompiled regex at table index `regex_id`
and, on a match, records the capture groups into the context for later `$N`
back-references. **Every** HMR `match-value` lowers to this call — see
[Why match-values are always regexes](#why-match-values-are-always-regexes).

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
#include "hmr/runtime/Runtime.hpp"

auto ctx = hmr::runtime::makeContext(mod.info()); // compiles regexes, sizes slots
ctx.setVar(HMR_VAR_LOCAL_IP,    "203.0.113.5");
ctx.setVar(HMR_VAR_TRUNK_GROUP, "tg-42");
ctx.setVar(HMR_VAR_REALM,       "core.example.net");

for (each packet) {
    ctx.resetForApply();              // clears captures/scratch/verdict, keeps capacity
    int verdict = mod.apply(&msg, &ctx);
}
```

`makeContext` compiles the module's regex table once (throws `std::regex_error`
on a bad pattern) and sizes the store/load slots. `resetForApply()` clears
per-packet state while retaining buffer capacity — the source of the
zero-allocation steady state. See [examples/host_integration](../examples/host_integration)
for a complete, buildable host.

## Why match-values are always regexes

In Oracle HMR a `match-value` is a **regular expression for every
comparison-type**, not only `pattern-rule`. The IR generator therefore lowers
*every* literal `match-value` to an `addRegex` table entry + a
`hmr_rt_regex_match` guard (with the case-insensitive flag set for
`case-insensitive` comparisons), rather than a `str_eq`. This is the single most
important compatibility decision in the code generator; matching Oracle's
semantics here is what lets real-world rulesets (which rely on anchors and
character classes inside ostensibly "literal" match-values) compile correctly.

## ABI stability

`HMR_ABI_VERSION` is bumped on any incompatible change to the structs or
callback signatures. `ModuleManager::load` refuses a module whose
`hmr_module_info.abi_version` does not equal the host's `HMR_ABI_VERSION`, so a
stale `.so` fails fast at load instead of corrupting state at apply.
