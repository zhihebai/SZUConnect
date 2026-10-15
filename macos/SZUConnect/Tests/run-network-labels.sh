#!/bin/sh
# Pure metadata and temporary-file persistence tests. Does not capture OS networking state.
set -eu
TEST_ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
TEST_BUILD=$(mktemp -d "${TMPDIR:-/tmp}/szu-network-labels.XXXXXX")
trap 'rm -rf "$TEST_BUILD"' EXIT HUP INT TERM
xcrun swiftc -parse-as-library -module-cache-path "$TEST_BUILD/module-cache" \
  "$TEST_ROOT/Sources/NetworkLabels.swift" "$TEST_ROOT/Tests/NetworkLabelsTests.swift" \
  -o "$TEST_BUILD/network-labels-tests"
"$TEST_BUILD/network-labels-tests"
