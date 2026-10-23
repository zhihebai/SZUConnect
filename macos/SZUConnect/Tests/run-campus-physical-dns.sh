#!/bin/sh
# Deterministic UDP loss, error-response, total-budget and cancellation tests. No network I/O.
set -eu
TEST_ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
TEST_BUILD=$(mktemp -d "${TMPDIR:-/tmp}/szu-campus-dns.XXXXXX")
trap 'rm -rf "$TEST_BUILD"' EXIT HUP INT TERM
xcrun swiftc -module-cache-path "$TEST_BUILD/module-cache" \
  "$TEST_ROOT/Sources/Generated/SharedCopy.swift" "$TEST_ROOT/Sources/NetworkLabels.swift" "$TEST_ROOT/Sources/NetworkIdentity.swift" \
  "$TEST_ROOT/Sources/NetworkDescriptor.swift" "$TEST_ROOT/Sources/CampusPhysicalNetwork.swift" \
  "$TEST_ROOT/Tests/CampusPhysicalDNSTests.swift" -o "$TEST_BUILD/campus-dns-tests"
"$TEST_BUILD/campus-dns-tests"
