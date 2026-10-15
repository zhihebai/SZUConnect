#!/bin/sh
# Every lock and child process uses a temporary test cache, never the app cache.
set -eu
TEST_ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
TEST_BUILD=$(mktemp -d "${TMPDIR:-/tmp}/szu-single-instance.XXXXXX")
trap 'rm -rf "$TEST_BUILD"' EXIT HUP INT TERM
xcrun swiftc -parse-as-library -module-cache-path "$TEST_BUILD/module-cache" \
  "$TEST_ROOT/Sources/SingleInstance.swift" "$TEST_ROOT/Tests/SingleInstanceTests.swift" \
  -o "$TEST_BUILD/single-instance-tests"
"$TEST_BUILD/single-instance-tests"
