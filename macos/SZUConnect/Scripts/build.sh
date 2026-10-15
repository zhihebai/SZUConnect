#!/bin/bash
# Build/package only. Never installs, launches, authenticates, or changes networking.
set -Eeuo pipefail
umask 077
export PYTHONDONTWRITEBYTECODE=1
ROOT=$(cd -- "$(dirname -- "$0")/.." && pwd)
mkdir -p -- "${SZU_OUTPUT_DIR:-$ROOT/build}"
DEST=$(cd -- "${SZU_OUTPUT_DIR:-$ROOT/build}" && pwd)
[[ "$DEST" != / && "$DEST" != "$ROOT" ]] || { echo 'Unsafe output directory' >&2; exit 2; }
# Serialize writers; a stale lock requires inspection after an untrappable kill.
mkdir "$DEST/.szu-build-lock" || { echo 'Output directory is locked' >&2; exit 2; }
SCRATCH=$(mktemp -d /private/tmp/szu-build.XXXXXX) || { rmdir "$DEST/.szu-build-lock"; exit 1; }
STAGE="$SCRATCH/artifacts"
mkdir "$STAGE"
export TMPDIR="$SCRATCH/tmp" CLANG_MODULE_CACHE_PATH="$SCRATCH/clang-cache" SWIFT_MODULECACHE_PATH="$SCRATCH/swift-cache"
mkdir "$TMPDIR"
RUN_ID="$(date -u +%Y%m%dT%H%M%SZ)-$$"
IOS_KIND=ios
MAC_MODE=adhoc
[[ -z "${SZU_TEAM:-}" ]] || IOS_KIND=signed-ios
[[ -z "${SZU_SIGN_IDENTITY:-}" || "$SZU_SIGN_IDENTITY" == - ]] || MAC_MODE=certificate

manifest() {
    python3 - "$ROOT" "$STAGE" "$1" "$RUN_ID" "$IOS_KIND" "$MAC_MODE" "${SZU_TEAM:-}" <<'PY'
import datetime, json, sys
from pathlib import Path
sys.path.insert(0, str(Path(sys.argv[1]) / 'Scripts'))
from verify_artifacts import ARTIFACTS, source, sha256, write_json
root, stage = map(Path, sys.argv[1:3])
status, run_id, ios_kind, mac_mode, team = sys.argv[3:]
path = stage / 'build-manifest.json'
data = json.loads(path.read_text()) if path.exists() else {
    'schemaVersion': 1, 'runID': run_id, 'source': source(root), 'team': team,
    'configuration': 'Release', 'startedUTC': datetime.datetime.now(datetime.timezone.utc).isoformat()}
data.update(status=status, artifacts=[], logs=sorted(p.name for p in stage.glob('*.log')))
if status == 'success':
    for kind, signature in [('mac', mac_mode), (ios_kind, 'development' if ios_kind == 'signed-ios' else 'unsigned'), ('sim', 'unsigned')]:
        data['artifacts'].append({'kind': kind, 'signature': signature, 'sha256': sha256(stage / ARTIFACTS[kind][0])})
if status != 'running':
    data['finishedUTC'] = datetime.datetime.now(datetime.timezone.utc).isoformat()
write_json(path, data)
PY
}

