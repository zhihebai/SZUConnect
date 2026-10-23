#!/bin/sh
# Pure transient presentation state: no credentials, preferences, network, or system permissions.
set -eu
TEST_ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
TEST_BUILD=$(mktemp -d "${TMPDIR:-/tmp}/szu-form-disclosure.XXXXXX")
trap 'rm -rf "$TEST_BUILD"' EXIT HUP INT TERM
xcrun swiftc -parse-as-library -Osize -whole-module-optimization -warnings-as-errors \
  -module-cache-path "$TEST_BUILD/module-cache" \
  "$TEST_ROOT/Sources/ConnectionFormDisclosureState.swift" "$TEST_ROOT/Tests/ConnectionFormDisclosureTests.swift" \
  -o "$TEST_BUILD/form-disclosure-tests"
"$TEST_BUILD/form-disclosure-tests"
