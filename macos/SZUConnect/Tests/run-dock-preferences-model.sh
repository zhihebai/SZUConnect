#!/bin/sh
# Dock preferences use an isolated temporary preference domain; no credential reads or network calls.
set -eu
TEST_ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
TEST_BUILD=$(mktemp -d "${TMPDIR:-/tmp}/szu-dock-preferences.XXXXXX")
trap 'rm -rf "$TEST_BUILD"' EXIT HUP INT TERM
xcrun clang++ -std=c++17 -c "$TEST_ROOT/Sources/SrunCore.cpp" -o "$TEST_BUILD/core.o"
xcrun clang++ -std=c++17 -c "$TEST_ROOT/Sources/CampusPhysicalCurl.cpp" -o "$TEST_BUILD/physical-curl.o"
xcrun swiftc -module-cache-path "$TEST_BUILD/module-cache" \
  -import-objc-header "$TEST_ROOT/Sources/SrunCore.h" \
  "$TEST_ROOT/Sources/NetworkLabels.swift" "$TEST_ROOT/Sources/NetworkIdentity.swift" "$TEST_ROOT/Sources/NetworkDescriptor.swift" "$TEST_ROOT/Sources/ConnectionPolicy.swift" "$TEST_ROOT/Sources/ConnectionHealth.swift" "$TEST_ROOT/Sources/PortalClient.swift" "$TEST_ROOT/Sources/RecoveryHolds.swift" \
  "$TEST_ROOT/Sources/EventLogFile.swift" "$TEST_ROOT/Sources/WebsiteDiagnostics.swift" "$TEST_ROOT/Sources/InternetCheck.swift" "$TEST_ROOT/Sources/WebsitePreferences.swift" "$TEST_ROOT/Sources/Generated/SharedCopy.swift" "$TEST_ROOT/Sources/DiagnosticContext.swift" "$TEST_ROOT/Sources/WiFiRecovery.swift" "$TEST_ROOT/Sources/AppModel.swift" "$TEST_ROOT/Sources/NetworkMemory.swift" "$TEST_ROOT/Tests/DockPreferencesAppModelTests.swift" \
  "$TEST_ROOT/Sources/CampusPhysicalNetwork.swift" "$TEST_ROOT/Sources/CampusPhysicalTransport.swift" \
  "$TEST_BUILD/core.o" "$TEST_BUILD/physical-curl.o" -lcurl -Xlinker -lc++ -o "$TEST_BUILD/dock-preference-tests"
"$TEST_BUILD/dock-preference-tests"