publish_record() {
    cp "$STAGE/build-manifest.json" "$DEST/.build-manifest.$RUN_ID.tmp"
    mv -f "$DEST/.build-manifest.$RUN_ID.tmp" "$DEST/build-manifest.json"
}
finish() {
    result=$?
    trap - EXIT
    set +e
    if [[ $result -ne 0 ]]; then
        # The destination manifest remains non-successful even after a partial copy.
        manifest failed || printf '{"schemaVersion":1,"status":"failed","runID":"%s"}\n' "$RUN_ID" > "$STAGE/build-manifest.json"
        publish_record
        rm -f "$DEST"/SZUConnect-{macOS.zip,iOS-Development.ipa,iOS-Unsigned.zip,iOS-Simulator.zip} "$DEST/artifact-verification.json"
        for log in "$STAGE"/*.log; do [[ ! -f "$log" ]] || cp "$log" "$DEST/"; done
        echo "Build failed. Diagnostics: $DEST; staging retained: $SCRATCH" >&2
    else
        rm -rf "$SCRATCH"
        echo "Verified Release artifacts: $DEST (no app launched)"
    fi
    rm -f "$DEST/.build-manifest.$RUN_ID.tmp"
    rmdir "$DEST/.szu-build-lock"
    exit "$result"
}
trap finish EXIT
trap 'exit 130' INT
trap 'exit 143' TERM HUP
# Invalidate success before doing any work. Known archives are removed, never unrelated files.
printf '{"schemaVersion":1,"status":"running","runID":"%s"}\n' "$RUN_ID" > "$STAGE/build-manifest.json"
publish_record
rm "$STAGE/build-manifest.json"
manifest running
publish_record
for name in SZUConnect-macOS.zip SZUConnect-iOS-Development.ipa SZUConnect-iOS-Unsigned.zip SZUConnect-iOS-Simulator.zip artifact-verification.json iOS-signing.json; do
    rm -f -- "$DEST/$name"
done
run_logged() { local log=$1; shift; "$@" > "$STAGE/$log.log" 2>&1; }
COMMON=(-project "$ROOT/SZUConnect.xcodeproj" -configuration Release ONLY_ACTIVE_ARCH=NO)
MAC_SIGN=(CODE_SIGN_IDENTITY=-)
IOS_SIGN=(CODE_SIGNING_ALLOWED=NO CODE_SIGNING_REQUIRED=NO)
if [[ -n "${SZU_SIGN_IDENTITY:-}" ]]; then
    MAC_SIGN=(CODE_SIGN_STYLE=Manual "CODE_SIGN_IDENTITY=$SZU_SIGN_IDENTITY" "DEVELOPMENT_TEAM=${SZU_TEAM:-}")
fi
if [[ "$IOS_KIND" == signed-ios ]]; then
    IOS_SIGN=(CODE_SIGNING_ALLOWED=YES CODE_SIGNING_REQUIRED=YES CODE_SIGN_STYLE=Automatic CODE_SIGN_IDENTITY="Apple Development" "DEVELOPMENT_TEAM=$SZU_TEAM")
fi
run_logged offline-tests sh "$ROOT/Tests/run-offline.sh"
if [[ -f "$ROOT/Tests/run-connection-policy.sh" ]]; then
    run_logged connection-policy-tests sh "$ROOT/Tests/run-connection-policy.sh"
fi
run_logged transport-tests sh "$ROOT/Tests/run-transport.sh"
run_logged website-tests sh "$ROOT/Tests/run-website-diagnostics.sh"
run_logged network-identity-tests sh "$ROOT/Tests/run-network-identity.sh"
run_logged network-memory-tests sh "$ROOT/Tests/run-network-memory.sh"
run_logged memory-plan-tests sh "$ROOT/Tests/run-memory-plan.sh"
run_logged health-tests sh "$ROOT/Tests/run-health.sh"
run_logged event-store-tests sh "$ROOT/Tests/run-event-store.sh"
run_logged app-model-tests sh "$ROOT/Tests/run-app-model.sh"
# Additional regression suites; optional only for isolated artifact-script fixtures.
for suite in campus-physical-network campus-physical-dns campus-physical-routing campus-physical-transport known-network-automation-model recovery-latency-model tour-state readonly-model single-instance icon-path partial-connectivity-model network-labels wifi-recovery network-preferences-model path-recovery-model network-memory-model internet-check selected-websites-model connection-health-websites shared-contract auto-recovery auto-recovery-model recovery-journal diagnostic-regressions; do
    if [[ -f "$ROOT/Tests/run-$suite.sh" ]]; then
        run_logged "$suite-tests" sh "$ROOT/Tests/run-$suite.sh"
    fi
done
run_logged artifact-tests python3 "$ROOT/Tests/test_artifact_scripts.py"
run_logged build-mac xcodebuild "${COMMON[@]}" -scheme SZUConnect-macOS -derivedDataPath "$SCRATCH/mac" -destination 'generic/platform=macOS' "${MAC_SIGN[@]}" build
run_logged build-ios xcodebuild "${COMMON[@]}" -scheme SZUConnect-iOS -derivedDataPath "$SCRATCH/ios" -destination 'generic/platform=iOS' "${IOS_SIGN[@]}" build
run_logged build-simulator xcodebuild "${COMMON[@]}" -scheme SZUConnect-iOS -derivedDataPath "$SCRATCH/sim" -destination 'generic/platform=iOS Simulator' CODE_SIGNING_ALLOWED=NO CODE_SIGNING_REQUIRED=NO build
MAC_APP="$SCRATCH/mac/Build/Products/Release/SZUConnect.app"
IOS_APP="$SCRATCH/ios/Build/Products/Release-iphoneos/SZUConnect.app"
SIM_APP="$SCRATCH/sim/Build/Products/Release-iphonesimulator/SZUConnect.app"
run_logged package-mac ditto -c -k --norsrc --keepParent "$MAC_APP" "$STAGE/SZUConnect-macOS.zip"
run_logged package-simulator ditto -c -k --norsrc --keepParent "$SIM_APP" "$STAGE/SZUConnect-iOS-Simulator.zip"
if [[ "$IOS_KIND" == signed-ios ]]; then
    mkdir "$SCRATCH/Payload"
    run_logged copy-ios ditto --norsrc "$IOS_APP" "$SCRATCH/Payload/SZUConnect.app"
    run_logged package-ios ditto -c -k --norsrc --keepParent "$SCRATCH/Payload" "$STAGE/SZUConnect-iOS-Development.ipa"
else
    run_logged package-ios ditto -c -k --norsrc --keepParent "$IOS_APP" "$STAGE/SZUConnect-iOS-Unsigned.zip"
fi
: > "$STAGE/artifact-verification.log"
manifest success
run_logged artifact-verification python3 "$ROOT/Scripts/verify_artifacts.py" --artifacts-dir "$STAGE" --source-root "$ROOT"
# Publish only a fully verified set; successful manifest is the final commit marker.
for file in "$STAGE"/*; do
    [[ "$(basename "$file")" == build-manifest.json ]] || cp "$file" "$DEST/"
done
publish_record
