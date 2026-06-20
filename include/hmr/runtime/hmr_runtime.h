/* SPDX-License-Identifier: MIT
 *
 * hmr_runtime.h — stable C ABI between the host SBC and a generated HMR module.
 *
 * A compiled ruleset is a native shared object that exports a single entry
 * point, `hmr_apply`, plus a static descriptor, `hmr_module_info`. The module
 * never allocates: it manipulates the SIP message and reads/writes scratch
 * state through the `hmr_rt_*` callbacks declared here, all of which operate on
 * caller-owned, preallocated storage. This header is the *only* contract shared
 * between the compiler-generated code and the runtime, so it is intentionally
 * pure C and ABI-stable (versioned via HMR_ABI_VERSION).
 *
 * Ownership / lifetime rules:
 *   * HmrStr is a non-owning (pointer,len) view. Strings returned by the
 *     runtime are valid until the next mutating call on the same object.
 *   * The generated module must not free or retain any HmrStr across calls.
 *   * All callbacks are reentrant with respect to distinct HmrContext objects,
 *     enabling one context per worker thread with zero shared mutable state.
 */

#ifndef HMR_RUNTIME_H
#define HMR_RUNTIME_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Bump on any incompatible change to the structs or callback signatures. */
#define HMR_ABI_VERSION 1u

/* Non-owning string view exchanged across the ABI. */
typedef struct HmrStr {
    const char* data;
    uint32_t    len;
} HmrStr;

/* Opaque handles. Their layout lives in the C++ runtime (SipMessage.hpp /
 * Runtime.hpp); the generated module only ever passes them through. */
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

/* URI element selectors for hmr_rt_uri_get / hmr_rt_uri_set. Codegen maps the
 * AST ElementType onto these stable codes. */
typedef enum HmrUriElement {
    HMR_URI_WHOLE   = 0,   /* the entire header value (passthrough) */
    HMR_URI_DISPLAY = 1,   /* display name before the addr-spec */
    HMR_URI_USER    = 2,   /* user part of sip:user@host */
    HMR_URI_HOST    = 3,   /* host part */
    HMR_URI_PORT    = 4    /* port part */
} HmrUriElement;

/* Result of hmr_apply: how the host should proceed with the message. */
typedef enum HmrVerdict {
    HMR_OK       = 0,   /* forward the (possibly mutated) message */
    HMR_REJECTED = 1,   /* drop/reject; see hmr_rt_rejected() */
    HMR_ERROR    = 2    /* internal error during application */
} HmrVerdict;

/* ---- Runtime callbacks invoked by generated code ------------------------- */

/* Header access. Names are case-insensitive per RFC 3261. get returns an empty
 * HmrStr (data may be NULL, len == 0) when the header is absent. */
HmrStr hmr_rt_get_header(HmrSipMsg* msg, HmrStr name);
int    hmr_rt_set_header(HmrSipMsg* msg, HmrStr name, HmrStr value);
int    hmr_rt_add_header(HmrSipMsg* msg, HmrStr name, HmrStr value);
int    hmr_rt_delete_header(HmrSipMsg* msg, HmrStr name);

/* Request-line / status accessors used by element rules. */
HmrStr hmr_rt_get_method(const HmrSipMsg* msg);
int    hmr_rt_is_request(const HmrSipMsg* msg);
uint32_t hmr_rt_status_code(const HmrSipMsg* msg);

/* Comparison primitives (return 1 on match, 0 otherwise). The pattern-rule
 * path uses precompiled regexes referenced by their table index; on a match it
 * records capture groups into the context for later $N back-references. */
int hmr_rt_str_eq(HmrStr a, HmrStr b, int case_insensitive);
int hmr_rt_regex_match(HmrContext* ctx, uint32_t regex_id, HmrStr subject);

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
 * requested component; replace rewrites it and returns the rebuilt header. */
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
