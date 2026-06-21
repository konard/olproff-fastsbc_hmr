/* SPDX-License-Identifier: MIT
 *
 * hmr_runtime.h — stable C ABI between the host SBC and a generated HMR module.
 *
 * A compiled ruleset is a native shared object that exports a single entry
 * point, `hmr_apply`, plus a static descriptor, `hmr_module_info`. The module
 * never allocates: it reads the SIP message through zero-copy views and writes
 * modified values into a per-worker bump arena, all via the `hmr_rt_*` callbacks
 * declared here. This header is the *only* contract shared between the
 * compiler-generated code and the runtime, so it is intentionally pure C and
 * ABI-stable (versioned via HMR_ABI_VERSION).
 *
 * Memory model (review point #4 — zero-copy arena):
 *   * The input packet is immutable. Header names/values and URI components are
 *     slices into the original raw buffer; reading them copies nothing.
 *   * Mutations (set/add header, set field, value-builder results) are
 *     bump-allocated from a 64 KiB arena owned by the HmrContext (one per worker
 *     thread). The arena is reset in O(1) between packets — no per-packet heap
 *     traffic, no frees.
 *   * HmrStr is a non-owning (pointer,len) view into either the raw packet or
 *     the arena. It is valid until the arena is reset (end of packet). The
 *     generated module must not free or retain an HmrStr across packets.
 *   * All callbacks are reentrant across distinct HmrContext objects, enabling
 *     one context (and arena) per worker thread with zero shared mutable state.
 */

#ifndef HMR_RUNTIME_H
#define HMR_RUNTIME_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Bump on any incompatible change to the structs or callback signatures.
 * v2: zero-copy arena SIP model + match dispatch + field accessors. */
#define HMR_ABI_VERSION 2u

/* Maximum number of headers tracked per message (fixed, no heap). */
#define HMR_MAX_HEADERS 32u

/* Non-owning string view exchanged across the ABI. */
typedef struct HmrStr {
    const char* data;
    uint32_t    len;
} HmrStr;

/* Opaque handles. Their layout lives in the C++ runtime (sip_message.hpp /
 * context.hpp); the generated module only ever passes them through. */
typedef struct HmrSipMsg  HmrSipMsg;
typedef struct HmrContext HmrContext;

/* ---- Built-in HMR variables ($LOCAL_IP, $REMOTE_IP, ...) -----------------
 * Stable numeric IDs so the generated code can reference a variable with an
 * immediate operand instead of a string lookup. The host populates the values
 * it knows; unknown variables resolve to the empty string. */
typedef enum HmrVarId {
    HMR_VAR_NONE          = 0,
    HMR_VAR_LOCAL_IP      = 1,
    HMR_VAR_REMOTE_IP     = 2,
    HMR_VAR_LOCAL_PORT    = 3,
    HMR_VAR_REMOTE_PORT   = 4,
    HMR_VAR_TRUNK_GROUP   = 5,
    HMR_VAR_REALM         = 6,
    HMR_VAR_INTERFACE     = 7,
    HMR_VAR_METHOD        = 8,
    HMR_VAR_RURI_USER     = 9,
    HMR_VAR_RURI_HOST     = 10,
    HMR_VAR_TO_USER       = 11,
    HMR_VAR_TO_HOST       = 12,
    HMR_VAR_FROM_USER     = 13,
    HMR_VAR_FROM_HOST     = 14,
    HMR_VAR_MAX
} HmrVarId;

/* Pre-extracted URI component fields (zero-copy slices into the raw packet,
 * overridable via the arena). These are the hot elements HMR rules rewrite, so
 * codegen can address them directly instead of re-parsing a header each time. */
typedef enum HmrField {
    HMR_FIELD_REQUEST_URI_USER = 0,
    HMR_FIELD_REQUEST_URI_HOST = 1,
    HMR_FIELD_FROM_USER        = 2,
    HMR_FIELD_FROM_HOST        = 3,
    HMR_FIELD_TO_USER          = 4,
    HMR_FIELD_TO_HOST          = 5,
    HMR_FIELD_MAX
} HmrField;

/* URI element selectors for hmr_rt_uri_get / hmr_rt_uri_set. Codegen maps the
 * AST ElementType onto these stable codes. */
typedef enum HmrUriElement {
    HMR_URI_WHOLE   = 0,   /* the entire header value (passthrough) */
    HMR_URI_DISPLAY = 1,   /* display name before the addr-spec */
    HMR_URI_USER    = 2,   /* user part of sip:user@host */
    HMR_URI_HOST    = 3,   /* host part */
    HMR_URI_PORT    = 4    /* port part */
} HmrUriElement;

/* match-val-type dispatch codes. Codegen selects the matcher with an immediate
 * operand; the runtime routes to the specialized implementation (review #2). */
typedef enum HmrMatchType {
    HMR_MATCH_EXACT    = 0,   /* byte compare (strcmp) */
    HMR_MATCH_EXACT_CI = 1,   /* ASCII case-insensitive compare */
    HMR_MATCH_REGEX    = 2,   /* precompiled regex; uses regex_id, records caps */
    HMR_MATCH_IP       = 3,   /* canonical IP equality (v4 & v6) */
    HMR_MATCH_IP_MASK  = 4,   /* CIDR / dotted-netmask subnet membership */
    HMR_MATCH_IP_RANGE = 5,   /* inclusive low-high range */
    HMR_MATCH_FQDN     = 6    /* case-insensitive domain compare */
} HmrMatchType;

