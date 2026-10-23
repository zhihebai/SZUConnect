#!/bin/sh
# Native detached layout and pure result projection only; no real configuration or network access.
set -eu
TEST_ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
TEST_BUILD=$(mktemp -d "${TMPDIR:-/tmp}/szu-proxy-assistant-presentation.XXXXXX")
trap 'rm -rf "$TEST_BUILD"' EXIT HUP INT TERM
xcrun swiftc -parse-as-library -Osize -whole-module-optimization -warnings-as-errors \
  -module-cache-path "$TEST_BUILD/module-cache" \
  "$TEST_ROOT/Sources/Generated/SharedCopy.swift" "$TEST_ROOT/Sources/Generated/SharedTokens.swift" \
  "$TEST_ROOT/Sources/Generated/SharedIcons.swift" "$TEST_ROOT/Sources/DesignSystem.swift" \
  "$TEST_ROOT/Sources/ProxyAssistantPresentation.swift" "$TEST_ROOT/Tests/ProxyAssistantPresentationTests.swift" \
  -o "$TEST_BUILD/proxy-assistant-presentation-tests"
"$TEST_BUILD/proxy-assistant-presentation-tests"
