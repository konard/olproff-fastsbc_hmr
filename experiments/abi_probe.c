#include "hmr/runtime/hmr_runtime.h"

/* A representative apply: get a header, build a new value, set it, match a
 * regex, reject. Exercises by-value HmrStr args AND returns. */
int probe(HmrSipMsg* msg, HmrContext* ctx) {
    HmrStr from = { "From", 4 };
    HmrStr v = hmr_rt_get_header(msg, from);          /* returns HmrStr by value */
    if (hmr_rt_regex_match(ctx, 0, v)) {              /* HmrStr arg by value */
        hmr_rt_val_reset(ctx);
        HmrStr lit = { "sip:", 4 };
        hmr_rt_val_append_lit(ctx, lit);
        hmr_rt_val_append_var(ctx, HMR_VAR_LOCAL_IP);
        HmrStr built = hmr_rt_val_finish(ctx);        /* returns HmrStr */
        hmr_rt_set_header(msg, from, built);          /* two HmrStr args */
    }
    HmrStr cap = hmr_rt_get_capture(ctx, 1);
    HmrStr reason = { "blocked", 7 };
    hmr_rt_reject(ctx, 403, reason);
    return cap.len ? 1 : 0;
}
