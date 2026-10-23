#!/bin/sh
# Offline geometry verification and optional Release microbenchmark; no app/system service starts.
set -eu
TEST_ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
TEST_BUILD=$(mktemp -d "${TMPDIR:-/tmp}/szu-icon-path.XXXXXX")
trap 'rm -rf "$TEST_BUILD"' EXIT HUP INT TERM
xcrun swiftc -O -whole-module-optimization -module-cache-path "$TEST_BUILD/module-cache" \
  "$TEST_ROOT/Sources/Generated/SharedIcons.swift" \
  "$TEST_ROOT/Sources/Generated/SharedTokens.swift" \
  "$TEST_ROOT/Sources/Generated/SharedCopy.swift" \
  "$TEST_ROOT/Sources/DesignSystem.swift" "$TEST_ROOT/Tests/IconPathGeometryTests.swift" \
  -o "$TEST_BUILD/icon-path-tests"
"$TEST_BUILD/icon-path-tests" "$@"
