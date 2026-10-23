#!/bin/sh
# Native disposable bundle only; no installed app, preferences, network, or system service.
set -eu
TEST_ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
TEST_BUILD=$(mktemp -d "${TMPDIR:-/tmp}/szu-open-source-licenses.XXXXXX")
trap 'rm -rf "$TEST_BUILD"' EXIT HUP INT TERM
TEST_APP="$TEST_BUILD/LicenseTests.app"
mkdir -p "$TEST_APP/Contents/MacOS" "$TEST_APP/Contents/Resources"
cat > "$TEST_APP/Contents/Info.plist" <<'PLIST'
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0"><dict>
<key>CFBundleIdentifier</key><string>local.SZUConnect.tests.licenses</string>
<key>CFBundleExecutable</key><string>license-tests</string>
<key>CFBundlePackageType</key><string>APPL</string>
</dict></plist>
PLIST
for name in LICENSE AutoSZUWeb-LICENSE.txt curl-COPYING.txt; do
    printf 'fixture %s\n' "$name" > "$TEST_APP/Contents/Resources/$name"
done
xcrun swiftc -parse-as-library -O -warnings-as-errors -module-cache-path "$TEST_BUILD/module-cache" \
  "$TEST_ROOT/Sources/Generated/SharedIcons.swift" \
  "$TEST_ROOT/Sources/Generated/SharedTokens.swift" \
  "$TEST_ROOT/Sources/Generated/SharedCopy.swift" \
  "$TEST_ROOT/Sources/DesignSystem.swift" \
  "$TEST_ROOT/Sources/OpenSourceLicenses.swift" "$TEST_ROOT/Tests/OpenSourceLicensesTests.swift" \
  -o "$TEST_APP/Contents/MacOS/license-tests"
"$TEST_APP/Contents/MacOS/license-tests"
