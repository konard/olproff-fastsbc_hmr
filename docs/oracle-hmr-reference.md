# Oracle SBC HMR — DSL Reference (compiled from Oracle documentation)

> This reference targets the **Oracle Communications Session Border Controller
> 10.1.0** `sip-manipulation` element family. The attribute and enumerated-value
> sets below were taken from the 10.1.0 *ACLI Reference Guide*
> (`sip-manipulation > header-rules > sip-element-rules`), cross-checked against
> the version-specific 10.0.0 ACLI Reference HTML (whose `sip-manipulation`
> schema is identical to 10.1.0), with the Oracle E-SBC E-CZ8.1.0 *HMR Guide*
> ("SIP Header and Parameter Manipulation Configuration", "HMR Components",
> "Configuration Examples") supplying the prose semantics. It is the authority
> for the grammar, lexer keyword sets and the compatibility matrix in this
> project.
>
> **Sources**
> - Oracle SBC **10.1.0** *ACLI Reference Guide* (PDF).
> - Oracle SBC **10.0.0** *ACLI Reference*: `sip-manipulation > header-rules >
>   sip-element-rules` (version-specific HTML; same schema as 10.1.0).
> - Oracle E-SBC **E-CZ8.1.0** *HMR Guide*.

## Block structure & attribute names

### `sip-manipulation`
| Attribute | Notes |
|---|---|
| `name` | Identifier for the manipulation set. |
| `description` | Free text. |
| `header-rule` (0..*) | Header manipulation rules. |
| `mime-rules`, `mime-isup-rules`, `mime-sdp-rules` | Body manipulation (not yet compiled). |
| `import`, `export` | Ruleset import/export filenames. |
| `split-headers`, `join-headers` | **Legacy / uncertain** — absent from the 10.1.0 ACLI attribute list. Accepted but deprecated. |

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

The 10.1.0 ACLI lists the **same** ten-value `action` enum for both `header-rule`
and `sip-element-rule`; defaults are `none`.

* header-rule `action` (ACLI): `none`, `add`, `store`, `sip-manip`, `replace`,
  `find-replace-all`, `delete-element`, `delete-header`, `log`, `reject`.
  Accepted aliases: `manipulate` (HMR-guide spelling, see notes) and `delete`
  (alias of `delete-header`).
* element-rule `action` (ACLI): `none`, `add`, `store`, `sip-manip`, `replace`,
  `find-replace-all`, `delete-element`, `delete-header`, `log`, `reject`.
* element-rule `type` (default `none`): `header-value`, `header-param-name`,
  `header-param`, `uri-display`, `uri-user`, `uri-host`, `uri-port`,
  `uri-param-name`, `uri-param`, `uri-header-name`, `uri-header`,
  `uri-user-param`, `status-code`, `reason-phrase`, `uri-user-only`,
  `uri-phone-number-only`. (`uri-user-only` and `uri-phone-number-only` are the
  10.1.0 user-part refinements.)
* `comparison-type` (default `case-sensitive`): `case-sensitive`,
  `case-insensitive`, `pattern-rule`, `refer-case-sensitive`,
  `refer-case-insensitive`, `boolean`.
* `match-val-type` (default `any`): `any`, `ip`, `fqdn` (rendered uppercase
  `ANY`/`IP`/`FQDN` in the ACLI; matched case-insensitively).
* `msg-type` (default `any`): `any`, `request`, `reply`, `out-of-dialog`.
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
* `split-headers`/`join-headers` are legacy and absent from the 10.1.0 ACLI
  attribute list; accepted-but-deprecated.
* `manipulate` is documented as a header-rule `action` in the Oracle *HMR Guide*
  ("when the action parameter is set to add or to manipulate, you enter the new
  value…"), though the ACLI `action` enum lists only the leaf operations. It is
  accepted and is the conventional trigger for drilling into element-rules.
* `delete` is accepted as an alias of `delete-header`.
* `match-val-type` renders uppercase (`ANY`/`IP`/`FQDN`) in the ACLI; the parser
  matches it case-insensitively. (One older page truncates `ANY` to `AN`.)
* `$<HDR>_*` and `$T_GROUP`/`$T_CONTEXT` are newer-firmware variable families.
