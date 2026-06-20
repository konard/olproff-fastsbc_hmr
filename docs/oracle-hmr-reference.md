# Oracle SBC HMR — DSL Reference (compiled from Oracle documentation)

> This reference was compiled from the Oracle Communications Session Border
> Controller 9.2.0 ACLI Reference and the Oracle ESBC E-CZ8.1.0 HMR Guide
> ("SIP Header and Parameter Manipulation Configuration", "HMR Components",
> "Configuration Examples"). It is the authority for the grammar, lexer keyword
> sets and the compatibility matrix in this project.

## Block structure & attribute names

### `sip-manipulation`
| Attribute | Notes |
|---|---|
| `name` | Identifier for the manipulation set. |
| `description` | Free text. |
| `header-rule` (0..*) | Header manipulation rules. |
| `mime-rules`, `mime-isup-rules`, `mime-sdp-rules` | Body manipulation (not yet compiled). |
| `import`, `export` | Ruleset import/export filenames. |
| `split-headers`, `join-headers` | **Legacy / uncertain** — absent from the 9.2.0 ACLI attribute list. Accepted but deprecated. |

### `header-rule`
`name`, `header-name`, `action`, `comparison-type`, `match-value`, `msg-type`,
`methods`, `new-value`, `element-rule` (0..*).

### `element-rule`
`name`, `parameter-name`, `type`, `action`, `match-val-type`,
`comparison-type`, `match-value`, `new-value`.

> **Format note:** in authentic Oracle ACLI dumps the block keyword stands
> alone on its line and `name` is a *child* attribute. This compiler accepts
> that canonical form and also an optional inline name after the keyword.

> **Naming constraints:** rule names start with a letter, then letters / digits
> / `_`; dashes are reserved (the `-` strip operator); all-uppercase names are
> reserved for built-in variables.

## Enumerated values (exact spelling)

* header-rule `action`: `none`, `add`, `store`, `manipulate`, `replace`,
  `find-replace-all`, `delete`, `delete-element`, `delete-header`, `sip-manip`,
  `log`, `reject`. (`delete` is an alias of `delete-header`.)
* element-rule `action`: `none`, `add`, `store`, `replace`, `delete-element`,
  `delete-header`, `find-replace-all`, `sip-manip`, `log`, `reject`.
* element-rule `type`: `header-value`, `header-param-name`, `header-param`,
  `uri-display`, `uri-user`, `uri-user-param`, `uri-host`, `uri-port`,
  `uri-param-name`, `uri-param`, `uri-header-name`, `uri-header`,
  `status-code`, `reason-phrase`.
* `comparison-type`: `case-sensitive`, `case-insensitive`, `pattern-rule`,
  `boolean`, `refer-case-sensitive`, `refer-case-insensitive`.
* `match-val-type`: `ip`, `fqdn`, `any` (default `any`).
* `msg-type`: `any`, `request`, `reply`, `out-of-dialog`.
* `methods`: free comma-separated SIP method list (empty = all).

## Built-in variables ($-prefixed, all uppercase)

Network/interface: `$LOCAL_IP`, `$LOCAL_PORT`, `$REMOTE_IP`, `$REMOTE_PORT`,
`$REPLY_IP`, `$REPLY_PORT`, `$TARGET_IP`, `$TARGET_PORT`, `$REMOTE_VIA_HOST`.

Per-header URI families for `TO`, `FROM`, `CONTACT`, `RURI`, `PAI`, `PPI`,
`PCPID` — `$<HDR>_USER`, `$<HDR>_PHONE`, `$<HDR>_HOST`, `$<HDR>_PORT`.

Content/misc: `$ORIGINAL`, `$CALL_ID`, `$TIMESTAMP_UTC`, `$CRLF`,
`$MANIP_STRING`, `$MANIP_PATTERN`, `$M_STRING`.

Trunk-group (legacy): `$TRUNK_GROUP`, `$TRUNK_GROUP_CONTEXT`, `$T_GROUP`,
`$T_CONTEXT`.

## Pattern matching & expressions

* Regex capture groups `( … )`, up to 10 matches. `$0` = whole match,
  `$1..$N` = captured groups.
* Back-references: `$ruleName` (boolean: did it match), `$ruleName.$N` (a
  capture from that rule), `$headerRule.$elementRule.$N` (element capture).
* Self-storing `manipulate`/`replace` with `pattern-rule` lets `new-value` use
  bare `$1`, `$2`, `$0`.
* `new-value` operators: `+` concatenation, `-` strip pattern, `!` boolean NOT.
* String literals are double-quoted with `\"` escapes.
* `pattern-rule` evaluates `match-value` as an ERE regex; `boolean` evaluates a
  stored boolean/reference expression.

## Confidence notes
* `split-headers`/`join-headers` are legacy and absent from the 9.2.0 ACLI list.
* The "friendly" HMR guide lists a shorter header-rule action set
  (`add|delete|manipulate|store|none`); the ACLI reference is the full set used
  here.
* `match-val-type` third value renders as `AN` in one ACLI page — read as `any`.
* `$<HDR>_*` and `$T_GROUP`/`$T_CONTEXT` are newer-firmware variable families.
