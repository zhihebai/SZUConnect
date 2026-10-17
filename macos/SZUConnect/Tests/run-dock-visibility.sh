#!/bin/sh
# Pure lifecycle decisions only; no windows, OS permissions or network requests.
set -eu
TEST_ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
TEST_BUILD=$(mktemp -d "/private/tmp/szu-dock-visibility.XXXXXX")
trap 'rm -rf "$TEST_BUILD"' EXIT HUP INT TERM
xcrun swiftc -parse-as-library -module-cache-path "$TEST_BUILD/module-cache" \
  "$TEST_ROOT/Sources/DockVisibility.swift" "$TEST_ROOT/Tests/DockVisibilityTests.swift" \
  -o "$TEST_BUILD/dock-tests"
"$TEST_BUILD/dock-tests"
