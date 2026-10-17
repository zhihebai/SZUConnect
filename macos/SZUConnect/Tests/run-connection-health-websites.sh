#!/bin/sh
# Pure in-memory ConnectionHealth migration and aggregate tests.
# Never launches the app, reads user data, or sends network requests.
set -eu
TEST_ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
TEST_BUILD=$(mktemp -d "${TMPDIR:-/tmp}/szu-health-websites.XXXXXX")
trap 'rm -rf "$TEST_BUILD"' EXIT HUP INT TERM
xcrun clang++ -std=c++17 -c "$TEST_ROOT/Sources/SrunCore.cpp" -o "$TEST_BUILD/core.o"
xcrun clang++ -std=c++17 -c "$TEST_ROOT/Sources/CampusPhysicalCurl.cpp" -o "$TEST_BUILD/physical-curl.o"
xcrun swiftc -module-cache-path "$TEST_BUILD/module-cache" \
  -import-objc-header "$TEST_ROOT/Sources/SrunCore.h" \
  "$TEST_ROOT/Sources/Generated/SharedCopy.swift" "$TEST_ROOT/Sources/PortalClient.swift" "$TEST_ROOT/Sources/RecoveryHolds.swift" "$TEST_ROOT/Sources/NetworkDescriptor.swift" \
  "$TEST_ROOT/Sources/ConnectionHealth.swift" \
  "$TEST_ROOT/Tests/ConnectionHealthWebsiteAggregationTests.swift" \
  "$TEST_ROOT/Sources/NetworkLabels.swift" "$TEST_ROOT/Sources/NetworkIdentity.swift" "$TEST_ROOT/Sources/CampusPhysicalNetwork.swift" "$TEST_ROOT/Sources/CampusPhysicalTransport.swift" \
  "$TEST_BUILD/core.o" "$TEST_BUILD/physical-curl.o" -lcurl -Xlinker -lc++ -o "$TEST_BUILD/connection-health-websites-tests"
"$TEST_BUILD/connection-health-websites-tests"
