# HMR grammar

[`Hmr.g4`](./Hmr.g4) is the ANTLR4 grammar for the Oracle SBC *Header
Manipulation Rules* (HMR) configuration DSL (Oracle Communications Session
Border Controller 10.1.0, the `sip-manipulation` element family).

## Status: the single source of truth for the parser

`Hmr.g4` is **on the build's critical path** — there is no hand-written parser.
The Meson build (`custom_target('antlr_hmr')`, via
[`scripts/run_antlr.sh`](../scripts/run_antlr.sh)) runs

```sh
antlr4 -Dlanguage=Cpp -visitor -no-listener -o generated grammar/Hmr.g4
```

and compiles the generated `HmrLexer` / `HmrParser` / `HmrBaseVisitor` /
`HmrVisitor` against the ANTLR4 C++ runtime into the `hmr_antlr` library
(warnings suppressed for the generated TUs only).
[`hmr::parser::Parser`](../src/parser/parser.cpp) is a thin visitor over the
resulting parse tree: it walks the CST, reports diagnostics, and hands values to
`ast::AstFactory`. Editing the surface syntax means editing this grammar and
nothing else — the C++ parser is regenerated on the next build.

The one-time ANTLR tool jar + C++ runtime are fetched by
[`scripts/setup_antlr.sh`](../scripts/setup_antlr.sh) (see the
[user guide](../docs/user-guide.md)); the grammar is pinned to and checked with
`antlr-4.13.2`, which generates the lexer, parser, and visitor with no warnings.

## Block structure is keyword-delimited, not indentation-sensitive

Oracle's ACLI renders configuration as blocks introduced by distinct,
non-overlapping keywords (`sip-manipulation` / `header-rule` / `element-rule` /
`mime-*-rule`) whose bodies are a flat sequence of `key value` attribute lines.
A block ends when the next line opens a sibling/parent block keyword, or at end
of input. The grammar therefore resolves block boundaries purely by **keyword
lookahead**: horizontal whitespace is insignificant and indentation is purely
cosmetic, matching how the SBC actually emits and re-reads its config. (There is
no off-side rule and no `INDENT`/`DEDENT` denter — an earlier draft used one;
the production grammar does not.)

Only block-introducer and attribute-key keywords are reserved, and only in those
positions. Every other lexeme — enum values (`case-sensitive`, `uri-host`, …),
variables (`$LOCAL_IP`), regexes, IP literals, header names — is a `WORD` the
visitor classifies later, and the `keywordAsValue` rule re-admits every reserved
word in value position so no word is reserved away from values.

## Generating a parser for another language

The C++ target is what the build uses, but the same grammar drives other
targets unchanged — handy for an independent oracle (generate a second parser
and diff its behaviour) or editor tooling:

```sh
# Java (also emits a listener you can subclass):
java -jar antlr-4.13.2-complete.jar -Dlanguage=Java -o gen grammar/Hmr.g4

# Python, etc.:
java -jar antlr-4.13.2-complete.jar -Dlanguage=Python3 -o gen grammar/Hmr.g4
```
