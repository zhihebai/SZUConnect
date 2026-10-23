#!/bin/bash
# Compile only. No tests, code signing, installation, launch, or network configuration.
set -euo pipefail
ROOT=$(cd -- "$(dirname -- "$0")/.." && pwd)
OUT="${SZU_COMPILE_OUTPUT_DIR:-$ROOT/build/mac-compile-only}"
mkdir -p "$OUT"
# A failed retry must not retain a previous success marker.
if [[ -e "$OUT/APP-PATH.txt" ]]; then rm -- "$OUT/APP-PATH.txt"; fi
if [[ -n "${SZU_DERIVED_DATA_DIR:-}" ]]; then
    DERIVED="$SZU_DERIVED_DATA_DIR"
else
    DERIVED=$(mktemp -d /private/tmp/SZUConnect-MacCompile.XXXXXX)
fi
mkdir -p "$DERIVED"
printf '%s\n' "$DERIVED" > "$OUT/DERIVED-DATA-PATH.txt"
if xcodebuild \
    -project "$ROOT/SZUConnect.xcodeproj" \
    -scheme SZUConnect-macOS \
    -configuration Release \
    -destination 'generic/platform=macOS' \
    -derivedDataPath "$DERIVED" \
    -disableAutomaticPackageResolution \
    ONLY_ACTIVE_ARCH=NO \
    CODE_SIGNING_ALLOWED=NO \
    CODE_SIGNING_REQUIRED=NO \
    build > "$OUT/build.log" 2>&1; then
    APP="$DERIVED/Build/Products/Release/SZUConnect.app"
    test -d "$APP"
    # Build actions can retain object-file paths in the linker debug map.
    # Keep debug maps and local symbols out of the unsigned release product.
    # Global/dynamically bound symbols and Swift runtime metadata are retained.
    xcrun strip -S -x "$APP/Contents/MacOS/SZUConnect"
    printf '%s\n' "$APP" > "$OUT/APP-PATH.txt"
    printf 'Mac Release compile succeeded. Unsigned product: %s\nLog: %s\n' "$APP" "$OUT/build.log"
else
    result=$?
    tail -n 80 "$OUT/build.log" >&2
    printf 'Compile failed. Log: %s\n' "$OUT/build.log" >&2
    exit "$result"
fi
