#!/bin/sh
# Builds and runs the portable C++ core tests with any C++20 compiler (g++/clang++).
# Windows builds the same sources through CMake (see BUILD-WINDOWS.md).
set -eu
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../../.." && pwd)
CXX=${CXX:-g++}
# Default scratch belongs to this invocation, outside the source archive. An explicit OUT is caller-owned.
if [ -z "${OUT:-}" ]; then
    OUT=$(mktemp -d "${TMPDIR:-/tmp}/szu-core-tests.XXXXXX")
    trap 'rm -rf "$OUT"' EXIT HUP INT TERM
fi
mkdir -p "$OUT"
"$CXX" -std=c++20 -O1 -Wall -Wextra -Wpedantic -Werror \
  -I"$HERE/../core/include" -I"$HERE/../core/generated" \
  "$HERE"/../core/src/*.cpp "$HERE"/../core/generated/*.cpp "$HERE"/core_tests.cpp \
  -o "$OUT/szu_core_tests"
"$OUT/szu_core_tests" "$ROOT"
