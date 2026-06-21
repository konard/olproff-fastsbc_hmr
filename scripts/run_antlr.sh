#!/bin/sh
# SPDX-License-Identifier: MIT
#
# run_antlr.sh — thin, deterministic wrapper around the ANTLR4 generator used by
# the Meson custom_target. It exists so the build invokes exactly one command
# regardless of whether ANTLR is available as a `java -jar` jar or as a native
# `antlr4` program, and so the output directory is created first.
#
# Usage:  run_antlr.sh <JAR_OR_"antlr4"> <GRAMMAR.g4> <OUTDIR>
#
# With absolute paths, `antlr4 -o OUTDIR GRAMMAR` writes the generated files
# FLAT into OUTDIR (no package/path mirroring), which is what Meson expects.
set -eu

if [ "$#" -ne 3 ]; then
    echo "usage: run_antlr.sh <jar|antlr4> <grammar.g4> <outdir>" >&2
    exit 2
fi

ANTLR="$1"
GRAMMAR="$2"
OUTDIR="$3"

mkdir -p "$OUTDIR"

# Generate a C++ visitor parser (no listener) for the grammar.
set -- -Dlanguage=Cpp -visitor -no-listener -o "$OUTDIR" "$GRAMMAR"

case "$ANTLR" in
    *.jar) exec java -jar "$ANTLR" "$@" ;;
    *)     exec "$ANTLR" "$@" ;;
esac
