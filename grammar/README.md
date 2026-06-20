# HMR grammar

[`Hmr.g4`](./Hmr.g4) is the ANTLR4 reference grammar for the Oracle SBC
*Header Manipulation Rules* (HMR) configuration DSL.

## Status: specification, not a build dependency

The production front-end does **not** invoke ANTLR. The reference parser is a
hand-written, indentation-aware lexer
([`src/parser/Lexer.cpp`](../src/parser/Lexer.cpp)) feeding a recursive-descent
parser ([`src/parser/Parser.cpp`](../src/parser/Parser.cpp)). Every parser rule
in `Hmr.g4` mirrors a function in that parser, so the grammar doubles as:

* a precise, reviewable specification of the surface syntax,
* the source for editor tooling / syntax highlighting, and
* an independent oracle: you can generate a second parser from it and diff its
  behaviour against the hand-written one.

Keeping ANTLR off the build's critical path is deliberate — the issue requires a
self-contained native toolchain with no code-generation step at compile time.

## Indentation

HMR is an off-side-rule language (block structure comes from leading
whitespace). A plain ANTLR lexer cannot emit the `INDENT` / `DEDENT` tokens this
requires, so the grammar declares them in its `tokens {}` prequel and expects a
*denter* to synthesise them from the whitespace/newline stream. The reference
C++ lexer already implements that denter; if you generate a parser from this
grammar, wrap the lexer with e.g.
[antlr-denter](https://github.com/yshavit/antlr-denter) or a Python-style
token-stream rewriter (tab stop = 8 columns, matching `Lexer::kTabWidth`).

## Generating a reference parser

```sh
# Java target (also emits a listener you can subclass):
java -jar antlr-4.13.2-complete.jar -Dlanguage=Java -o gen grammar/Hmr.g4

# Other targets work too, e.g. -Dlanguage=Cpp or -Dlanguage=Python3.
```

The grammar is checked with `antlr-4.13.2`; it generates a lexer, parser, and
listener with no warnings.
