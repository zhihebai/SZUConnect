#!/bin/sh
# Generated public preset metadata and local presentation state only.
set -eu
TEST_ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
TEST_BUILD=$(mktemp -d "${TMPDIR:-/tmp}/szu-website-presets-presentation.XXXXXX")
trap 'rm -rf "$TEST_BUILD"' EXIT HUP INT TERM
xcrun swiftc -parse-as-library -Osize -whole-module-optimization -warnings-as-errors \
  -module-cache-path "$TEST_BUILD/module-cache" \
  "$TEST_ROOT/Sources/Generated/SharedCopy.swift" "$TEST_ROOT/Sources/WebsitePresetPresentation.swift" \
  "$TEST_ROOT/Tests/WebsitePresetPresentationTests.swift" -o "$TEST_BUILD/website-preset-presentation-tests"
"$TEST_BUILD/website-preset-presentation-tests"
