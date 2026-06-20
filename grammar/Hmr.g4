// SPDX-License-Identifier: MIT
//
// Hmr.g4 — ANTLR4 grammar for the Oracle SBC "Header Manipulation Rules" (HMR)
// configuration DSL, as implemented by fastsbc_hmr.
//
// This grammar is the *specification* of the surface syntax. The production
// engine does not run ANTLR at build time: the reference front-end is a
// hand-written, indentation-aware lexer (src/parser/Lexer.cpp) feeding a
// recursive-descent parser (src/parser/Parser.cpp). Every rule below mirrors a
// function in that parser, so the two stay in lock-step and this file can be
// fed to `antlr4 Hmr.g4` to generate an independent reference parser or to
// drive editor tooling / syntax highlighting.
//
// ----------------------------------------------------------------------------
// Indentation sensitivity
// ----------------------------------------------------------------------------
// HMR is an off-side-rule (Python-like) language: block structure is expressed
// purely through leading whitespace, not braces. The reference lexer therefore
// synthesises three virtual tokens that a plain ANTLR lexer cannot emit on its
// own:
//
//     INDENT   leading whitespace of a line is strictly greater than the
//              enclosing block's column  -> open a new block
//     DEDENT   leading whitespace is less than the current block's column ->
//              close one block per popped level; an in-between column that
//              matches no open level is an error (see invalid_bad_indent.hmr)
//     NEWLINE  logical end of a non-blank, non-comment line
//
// A tab advances to the next multiple of 8 columns. Blank lines and
// comment-only lines do not participate in indentation tracking.
//
// To run this grammar through ANTLR you must wrap the generated lexer with a
// "denter" that turns the WS/NEWLINE stream into INDENT/DEDENT/NEWLINE tokens
// (e.g. antlr-denter, or a Python-style token-stream rewriter). The reference
// C++ lexer already does exactly this; see Lexer::tokenize / Lexer::lexLine.
//
// ----------------------------------------------------------------------------
// Keywords are contextual, not reserved
// ----------------------------------------------------------------------------
// The reference lexer has no keyword tokens at all: every unquoted run of
// non-whitespace is a single WORD, and the parser decides whether a WORD is a
// structural keyword or an attribute value by *position*. Consequently a value
// may legally be spelled like a keyword (e.g. `new-value name`). We preserve
// that property here: keyword literals are admitted by the `value` rule via the
// `keyword` alias, so no word is ever reserved away from the value position.

grammar Hmr;

// INDENT / DEDENT are emitted by the denter (see indentation note above), never
// matched from raw text. They are declared here so the parser rules can
// reference them; ANTLR requires the tokens{} section in the grammar prequel.
tokens { INDENT, DEDENT }

// =====================================================================
// Parser rules  (mirror src/parser/Parser.cpp)
// =====================================================================

// Parser::parse -> parseRuleset. A unit is exactly one sip-manipulation set.
unit
    : NEWLINE* ruleset EOF
    ;

// Parser::parseRuleset
ruleset
    : KW_SIP_MANIPULATION inlineName? NEWLINE
      ( INDENT rulesetItem* DEDENT )?
    ;

rulesetItem
    : headerRule
    | rulesetAttr
    ;

// Scalar attributes accepted directly under sip-manipulation. Unknown keys are
// accepted and ignored with a warning by the reference parser, hence the
// `genericAttr` fallback.
rulesetAttr
    : KW_NAME           value  NEWLINE
    | KW_DESCRIPTION    value  NEWLINE
    | KW_IMPORT         value  NEWLINE
    | KW_EXPORT         value  NEWLINE
    | KW_SPLIT_HEADERS  value  NEWLINE   // legacy / deprecated
    | KW_JOIN_HEADERS   value  NEWLINE   // legacy / deprecated
    | genericAttr
    ;

