#!/bin/sh
# Native typography and in-memory disclosure choices only; no app data or network access.
set -eu
TEST_ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
TEST_BUILD=$(mktemp -d "${TMPDIR:-/tmp}/szu-help-presentation.XXXXXX")
trap 'rm -rf "$TEST_BUILD"' EXIT HUP INT TERM
xcrun swiftc -parse-as-library -Osize -whole-module-optimization -warnings-as-errors \
  -module-cache-path "$TEST_BUILD/module-cache" \
  "$TEST_ROOT/Sources/HelpDisclosureState.swift" "$TEST_ROOT/Tests/HelpPresentationTests.swift" \
  -o "$TEST_BUILD/help-presentation-tests"
"$TEST_BUILD/help-presentation-tests" "$TEST_ROOT/../../shared/copy.zh-CN.json"
