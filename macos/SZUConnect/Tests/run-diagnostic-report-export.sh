#!/bin/sh
# Temporary fake snapshots and injected metadata only. No system crash source,
# application data, authentication, permissions or network access.
set -eu
TEST_ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
TEST_BUILD=$(mktemp -d "${TMPDIR:-/tmp}/szu-report-export.XXXXXX")
trap 'rm -rf "$TEST_BUILD"' EXIT HUP INT TERM
xcrun swiftc -parse-as-library -Osize -whole-module-optimization -warnings-as-errors \
  -module-cache-path "$TEST_BUILD/module-cache" \
  "$TEST_ROOT/Sources/DiagnosticReportExport.swift" "$TEST_ROOT/Tests/DiagnosticReportExportTests.swift" \
  -o "$TEST_BUILD/diagnostic-report-export-tests"
"$TEST_BUILD/diagnostic-report-export-tests"