// Parser::parseHeaderRule
headerRule
    : KW_HEADER_RULE inlineName? NEWLINE
      ( INDENT headerRuleItem* DEDENT )?
    ;

headerRuleItem
    : elementRule
    | headerRuleAttr
    ;

headerRuleAttr
    : KW_NAME            value         NEWLINE
    | KW_HEADER_NAME     value         NEWLINE
    | KW_ACTION          headerAction  NEWLINE
    | KW_COMPARISON_TYPE comparison    NEWLINE
    | KW_MSG_TYPE        msgType       NEWLINE
    | KW_METHODS         methodList    NEWLINE
    | KW_MATCH_VALUE     value         NEWLINE
    | KW_NEW_VALUE       value         NEWLINE
    | genericAttr
    ;

// Parser::parseElementRule
elementRule
    : KW_ELEMENT_RULE inlineName? NEWLINE
      ( INDENT elementRuleItem* DEDENT )?
    ;

elementRuleItem
    : elementRuleAttr
    ;

elementRuleAttr
    : KW_NAME            value         NEWLINE
    | KW_PARAMETER_NAME  value         NEWLINE
    | KW_TYPE            elementType   NEWLINE
    | KW_ACTION          elementAction NEWLINE
    | KW_MATCH_VAL_TYPE  matchValType  NEWLINE
    | KW_COMPARISON_TYPE comparison    NEWLINE
    | KW_MATCH_VALUE     value         NEWLINE
    | KW_NEW_VALUE       value         NEWLINE
    | genericAttr
    ;

// Optional inline name token right after a block keyword
// (e.g. `header-rule anonFromHost`). Most configs put `name` on its own line.
inlineName
    : value
    ;

// Any otherwise-unrecognised `key value` line. The reference parser warns and
// ignores it rather than failing, so editors should treat it as benign.
genericAttr
    : valueAtom value? NEWLINE
    ;

// ---- enumerated attribute values (AstFactory lookup tables) ----------------

headerAction
    : ( KW_NONE | KW_ADD | KW_STORE | KW_MANIPULATE | KW_REPLACE
      | KW_FIND_REPLACE_ALL | KW_DELETE | KW_DELETE_ELEMENT | KW_DELETE_HEADER
      | KW_SIP_MANIP | KW_LOG | KW_REJECT )
    ;

elementAction
    : ( KW_NONE | KW_ADD | KW_STORE | KW_REPLACE | KW_DELETE_ELEMENT
      | KW_DELETE_HEADER | KW_FIND_REPLACE_ALL | KW_SIP_MANIP | KW_LOG
      | KW_REJECT )
    ;

elementType
    : ( KW_HEADER_VALUE | KW_HEADER_PARAM_NAME | KW_HEADER_PARAM
      | KW_URI_DISPLAY | KW_URI_USER | KW_URI_USER_PARAM | KW_URI_HOST
      | KW_URI_PORT | KW_URI_PARAM_NAME | KW_URI_PARAM | KW_URI_HEADER_NAME
      | KW_URI_HEADER | KW_STATUS_CODE | KW_REASON_PHRASE )
    ;

comparison
    : ( KW_CASE_SENSITIVE | KW_CASE_INSENSITIVE | KW_PATTERN_RULE | KW_BOOLEAN
      | KW_REFER_CASE_SENSITIVE | KW_REFER_CASE_INSENSITIVE )
    ;

matchValType
    : ( KW_ANY | KW_AN | KW_IP | KW_FQDN )
    ;

msgType
    : ( KW_ANY | KW_REQUEST | KW_REPLY | KW_OUT_OF_DIALOG )
    ;

// `methods INVITE,UPDATE` — comma/space separated SIP method tokens.
methodList
    : value
    ;

