#!/bin/sh
# Exercise the production ribbon-selection value without opening an app or reading user history.
set -eu
TEST_ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
TEST_BUILD=$(mktemp -d "${TMPDIR:-/tmp}/szu-record-selection.XXXXXX")
trap 'rm -rf "$TEST_BUILD"' EXIT HUP INT TERM
# The Foundation-only value precedes the SwiftUI page in the same source file.
sed '/^import SwiftUI$/,$d' "$TEST_ROOT/Sources/RecordsPage.swift" > "$TEST_BUILD/RecordSampleSelection.swift"
xcrun swiftc -module-cache-path "$TEST_BUILD/module-cache" \
  "$TEST_BUILD/RecordSampleSelection.swift" "$TEST_ROOT/Tests/RecordSampleSelectionTests.swift" \
  -o "$TEST_BUILD/record-selection-tests"
"$TEST_BUILD/record-selection-tests"
