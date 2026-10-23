#!/bin/sh
# Only temporary fake applications, simulated credentials/login items, and an isolated preference domain.
set -eu
TEST_ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
TEST_BUILD=$(mktemp -d "${TMPDIR:-/tmp}/szu-app-uninstall.XXXXXX")
trap 'rm -rf "$TEST_BUILD"' EXIT HUP INT TERM
xcrun swiftc -warnings-as-errors -module-cache-path "$TEST_BUILD/module-cache" \
  "$TEST_ROOT/Sources/Generated/SharedCopy.swift" "$TEST_ROOT/Sources/AppUninstall.swift" "$TEST_ROOT/Tests/AppUninstallTests.swift" \
  -o "$TEST_BUILD/app-uninstall-tests"
"$TEST_BUILD/app-uninstall-tests"
