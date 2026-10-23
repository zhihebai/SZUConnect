#!/bin/sh
# Hidden AppKit windows only; termination is intercepted and no model/data service is constructed.
set -eu
TEST_ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
TEST_BUILD=$(mktemp -d "${TMPDIR:-/tmp}/szu-uninstall-lifecycle.XXXXXX")
trap 'rm -rf "$TEST_BUILD"' EXIT HUP INT TERM
xcrun swiftc -warnings-as-errors -module-cache-path "$TEST_BUILD/module-cache" \
  "$TEST_ROOT/Sources/AppUninstallLifecycle.swift" "$TEST_ROOT/Tests/AppUninstallLifecycleTests.swift" \
  -o "$TEST_BUILD/app-uninstall-lifecycle-tests"
"$TEST_BUILD/app-uninstall-lifecycle-tests"
