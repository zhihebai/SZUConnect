#!/bin/sh
# In-memory presentation state only; no application data, system settings or network access.
set -eu
TEST_ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
TEST_BUILD=$(mktemp -d "${TMPDIR:-/tmp}/szu-network-details-presentation.XXXXXX")
trap 'rm -rf "$TEST_BUILD"' EXIT HUP INT TERM
xcrun swiftc -parse-as-library -Osize -whole-module-optimization -warnings-as-errors \
  -module-cache-path "$TEST_BUILD/module-cache" \
  "$TEST_ROOT/Sources/NetworkDetailsPresentationState.swift" "$TEST_ROOT/Tests/NetworkDetailsPresentationTests.swift" \
  -o "$TEST_BUILD/network-details-presentation-tests"
"$TEST_BUILD/network-details-presentation-tests"
