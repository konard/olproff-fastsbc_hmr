// SPDX-License-Identifier: MIT
//
// Hmr.g4 — ANTLR4 grammar for the Oracle SBC Header Manipulation Rules (HMR)
// configuration DSL (Oracle Communications Session Border Controller 10.1.0,
// the "sip-manipulation" element family).
//
// This grammar is the *single source of truth* for the parser. The build runs
//
//     antlr4 -Dlanguage=Cpp -visitor -no-listener -o generated grammar/Hmr.g4
//
// and compiles the generated HmrLexer/HmrParser against the ANTLR4 C++ runtime;
// hmr::parser::Parser is a thin visitor over the resulting parse tree
// (see src/parser/parser.cpp).
//
// ----------------------------------------------------------------------------
// Block structure is keyword-delimited, NOT indentation-sensitive
// ----------------------------------------------------------------------------
// Oracle's ACLI renders configuration as blocks introduced by distinct,
// non-overlapping keywords (sip-manipulation / header-rule / element-rule /
// mime-*-rule) whose bodies are a flat sequence of `key value` attribute lines.
// A block ends when the next line opens a sibling/parent block keyword, or at
// end of input. ANTLR therefore resolves block boundaries purely by keyword
// lookahead and horizontal whitespace is insignificant — indentation is purely
// cosmetic. This matches how the SBC actually emits and re-reads config.
//
// ----------------------------------------------------------------------------
// Keywords are reserved only at the start of a line
// ----------------------------------------------------------------------------
// Only block-introducer and attribute-key keywords are reserved lexemes. Every
// other lexeme — enum values (case-sensitive, uri-host, request, ...),
// variables ($LOCAL_IP), regexes, IP literals, header names — is a WORD that the
// AST factory classifies later. A value may still legally be spelled like a
// keyword (e.g. `new-value name`): the `keywordAsValue` rule re-admits every
// reserved word in value position, so no word is reserved away from values.
// Empty attribute values (`key` alone on a line) are valid Oracle config.

grammar Hmr;

// ===========================================================================
// Parser rules
// ===========================================================================

// A configuration is zero or more sip-manipulation sets. Leading blank lines and
// comment-only lines (which the lexer collapses into NEWLINE tokens) are
// tolerated.
unit : NEWLINE* manipulation* EOF ;

manipulation
    : KW_SIP_MANIPULATION value? NEWLINE manipItem*
    ;

manipItem
    : headerRule
    | mimeRule
    | attribute
    | NEWLINE      // blank / comment-only line inside the block
    ;

// mime-rule / mime-isup-rule / mime-sdp-rule share the header-rule body shape.
// Recognized for Oracle compatibility; the visitor reports them as unsupported
// and skips their bodies (the compiler targets the SIP header/element rules).
mimeRule
    : (KW_MIME_RULE | KW_MIME_ISUP_RULE | KW_MIME_SDP_RULE) value? NEWLINE headerRuleItem*
    ;

headerRule
    : KW_HEADER_RULE value? NEWLINE headerRuleItem*
    ;

headerRuleItem
    : elementRule
    | attribute
    | NEWLINE
    ;

elementRule
    : KW_ELEMENT_RULE value? NEWLINE elementRuleItem*
    ;

elementRuleItem
    : attribute
    | NEWLINE
    ;

// A `key value?` line. The value is optional (Oracle permits empty values) and
// is terminated by end of line.
attribute
    : key value? NEWLINE
    ;

// Recognized attribute keys plus a catch-all (WORD) so unknown / forward-compat
// attributes parse and are reported as warnings by the visitor instead of
// aborting the parse (error recovery).
key
    : KW_NAME | KW_DESCRIPTION | KW_HEADER_NAME | KW_ACTION
    | KW_COMPARISON_TYPE | KW_MSG_TYPE | KW_METHODS | KW_MATCH_VALUE
    | KW_NEW_VALUE | KW_PARAMETER_NAME | KW_TYPE | KW_MATCH_VAL_TYPE
    | KW_SPLIT_HEADERS | KW_JOIN_HEADERS | KW_IMPORT | KW_EXPORT
    | WORD
    ;

// A value is one or more atoms on the same line. The visitor joins atom text and
// hands it to ast::Value / ast::AstFactory for interpolation / enum mapping.
value : valueAtom+ ;

valueAtom
    : WORD | STRING | keywordAsValue
    ;

keywordAsValue
    : KW_SIP_MANIPULATION | KW_HEADER_RULE | KW_ELEMENT_RULE
    | KW_MIME_RULE | KW_MIME_ISUP_RULE | KW_MIME_SDP_RULE
    | KW_NAME | KW_DESCRIPTION | KW_HEADER_NAME | KW_ACTION
    | KW_COMPARISON_TYPE | KW_MSG_TYPE | KW_METHODS | KW_MATCH_VALUE
    | KW_NEW_VALUE | KW_PARAMETER_NAME | KW_TYPE | KW_MATCH_VAL_TYPE
    | KW_SPLIT_HEADERS | KW_JOIN_HEADERS | KW_IMPORT | KW_EXPORT
    ;

// ===========================================================================
// Lexer rules
// ===========================================================================
// Keyword tokens are listed before WORD so an exact spelling wins ANTLR's
// maximal-munch tie-break, while a longer run (e.g. `header-named`, `X-Custom`)
// still falls through to WORD.

// ---- block-introducer keywords --------------------------------------------
KW_SIP_MANIPULATION : 'sip-manipulation' ;
KW_HEADER_RULE      : 'header-rule'     | 'header-rules' ;
KW_ELEMENT_RULE     : 'element-rule'    | 'element-rules' ;
KW_MIME_RULE        : 'mime-rule'       | 'mime-rules' ;
KW_MIME_ISUP_RULE   : 'mime-isup-rule'  | 'mime-isup-rules' ;
KW_MIME_SDP_RULE    : 'mime-sdp-rule'   | 'mime-sdp-rules' ;

// ---- attribute keys --------------------------------------------------------
KW_NAME             : 'name' ;
KW_DESCRIPTION      : 'description' ;
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
KW_SPLIT_HEADERS    : 'split-headers' ;
KW_JOIN_HEADERS     : 'join-headers' ;
KW_IMPORT           : 'import' ;
KW_EXPORT           : 'export' ;

// ---- structural & literal tokens ------------------------------------------

// Double-quoted string with simple backslash escapes (\n \t \r \" \\).
STRING  : '"' ( '\\' . | ~["\\\r\n] )* '"' ;

// A bareword: a maximal run of non-whitespace not starting with '#' (comment) or
// '"' (string). Hyphens, dots, ':', '$', '@', regex metacharacters, '/' (CIDR)
// are ordinary word characters.
WORD    : ~[ \t\r\n#"] ~[ \t\r\n]* ;

// '#' to end of line is a comment. WORD cannot start with '#', so a stand-alone
// '#' lands here; a '#' embedded in a WORD stays part of the word.
COMMENT : '#' ~[\r\n]* -> skip ;

// NEWLINE is significant: it terminates every attribute line and block header.
// Consecutive newlines (blank lines) collapse into one token.
NEWLINE : ( '\r'? '\n' )+ ;

// Horizontal whitespace is insignificant between tokens.
WS      : [ \t]+ -> skip ;
