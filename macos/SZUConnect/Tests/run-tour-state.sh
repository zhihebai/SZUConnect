#!/bin/sh
# Pure Foundation guide tests. No UI, model, credentials or network services.
set -eu
HARNESS_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
if [ -f "$HARNESS_DIR/../Sources/Onboarding.swift" ]; then
    DEFAULT_ONBOARDING_SOURCE="$HARNESS_DIR/../Sources/Onboarding.swift"
else
    DEFAULT_ONBOARDING_SOURCE="$HARNESS_DIR/../source/macos/SZUConnect/Sources/Onboarding.swift"
fi
ONBOARDING_SOURCE=${1:-"$DEFAULT_ONBOARDING_SOURCE"}
TOUR_TEST_BUILD=$(mktemp -d "${TMPDIR:-/tmp}/szu-tour.XXXXXX")
trap 'rm -rf "$TOUR_TEST_BUILD"' EXIT HUP INT TERM
xcrun swiftc -parse-as-library -module-cache-path "$TOUR_TEST_BUILD/module-cache" \
  "$ONBOARDING_SOURCE" "$HARNESS_DIR/TourStateTests.swift" -o "$TOUR_TEST_BUILD/tour-state-tests"
"$TOUR_TEST_BUILD/tour-state-tests"
