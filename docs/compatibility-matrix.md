# Oracle HMR compatibility matrix

This table records what the compiler does with each Oracle HMR construct, split
into three honest buckets:

* **✅ Lowered** — parsed *and* emitted to IR; exercised by tests/samples.
* **🟡 Parsed** — accepted by the lexer/parser (and validated), but the code
  generator currently treats it as a no-op or an approximation. The relevant
  gap is noted.
* **⬜ Recognized** — listed in the [DSL reference](./oracle-hmr-reference.md)
  and accepted as syntax, but with no dedicated runtime backing yet.

The authority for keyword spelling is
[oracle-hmr-reference.md](./oracle-hmr-reference.md); the formal grammar is
[`grammar/Hmr.g4`](../grammar/Hmr.g4). "Lowered" rows are grounded in
`src/codegen/IrGenerator.cpp`.

## Block structure

| Construct | Status | Notes |
|---|---|---|
| `sip-manipulation` (`name`, `description`) | ✅ | the ruleset root |
| `header-rule` | ✅ | full header-level rule |
| `element-rule` | ✅ | nested element rule |
| inline name after a block keyword | ✅ | canonical "name as child" form also accepted |
| `mime-rules` / `mime-isup-rules` / `mime-sdp-rules` | ⬜ | body manipulation — not compiled |
| `import` / `export` | 🟡 | parsed into the AST; not resolved at compile time |
| `split-headers` / `join-headers` | 🟡 | legacy; accepted-but-deprecated, no codegen |

## header-rule `action`

| Action | Status | Notes |
|---|---|---|
| `add` | ✅ | `hmr_rt_add_header(new-value)` |
| `replace` | ✅ | `set_header(new-value)`, or delegate to element rules |
| `manipulate` | ✅ | header-level replace via `new-value`, else element rules |
| `delete` / `delete-header` | ✅ | `hmr_rt_delete_header` |
| `store` | ✅ | stores capture `$0` (pattern-rule) or the header value into a slot |
| `log` | ✅ | `hmr_rt_log` |
| `reject` | ✅ | `hmr_rt_reject(403, reason)` then branch to the reject exit |
| `none` | ✅ | intentional no-op |
| `find-replace-all` | 🟡 | parsed; not yet lowered (single-shot replace covers most cases) |
| `delete-element` (at header level) | 🟡 | parsed; expressed via element rules instead |
| `sip-manip` | 🟡 | parsed; nested-manipulation invocation not yet lowered |

## element-rule `action`

| Action | Status | Notes |
|---|---|---|
| `replace` | ✅ | rewrites the element (or whole value) via `hmr_rt_uri_set`/`set_header` |
| `delete-element` | ✅ | clears the element, or deletes the header for whole-value |
| `store` | ✅ | stores the element value into a slot |
| `reject` | ✅ | `hmr_rt_reject` then branch to the reject exit |
| `add` | 🟡 | parsed; element-level add not yet lowered |
| `find-replace-all` | 🟡 | parsed; not yet lowered |
| `sip-manip` / `delete-header` / `log` / `none` | 🟡 | parsed; no dedicated element-level codegen |

## element-rule `type`

| Type | Status | Maps to |
|---|---|---|
| `header-value` | ✅ | `HMR_URI_WHOLE` (whole header value) |
| `uri-display` | ✅ | `HMR_URI_DISPLAY` |
| `uri-user` | ✅ | `HMR_URI_USER` |
| `uri-host` | ✅ | `HMR_URI_HOST` |
| `uri-port` | ✅ | `HMR_URI_PORT` |
| `header-param-name` / `header-param` | ⬜ | header parameter access — skipped (no-op) |
| `uri-user-param` / `uri-param-name` / `uri-param` | ⬜ | URI parameter access — skipped |
| `uri-header-name` / `uri-header` | ⬜ | embedded URI headers — skipped |
| `status-code` / `reason-phrase` | ⬜ | status-line elements — skipped |

Element rules whose `type` is not yet lowered are skipped (no-op) rather than
miscompiled.

## `comparison-type`

All comparison types lower to a **precompiled regex** guard
(`hmr_rt_regex_match`) — see
[why match-values are always regexes](./runtime-api.md#why-match-values-are-always-regexes).

| Comparison | Status | Notes |
|---|---|---|
| `case-sensitive` | ✅ | regex, case-sensitive |
| `case-insensitive` | ✅ | regex, case-insensitive flag |
| `pattern-rule` | ✅ | regex; also exposes captures `$0..$N` |
| `refer-case-sensitive` | ✅ | treated as a case-sensitive regex |
| `refer-case-insensitive` | ✅ | treated as a case-insensitive regex |
| `boolean` | 🟡 | a literal regex still matches; a `$`-reference boolean expression is treated as always-true (documented v1 gap) |

## `msg-type`

| Value | Status | Notes |
|---|---|---|
| `any` | ✅ | no message-type guard |
| `request` | ✅ | guarded with `hmr_rt_is_request` |
| `reply` | ✅ | guarded with `!hmr_rt_is_request` |
| `out-of-dialog` | 🟡 | parsed; treated as `any` (no dialog-state guard) |

## `methods`

| Construct | Status | Notes |
|---|---|---|
| comma-separated method list | ✅ | case-insensitive OR across the listed methods |
| empty list (all methods) | ✅ | no method guard emitted |

## `match-val-type`

| Value | Status | Notes |
|---|---|---|
| `any` / `ip` / `fqdn` | 🟡 | parsed and validated; used as a hint, not enforced as a separate guard |

## `new-value` expressions

| Construct | Status | Notes |
|---|---|---|
| string literals (with `\"` escapes) | ✅ | `val_append_lit` |
| `$VAR` built-ins | ✅ | `val_append_var` (see variable table below) |
| `$N` capture back-references | ✅ | `val_append_capture` |
| `$ruleName` stored back-reference | ✅ | `val_append_slot` (store/load slot) |
| `+` concatenation | ✅ | sequential builder appends |
| `-` strip / `!` boolean-not operators | 🟡 | negation is modelled on match-values; advanced strip/boolean forms are a v1 gap |

## Built-in variables

These have stable `HmrVarId`s and are wired through the ABI (the host binds the
ones it knows; unknown variables resolve to the empty string):

`$LOCAL_IP`, `$REMOTE_IP`, `$LOCAL_PORT`, `$REMOTE_PORT`, `$TRUNK_GROUP`,
`$REALM`, `$INTERFACE`, `$METHOD`, `$RURI_USER`, `$RURI_HOST`, `$TO_USER`,
`$TO_HOST`, `$FROM_USER`, `$FROM_HOST`.

The broader Oracle catalogue (`$REPLY_IP`, `$TARGET_IP`, the per-header
`$<HDR>_PHONE` families, `$ORIGINAL`, `$CALL_ID`, `$TIMESTAMP_UTC`, `$CRLF`, …)
is documented in [oracle-hmr-reference.md](./oracle-hmr-reference.md) and parses,
but only the IDs above currently have runtime backing; others resolve to empty.
Adding one is a localized change: extend `HmrVarId`, the `variableId` map in the
generator, and the host's `setVar` calls.
