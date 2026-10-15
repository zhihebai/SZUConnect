#!/bin/sh
# In-memory fake radio only: never queries or changes the real Wi-Fi state.
set -eu
TEST_ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
TEST_BUILD=$(mktemp -d "${TMPDIR:-/tmp}/szu-wifi-recovery.XXXXXX")
trap 'rm -rf "$TEST_BUILD"' EXIT HUP INT TERM
xcrun swiftc -module-cache-path "$TEST_BUILD/module-cache" \
  "$TEST_ROOT/Sources/Generated/SharedCopy.swift" "$TEST_ROOT/Sources/WiFiRecovery.swift" "$TEST_ROOT/Tests/WiFiRecoveryTests.swift" \
  -o "$TEST_BUILD/wifi-recovery-tests"
"$TEST_BUILD/wifi-recovery-tests"
