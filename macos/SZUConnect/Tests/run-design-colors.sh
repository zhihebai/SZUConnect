#!/bin/sh
# Resolve cached colors in both appearances without opening an app or changing system settings.
set -eu
TEST_ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
TEST_BUILD=$(mktemp -d "${TMPDIR:-/tmp}/szu-design-colors.XXXXXX")
trap 'rm -rf "$TEST_BUILD"' EXIT HUP INT TERM
xcrun swiftc -O -whole-module-optimization -module-cache-path "$TEST_BUILD/module-cache" \
  "$TEST_ROOT/Sources/Generated/SharedIcons.swift" \
  "$TEST_ROOT/Sources/Generated/SharedTokens.swift" \
  "$TEST_ROOT/Sources/Generated/SharedCopy.swift" \
  "$TEST_ROOT/Sources/DesignSystem.swift" "$TEST_ROOT/Tests/DesignTokenColorTests.swift" \
  -o "$TEST_BUILD/design-color-tests"
"$TEST_BUILD/design-color-tests"
