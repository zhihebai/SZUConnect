#!/bin/sh
# Network memory and descriptor fixtures. No network, Keychain or OS network-state access.
set -eu
TEST_ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
TEST_BUILD=$(mktemp -d "${TMPDIR:-/tmp}/szu-network-memory.XXXXXX")
trap 'rm -rf "$TEST_BUILD"' EXIT HUP INT TERM
xcrun swiftc -parse-as-library -module-cache-path "$TEST_BUILD/module-cache" \
  "$TEST_ROOT/Sources/Generated/SharedCopy.swift" "$TEST_ROOT/Sources/NetworkDescriptor.swift" "$TEST_ROOT/Sources/NetworkMemory.swift" "$TEST_ROOT/Sources/NetworkLabels.swift" "$TEST_ROOT/Sources/NetworkIdentity.swift" "$TEST_ROOT/Sources/CampusPhysicalNetwork.swift" \
  "$TEST_ROOT/Tests/NetworkMemoryTests.swift" -o "$TEST_BUILD/network-memory-tests"
"$TEST_BUILD/network-memory-tests"
