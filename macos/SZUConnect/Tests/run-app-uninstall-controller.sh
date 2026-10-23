#!/bin/sh
set -eu
TEST_ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
TEST_BUILD=$(mktemp -d "${TMPDIR:-/tmp}/szu-uninstall-controller.XXXXXX")
trap 'rm -rf "$TEST_BUILD"' EXIT HUP INT TERM
xcrun swiftc -warnings-as-errors -module-cache-path "$TEST_BUILD/module-cache" \
  "$TEST_ROOT/Sources/Generated/SharedCopy.swift" "$TEST_ROOT/Sources/AppUninstall.swift" \
  "$TEST_ROOT/Sources/AppUninstallController.swift" "$TEST_ROOT/Tests/AppUninstallControllerTests.swift" \
  -o "$TEST_BUILD/controller-tests"
"$TEST_BUILD/controller-tests"
