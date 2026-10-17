#!/bin/sh
# Contract tests using Xcode command line tools. No network or Keychain access.
set -eu
TEST_ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
REPO_ROOT=$(CDPATH= cd -- "$TEST_ROOT/../.." && pwd)
TEST_BUILD=$(mktemp -d "${TMPDIR:-/tmp}/szu-shared-contract.XXXXXX")
trap 'rm -rf "$TEST_BUILD"' EXIT HUP INT TERM
xcrun clang++ -std=c++17 -c "$TEST_ROOT/Sources/SrunCore.cpp" -o "$TEST_BUILD/core.o"
xcrun clang++ -std=c++17 -c "$TEST_ROOT/Sources/CampusPhysicalCurl.cpp" -o "$TEST_BUILD/physical-curl.o"
xcrun swiftc -parse-as-library -module-cache-path "$TEST_BUILD/module-cache" -import-objc-header "$TEST_ROOT/Sources/SrunCore.h" \
  "$TEST_ROOT/Sources/Generated/SharedCopy.swift" "$TEST_ROOT/Sources/PortalClient.swift" "$TEST_ROOT/Sources/RecoveryHolds.swift" "$TEST_ROOT/Sources/NetworkDescriptor.swift" "$TEST_ROOT/Sources/StatusPresenter.swift" "$TEST_ROOT/Sources/ProxyEnvironment.swift" "$TEST_ROOT/Sources/WebsiteDiagnostics.swift" "$TEST_ROOT/Tests/SharedContractTests.swift" \
  "$TEST_ROOT/Sources/NetworkLabels.swift" "$TEST_ROOT/Sources/NetworkIdentity.swift" "$TEST_ROOT/Sources/CampusPhysicalNetwork.swift" "$TEST_ROOT/Sources/CampusPhysicalTransport.swift" \
  "$TEST_BUILD/core.o" "$TEST_BUILD/physical-curl.o" -lcurl -Xlinker -lc++ -o "$TEST_BUILD/tests"
"$TEST_BUILD/tests" "$REPO_ROOT"
