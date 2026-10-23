#!/bin/sh
# Every URL is intercepted by a test URLProtocol. No app or real network is used.
set -eu
TEST_ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
TEST_BUILD=$(mktemp -d "${TMPDIR:-/tmp}/szu-website.XXXXXX")
trap 'rm -rf "$TEST_BUILD"' EXIT HUP INT TERM
xcrun swiftc -module-cache-path "$TEST_BUILD/module-cache" \
  "$TEST_ROOT/Sources/Generated/SharedCopy.swift" "$TEST_ROOT/Sources/NetworkDescriptor.swift" "$TEST_ROOT/Sources/WebsiteDiagnostics.swift" "$TEST_ROOT/Tests/WebsiteDiagnosticsTests.swift" \
  -o "$TEST_BUILD/website-tests"
"$TEST_BUILD/website-tests"
