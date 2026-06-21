#!/bin/sh
# SPDX-License-Identifier: MIT
#
# setup_antlr.sh — fetch a pinned ANTLR4 tool jar and build the matching ANTLR4
# C++ runtime into a local prefix. Both the generator and the runtime must be
# the SAME version, so we pin one version here and use it for both.
#
# Usage:   scripts/setup_antlr.sh [PREFIX]
# Default PREFIX is "$PWD/.antlr". On success it prints, on stdout, the three
# values the Meson build needs (so callers can `eval` them):
#
#   ANTLR_JAR=<path to antlr-<ver>-complete.jar>
#   ANTLR_INC=<runtime include dir (contains antlr4-runtime.h)>
#   ANTLR_LIBDIR=<runtime library dir (contains libantlr4-runtime.a)>
#
# Requires: java (to run the generator later), cmake, a C++ compiler, curl/wget,
# and unzip. Building the runtime is cached: re-running is a no-op once built.
set -eu

ANTLR_VERSION="${ANTLR_VERSION:-4.13.2}"
PREFIX="${1:-$PWD/.antlr}"
PREFIX="$(mkdir -p "$PREFIX" && cd "$PREFIX" && pwd)"

JAR="$PREFIX/antlr-$ANTLR_VERSION-complete.jar"
INSTALL="$PREFIX/runtime-install"
INC="$INSTALL/include/antlr4-runtime"
LIBDIR="$INSTALL/lib"

JAR_URL="https://www.antlr.org/download/antlr-$ANTLR_VERSION-complete.jar"
SRC_URL="https://www.antlr.org/download/antlr4-cpp-runtime-$ANTLR_VERSION-source.zip"

fetch() { # fetch URL OUT
    if command -v curl >/dev/null 2>&1; then curl -fsSL "$1" -o "$2"
    elif command -v wget >/dev/null 2>&1; then wget -q "$1" -O "$2"
    else echo "setup_antlr: need curl or wget" >&2; exit 1; fi
}

# 1) generator jar -----------------------------------------------------------
if [ ! -f "$JAR" ]; then
    echo "setup_antlr: downloading $JAR_URL" >&2
    fetch "$JAR_URL" "$JAR"
fi

# 2) C++ runtime (built + installed once, then cached) ------------------------
if [ ! -f "$LIBDIR/libantlr4-runtime.a" ]; then
    echo "setup_antlr: building ANTLR4 C++ runtime $ANTLR_VERSION" >&2
    work="$PREFIX/runtime-src"
    rm -rf "$work" && mkdir -p "$work"
    fetch "$SRC_URL" "$PREFIX/runtime-src.zip"
    unzip -q -o "$PREFIX/runtime-src.zip" -d "$work"
    cmake -S "$work" -B "$work/build" \
        -DCMAKE_BUILD_TYPE=Release \
        -DANTLR4_INSTALL=ON \
        -DANTLR_BUILD_CPP_TESTS=OFF \
        -DWITH_DEMO=OFF \
        -DCMAKE_INSTALL_PREFIX="$INSTALL" >/dev/null
    cmake --build "$work/build" --parallel >/dev/null
    cmake --install "$work/build" >/dev/null
fi

echo "ANTLR_JAR=$JAR"
echo "ANTLR_INC=$INC"
echo "ANTLR_LIBDIR=$LIBDIR"
