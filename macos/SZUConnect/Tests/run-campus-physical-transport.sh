#!/bin/sh
# Offline fixtures and loopback sockets only; no external requests, accounts or app launch.
set -eu
TEST_ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
TEST_BUILD=$(mktemp -d "${TMPDIR:-/tmp}/szu-physical-transport.XXXXXX")
trap 'rm -rf "$TEST_BUILD"' EXIT HUP INT TERM
xcrun clang++ -std=c++17 -Wall -Wextra -DSZU_CAMPUS_CURL_TESTING \
  -c "$TEST_ROOT/Sources/CampusPhysicalCurl.cpp" -o "$TEST_BUILD/physical.o"
xcrun clang++ -std=c++17 -Wall -Wextra -DSZU_CAMPUS_CURL_TESTING \
  "$TEST_ROOT/Tests/CampusPhysicalSocketTests.cpp" "$TEST_BUILD/physical.o" \
  -lcurl -o "$TEST_BUILD/physical-socket-tests"
"$TEST_BUILD/physical-socket-tests"
xcrun swiftc -module-cache-path "$TEST_BUILD/module-cache" \
  -Xcc -DSZU_CAMPUS_CURL_TESTING -import-objc-header "$TEST_ROOT/Sources/CampusPhysicalCurl.h" \
  "$TEST_ROOT/Sources/NetworkLabels.swift" "$TEST_ROOT/Sources/NetworkIdentity.swift" "$TEST_ROOT/Sources/NetworkDescriptor.swift" \
  "$TEST_ROOT/Sources/CampusPhysicalNetwork.swift" "$TEST_ROOT/Sources/CampusPhysicalTransport.swift" \
  "$TEST_ROOT/Tests/CampusPhysicalTransportTests.swift" "$TEST_BUILD/physical.o" \
  -Xlinker -lc++ -Xlinker -lcurl -o "$TEST_BUILD/physical-transport-tests"
"$TEST_BUILD/physical-transport-tests"
