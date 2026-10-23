#!/bin/sh
set -eu
TEST_ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
TEST_BUILD=$(mktemp -d "${TMPDIR:-/tmp}/szu-event-store.XXXXXX")
trap 'rm -rf "$TEST_BUILD"' EXIT HUP INT TERM
xcrun swiftc "$TEST_ROOT/Sources/Generated/SharedCopy.swift" "$TEST_ROOT/Sources/DiagnosticContext.swift" "$TEST_ROOT/Sources/EventLogFile.swift" "$TEST_ROOT/Tests/EventLogFileTests.swift" -o "$TEST_BUILD/event-store-tests"
"$TEST_BUILD/event-store-tests"