// ---- value (match-value / new-value / scalar text) -------------------------
//
// AstFactory/Value::parse treats the joined text of a value as a small
// interpolation mini-language:
//   * $NAME              context variable     ($LOCAL_IP, $TRUNK_GROUP, $REALM)
//   * $0 $1 $2 ...       capture back-reference from a pattern-rule match
//   * everything else    literal text; for a match-value the literal is a
//                        regular expression (e.g. `internal\.local`,
//                        `sip:1900[0-9]+@`)
// These are lexically part of WORD/STRING, so they are not separate tokens
// here; the interpolation is resolved in Value.cpp, not the grammar.
value
    : valueAtom+
    ;

valueAtom
    : WORD
    | STRING
    | keyword          // keywords are contextual: usable in value position
    ;

// Every keyword literal, re-exposed so it can appear as an ordinary value.
keyword
    : KW_SIP_MANIPULATION | KW_HEADER_RULE | KW_ELEMENT_RULE
    | KW_NAME | KW_DESCRIPTION | KW_IMPORT | KW_EXPORT
    | KW_SPLIT_HEADERS | KW_JOIN_HEADERS
    | KW_HEADER_NAME | KW_ACTION | KW_COMPARISON_TYPE | KW_MSG_TYPE
    | KW_METHODS | KW_MATCH_VALUE | KW_NEW_VALUE | KW_PARAMETER_NAME
    | KW_TYPE | KW_MATCH_VAL_TYPE
    | KW_NONE | KW_ADD | KW_STORE | KW_MANIPULATE | KW_REPLACE
    | KW_FIND_REPLACE_ALL | KW_DELETE | KW_DELETE_ELEMENT | KW_DELETE_HEADER
    | KW_SIP_MANIP | KW_LOG | KW_REJECT
    | KW_HEADER_VALUE | KW_HEADER_PARAM_NAME | KW_HEADER_PARAM
    | KW_URI_DISPLAY | KW_URI_USER | KW_URI_USER_PARAM | KW_URI_HOST
    | KW_URI_PORT | KW_URI_PARAM_NAME | KW_URI_PARAM | KW_URI_HEADER_NAME
    | KW_URI_HEADER | KW_STATUS_CODE | KW_REASON_PHRASE
    | KW_CASE_SENSITIVE | KW_CASE_INSENSITIVE | KW_PATTERN_RULE | KW_BOOLEAN
    | KW_REFER_CASE_SENSITIVE | KW_REFER_CASE_INSENSITIVE
    | KW_ANY | KW_AN | KW_IP | KW_FQDN
    | KW_REQUEST | KW_REPLY | KW_OUT_OF_DIALOG
    ;

// =====================================================================
// Lexer rules
// =====================================================================
//
// IMPORTANT: keyword tokens are listed *before* WORD so an exact match wins the
// ANTLR maximal-munch tie-break. A longer run (e.g. `uri-hostname`) still falls
// through to WORD. This reproduces the reference parser's behaviour where an
// exact keyword spelling is structural but any other word is a plain value.

// ---- block keywords --------------------------------------------------------
KW_SIP_MANIPULATION : 'sip-manipulation' ;
KW_HEADER_RULE      : 'header-rule' | 'header-rules' ;
KW_ELEMENT_RULE     : 'element-rule' | 'element-rules' ;

// ---- attribute keys --------------------------------------------------------
KW_NAME             : 'name' ;
KW_DESCRIPTION      : 'description' ;
KW_IMPORT           : 'import' ;
KW_EXPORT           : 'export' ;
KW_SPLIT_HEADERS    : 'split-headers' ;
KW_JOIN_HEADERS     : 'join-headers' ;
KW_HEADER_NAME      : 'header-name' ;
KW_ACTION           : 'action' ;
KW_COMPARISON_TYPE  : 'comparison-type' ;
KW_MSG_TYPE         : 'msg-type' ;
KW_METHODS          : 'methods' ;
KW_MATCH_VALUE      : 'match-value' ;
KW_NEW_VALUE        : 'new-value' ;
KW_PARAMETER_NAME   : 'parameter-name' ;
KW_TYPE             : 'type' ;
KW_MATCH_VAL_TYPE   : 'match-val-type' ;

