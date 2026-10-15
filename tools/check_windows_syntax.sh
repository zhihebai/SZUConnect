#!/bin/sh
# Check platform and app C++ syntax with g++ and the declaration-only tools/winmock headers.
# This does not build a Windows application or validate Win32 API signatures and behaviour.
# Usage: sh tools/check_windows_syntax.sh
set -u
ROOT=$(cd "$(dirname "$0")/.." && pwd)
W="$ROOT/windows/SZUConnect"
CXX=${CXX:-g++}
total=0
failed=0
for f in "$W"/platform/src/*.cpp "$W"/app/*.cpp; do
  total=$((total + 1))
  if ! out=$("$CXX" -std=c++20 -fsyntax-only -fshort-wchar -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers -Wno-unknown-pragmas \
       -I"$ROOT/tools/winmock" -I"$W/core/include" -I"$W/core/generated" -I"$W/platform/include" -I"$W/app" -I"$W/resources" \
       -DUNICODE -D_UNICODE -DNOMINMAX -DWIN32_LEAN_AND_MEAN "$f" 2>&1); then
    failed=$((failed + 1))
    echo "== ${f#"$ROOT"/}"
    echo "$out" | grep -E 'error' | head -n "${SZU_MAX_ERRORS:-25}"
  elif [ -n "$out" ] && [ "${SZU_SHOW_WARNINGS:-0}" = 1 ]; then
    echo "== ${f#"$ROOT"/} (warnings)"; echo "$out" | grep -E 'warning' | head -n 20
  fi
done
echo "windows syntax check (mock SDK): $total files, $failed with errors"
[ "$failed" -eq 0 ]
