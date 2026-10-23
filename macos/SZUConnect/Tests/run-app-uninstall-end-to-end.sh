#!/bin/sh
# Disposable running test bundles move themselves to system Trash. Formal dependencies are never constructed.
set -eu
TEST_ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
TEST_BUILD=$(mktemp -d "/private/tmp/szu-uninstall-e2e.XXXXXX")
trap 'rm -rf "$TEST_BUILD"' EXIT HUP INT TERM
TEST_APP="$TEST_BUILD/Template.app"
TEST_OUTPUT=${SZU_UNINSTALL_E2E_OUTPUT:-"$TEST_BUILD/receipts"}
mkdir -p "$TEST_APP/Contents/MacOS" "$TEST_APP/Contents/Resources" "$TEST_OUTPUT"
cat > "$TEST_APP/Contents/Info.plist" <<'PLIST'
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0"><dict>
<key>CFBundleIdentifier</key><string>local.SZUConnect.offline.uninstall-e2e</string>
<key>CFBundleExecutable</key><string>fixture</string>
<key>CFBundlePackageType</key><string>APPL</string>
<key>LSUIElement</key><true/>
<key>NSHighResolutionCapable</key><true/>
</dict></plist>
PLIST
printf 'offline self-trash resource\n' > "$TEST_APP/Contents/Resources/OfflineResource.txt"
xcrun swiftc -warnings-as-errors -module-cache-path "$TEST_BUILD/module-cache" \
  "$TEST_ROOT/Sources/Generated/SharedCopy.swift" "$TEST_ROOT/Sources/AppUninstall.swift" \
  "$TEST_ROOT/Sources/AppUninstallController.swift" "$TEST_ROOT/Sources/AppUninstallLifecycle.swift" \
  "$TEST_ROOT/Tests/AppUninstallEndToEndTests.swift" -o "$TEST_APP/Contents/MacOS/fixture"
codesign --force --sign - --timestamp=none "$TEST_APP"
python3 - "$TEST_APP" "$TEST_BUILD" "$TEST_OUTPUT" <<'PY'
from pathlib import Path
import hashlib, json, shutil, subprocess, sys, time
app_template, root, output = map(Path, sys.argv[1:])
scenarios = [(scenario, iteration) for scenario in ('keep', 'clear') for iteration in range(1, 4)]
scenarios += [(scenario, 1) for scenario in ('cancel-keep', 'cancel-clear', 'retry-keep', 'retry-clear')]
summary = []
for scenario, iteration in scenarios:
    label = f'{scenario}-{iteration}'
    run_root = root / label
    app = run_root / 'Applications' / f'OfflineUninstall-{label}.app'
    app.parent.mkdir(parents=True)
    shutil.copytree(app_template, app)
    receipt = output / (label + '.json')
    started = time.monotonic()
    try:
        child = subprocess.run([str(app / 'Contents/MacOS/fixture'), scenario, str(run_root), str(receipt)],
                               stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=20, text=True)
    except subprocess.TimeoutExpired as error:
        (output / (label + '.log')).write_bytes(error.stdout or b'')
        raise AssertionError(label + ': self-uninstalled AppKit process failed to terminate') from error
    elapsed = time.monotonic() - started
    (output / (label + '.log')).write_text(child.stdout)
    assert child.returncode == 0, (label, child.returncode, child.stdout)
    data = json.loads(receipt.read_text())
    clear = scenario.endswith('clear')
    cancelled = scenario.startswith('cancel-')
    retried = scenario.startswith('retry-')
    success_steps = ['login-item', 'credentials', 'support', 'preferences', 'application'] if clear else ['login-item', 'application']
    expected = success_steps[:-1] + ['application-failed'] + success_steps if retried else success_steps
    assert data['scenario'] == scenario and data['removeData'] == clear, data
    assert data['bundleIdentifier'] == 'local.SZUConnect.offline.uninstall-e2e', data
    assert Path(data['runningBundleAtStart']).resolve() == app.resolve() and not app.exists(), data
    assert data['nestedPresentationCount'] == (2 if cancelled or retried else 1), data
    assert data['finished'] and data['terminated'] and data['childBusyStateRendered'], data
    assert not data['busyAtFinish'] and data['completedAtFinish'], data
    assert data['maintenanceCalls'] == (2 if retried else 1) and data['maintenanceTransitions'] == 1, data
    assert data['mutations'] == expected, data
    assert data['acknowledgedLoginItem'] and data['acknowledgedCredentials'] == clear, data
    assert not data['formalDependenciesConstructed'] and not data['nativeSignatureInspectionUsed'], data
    assert data['allWindowsTransparent'] and data['attachedSheetsAtTermination'] == 0, data
    assert data['selfBundleMoved'] and data['sourceBundleAbsent'], data
    assert data['systemTrashCalls'] == 1 and data['systemTrashSuccesses'] == 1 and data['resultURLPresent'], data
    result = Path(data['resultURL'])
    assert result.suffix == '.app' and ('.Trash' in result.parts or '.Trashes' in result.parts), data
    assert data['postMoveResourceLookupAttempted'] and data['postMoveResourceMissing'], data
    assert data['fakeSupportPresent'] == (not clear) and data['ownFakeCredentialPresent'] == (not clear), data
    assert data['unrelatedFakeCredentialPreserved'] and data['fakePreferencesPresent'] == (not clear), data
    if cancelled:
        assert data['cancellationReleasedUI'] and data['cancelledPreflightMadeNoChanges'], data
        assert data['latePreflightMadeNoChanges'] and data['closedBothSheetsAfterCancellation'], data
    if retried:
        assert data['failureReleasedUI'] and data['failureKeptOwnApplication'] and data['closedBothSheetsAfterFailure'], data
    summary.append({'scenario': scenario, 'iteration': iteration, 'elapsedSeconds': round(elapsed, 3),
                    'receipt': receipt.name, 'selfBundleMoved': True, 'terminated': True})
    print(f'PASS: {label}: running bundle moved to system Trash, original resource unavailable, nested sheets closed and process exited in {elapsed:.3f}s', flush=True)
(output / 'SUMMARY.json').write_text(json.dumps({'runs': summary, 'systemTrashSelfMoves': len(summary),
    'formalDependenciesConstructed': False, 'testBundleExecutableSHA256': hashlib.sha256((app_template / 'Contents/MacOS/fixture').read_bytes()).hexdigest()}, indent=2) + '\n')
print('App uninstall end-to-end: 10 actual self-Trash operations and real AppKit process exits passed; keep/clear repeated 3 times, cancellation and failure/retry covered; only disposable test bundles and fake data.')
PY
