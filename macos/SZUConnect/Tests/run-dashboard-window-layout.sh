#!/bin/sh
# Native hidden windows only; no app preferences, credentials, permissions, or network.
set -eu
TEST_ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
TEST_BUILD=$(mktemp -d "${TMPDIR:-/tmp}/szu-dashboard-layout.XXXXXX")
trap 'rm -rf "$TEST_BUILD"' EXIT HUP INT TERM
xcrun swiftc -parse-as-library -module-cache-path "$TEST_BUILD/module-cache" \
  "$TEST_ROOT/Sources/DashboardLayout.swift" "$TEST_ROOT/Tests/DashboardWindowLayoutTests.swift" \
  -o "$TEST_BUILD/dashboard-layout-tests"
"$TEST_BUILD/dashboard-layout-tests"
