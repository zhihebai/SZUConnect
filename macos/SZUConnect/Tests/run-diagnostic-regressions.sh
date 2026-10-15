#!/bin/sh
set -eu
TEST_ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
TEST_BUILD=$(mktemp -d "${TMPDIR:-/tmp}/szu-diagnostic.XXXXXX")
trap 'rm -rf "$TEST_BUILD"' EXIT HUP INT TERM
xcrun swiftc -module-cache-path "$TEST_BUILD/module-cache" \
 "$TEST_ROOT/Sources/NetworkDescriptor.swift" "$TEST_ROOT/Sources/WebsiteDiagnostics.swift" "$TEST_ROOT/Sources/ProxyEnvironment.swift" \
 "$TEST_ROOT/Tests/DiagnosticRegressionTests.swift" -o "$TEST_BUILD/tests"
"$TEST_BUILD/tests"