// ---- header / element actions ---------------------------------------------
KW_NONE             : 'none' ;
KW_ADD              : 'add' ;
KW_STORE            : 'store' ;
KW_MANIPULATE       : 'manipulate' ;
KW_REPLACE          : 'replace' ;
KW_FIND_REPLACE_ALL : 'find-replace-all' ;
KW_DELETE           : 'delete' ;
KW_DELETE_ELEMENT   : 'delete-element' ;
KW_DELETE_HEADER    : 'delete-header' ;
KW_SIP_MANIP        : 'sip-manip' ;
KW_LOG              : 'log' ;
KW_REJECT           : 'reject' ;

// ---- element types ---------------------------------------------------------
KW_HEADER_VALUE      : 'header-value' ;
KW_HEADER_PARAM_NAME : 'header-param-name' ;
KW_HEADER_PARAM      : 'header-param' ;
KW_URI_DISPLAY       : 'uri-display' ;
KW_URI_USER          : 'uri-user' ;
KW_URI_USER_PARAM    : 'uri-user-param' ;
KW_URI_HOST          : 'uri-host' ;
KW_URI_PORT          : 'uri-port' ;
KW_URI_PARAM_NAME    : 'uri-param-name' ;
KW_URI_PARAM         : 'uri-param' ;
KW_URI_HEADER_NAME   : 'uri-header-name' ;
KW_URI_HEADER        : 'uri-header' ;
KW_STATUS_CODE       : 'status-code' ;
KW_REASON_PHRASE     : 'reason-phrase' ;

// ---- comparison types ------------------------------------------------------
KW_CASE_SENSITIVE        : 'case-sensitive' ;
KW_CASE_INSENSITIVE      : 'case-insensitive' ;
KW_PATTERN_RULE          : 'pattern-rule' ;
KW_BOOLEAN               : 'boolean' ;
KW_REFER_CASE_SENSITIVE  : 'refer-case-sensitive' ;
KW_REFER_CASE_INSENSITIVE: 'refer-case-insensitive' ;

// ---- match-val-type / msg-type --------------------------------------------
KW_ANY           : 'any' ;
KW_AN            : 'an' ;     // tolerated truncation of "any" in legacy configs
KW_IP            : 'ip' ;
KW_FQDN          : 'fqdn' ;
KW_REQUEST       : 'request' ;
KW_REPLY         : 'reply' ;
KW_OUT_OF_DIALOG : 'out-of-dialog' ;

// ---- structural & literal tokens ------------------------------------------

// A double-quoted string with simple backslash escapes (\n \t \r \" \\).
STRING
    : '"' ( '\\' . | ~["\\\r\n] )* '"'
    ;

// A bareword: a maximal run of non-whitespace that does not *start* with '#'
// (which would begin a comment) or '"' (which would begin a string). Hyphens,
// dots, ':', '$', '@', regex metacharacters, etc. are all ordinary word chars,
// matching the reference lexer's "stop only at space/tab" rule.
WORD
    : ~[ \t\r\n#"] ~[ \t\r\n]*
    ;

// '#' to end of line is a comment, but only when '#' begins a token (i.e. it is
// preceded by whitespace or starts the line); a '#' embedded in a WORD is part
// of the word. Because WORD cannot start with '#', a stand-alone '#' lands here.
COMMENT
    : '#' ~[\r\n]* -> skip
    ;

// NEWLINE is significant (it terminates each attribute line and block header).
// A denter post-processes the WS around newlines into INDENT/DEDENT tokens; see
// the indentation note at the top of this file.
NEWLINE
    : ( '\r'? '\n' )+
    ;

// Horizontal whitespace is consumed by the denter for indentation accounting
// and is otherwise insignificant between tokens on a line.
WS
    : [ \t]+ -> skip
    ;
