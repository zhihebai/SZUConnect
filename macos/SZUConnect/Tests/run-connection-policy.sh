#!/bin/sh
# Executes pure connection policy only; no app or system service is instantiated.
set -eu
TEST_ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
TEST_BUILD=$(mktemp -d "${TMPDIR:-/tmp}/szu-policy.XXXXXX")
trap 'rm -rf "$TEST_BUILD"' EXIT HUP INT TERM
# Compile ConnectionPolicy with scoped holds and the PortalClient types they reference.
xcrun clang++ -std=c++17 -c "$TEST_ROOT/Sources/SrunCore.cpp" -o "$TEST_BUILD/core.o"
xcrun clang++ -std=c++17 -c "$TEST_ROOT/Sources/CampusPhysicalCurl.cpp" -o "$TEST_BUILD/physical-curl.o"
xcrun swiftc -module-cache-path "$TEST_BUILD/module-cache" -import-objc-header "$TEST_ROOT/Sources/SrunCore.h" \
  "$TEST_ROOT/Sources/PortalClient.swift" "$TEST_ROOT/Sources/RecoveryHolds.swift" "$TEST_ROOT/Sources/NetworkDescriptor.swift" \
  "$TEST_ROOT/Sources/ConnectionPolicy.swift" "$TEST_ROOT/Tests/ConnectionPolicyTests.swift" \
  "$TEST_ROOT/Sources/NetworkLabels.swift" "$TEST_ROOT/Sources/NetworkIdentity.swift" "$TEST_ROOT/Sources/CampusPhysicalNetwork.swift" "$TEST_ROOT/Sources/CampusPhysicalTransport.swift" \
  "$TEST_BUILD/core.o" "$TEST_BUILD/physical-curl.o" -lcurl -Xlinker -lc++ -o "$TEST_BUILD/policy-tests"
"$TEST_BUILD/policy-tests"