/* Result of hmr_apply: how the host should proceed with the message. */
typedef enum HmrVerdict {
    HMR_OK       = 0,   /* forward the (possibly mutated) message */
    HMR_REJECTED = 1,   /* drop/reject; see hmr_rt_rejected() */
    HMR_ERROR    = 2    /* internal error during application */
} HmrVerdict;

/* ---- Runtime callbacks invoked by generated code ------------------------- */

/* Header access. Names are case-insensitive per RFC 3261. get returns an empty
 * HmrStr (data may be NULL, len == 0) when the header is absent. set/add write
 * the value into the context arena, so `ctx` carries the storage. */
HmrStr hmr_rt_get_header(const HmrSipMsg* msg, HmrStr name);
int    hmr_rt_set_header(HmrSipMsg* msg, HmrContext* ctx, HmrStr name, HmrStr value);
int    hmr_rt_add_header(HmrSipMsg* msg, HmrContext* ctx, HmrStr name, HmrStr value);
int    hmr_rt_delete_header(HmrSipMsg* msg, HmrStr name);

/* Request-line / status accessors used by element rules. */
HmrStr hmr_rt_get_method(const HmrSipMsg* msg);
int    hmr_rt_is_request(const HmrSipMsg* msg);
uint32_t hmr_rt_status_code(const HmrSipMsg* msg);

/* Pre-extracted URI component fields (HmrField). get returns the arena override
 * if set, else the original slice; set writes `value` into the arena. */
HmrStr hmr_rt_get_field(const HmrSipMsg* msg, uint32_t field);
int    hmr_rt_set_field(HmrSipMsg* msg, HmrContext* ctx, uint32_t field, HmrStr value);

/* Comparison primitives (return 1 on match, 0 otherwise). */
int hmr_rt_str_eq(HmrStr a, HmrStr b, int case_insensitive);
int hmr_rt_regex_match(HmrContext* ctx, uint32_t regex_id, HmrStr subject);

/* Unified match-val-type dispatch. For HMR_MATCH_REGEX, `regex_id` selects the
 * precompiled pattern (captures recorded into ctx) and `pattern` is ignored;
 * for every other type `pattern` is the literal match-value and `regex_id` is
 * ignored. Routes to the specialized matcher (matchers.hpp). */
int hmr_rt_match(HmrContext* ctx, uint32_t match_type, HmrStr subject,
                 HmrStr pattern, uint32_t regex_id);

/* Capture / variable / stored-slot access. */
HmrStr hmr_rt_get_capture(const HmrContext* ctx, uint32_t index);
HmrStr hmr_rt_get_var(const HmrContext* ctx, uint32_t var_id);
void   hmr_rt_store(HmrContext* ctx, uint32_t slot, HmrStr value);
HmrStr hmr_rt_load(const HmrContext* ctx, uint32_t slot);

/* Value builder for new-value expressions. Builds into a reusable scratch
 * buffer owned by the context, so repeated applications do not allocate. */
void   hmr_rt_val_reset(HmrContext* ctx);
void   hmr_rt_val_append_lit(HmrContext* ctx, HmrStr literal);
void   hmr_rt_val_append_var(HmrContext* ctx, uint32_t var_id);
void   hmr_rt_val_append_capture(HmrContext* ctx, uint32_t index);
void   hmr_rt_val_append_slot(HmrContext* ctx, uint32_t slot);
HmrStr hmr_rt_val_finish(HmrContext* ctx);

/* URI element access on a header value (e.g. From/To/Request-URI). Returns the
 * requested component; replace rewrites it (into the arena) and returns the
 * rebuilt header. */
HmrStr hmr_rt_uri_get(HmrContext* ctx, HmrStr header_value, uint32_t element_type);
HmrStr hmr_rt_uri_set(HmrContext* ctx, HmrStr header_value, uint32_t element_type,
                      HmrStr new_value);

/* Diagnostics / control. */
void hmr_rt_log(HmrContext* ctx, HmrStr message);
void hmr_rt_reject(HmrContext* ctx, uint32_t status_code, HmrStr reason);

/* ---- Module descriptor exported by every generated .so ------------------- */

/* One precompiled regex the runtime must build at load time. */
typedef struct HmrRegexEntry {
    const char* pattern;     /* NUL-terminated regex source */
    uint32_t    flags;       /* bit0: case-insensitive */
} HmrRegexEntry;

typedef struct HmrModuleInfo {
    uint32_t             abi_version;   /* must equal HMR_ABI_VERSION */
    const char*          name;          /* ruleset name (NUL-terminated) */
    uint32_t             num_slots;     /* store/load slot count to allocate */
    uint32_t             num_regexes;   /* length of the regex table */
    const HmrRegexEntry* regexes;       /* regex table (may be NULL if 0) */
} HmrModuleInfo;

/* The single entry point every module exports. */
typedef int (*hmr_apply_fn)(HmrSipMsg* msg, HmrContext* ctx);

#ifdef __cplusplus
}  /* extern "C" */
#endif

#endif /* HMR_RUNTIME_H */
