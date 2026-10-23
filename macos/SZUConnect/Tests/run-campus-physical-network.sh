#!/bin/sh
# Offline evidence and DNS codec tests; never changes or samples networking.
set -eu
TEST_ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
TEST_BUILD=$(mktemp -d "${TMPDIR:-/tmp}/szu-campus-physical.XXXXXX")
trap 'rm -rf "$TEST_BUILD"' EXIT HUP INT TERM
xcrun swiftc -module-cache-path "$TEST_BUILD/module-cache" \
  "$TEST_ROOT/Sources/Generated/SharedCopy.swift" "$TEST_ROOT/Sources/NetworkLabels.swift" "$TEST_ROOT/Sources/NetworkIdentity.swift" \
  "$TEST_ROOT/Sources/NetworkDescriptor.swift" "$TEST_ROOT/Sources/CampusPhysicalNetwork.swift" \
  "$TEST_ROOT/Tests/CampusPhysicalNetworkTests.swift" -o "$TEST_BUILD/campus-physical-tests"
"$TEST_BUILD/campus-physical-tests"
