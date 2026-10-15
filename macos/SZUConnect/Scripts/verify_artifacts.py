#!/usr/bin/env python3
"""Fixed-schema verification for build.sh; Apple tools inspect files, never launch apps."""
import argparse
import datetime
import hashlib
import json
import os
import plistlib
import stat
import subprocess
import tempfile
import zipfile
from pathlib import Path, PurePosixPath

ARTIFACTS = {
    'mac': ('SZUConnect-macOS.zip', 'SZUConnect.app', 'macosx', {'arm64', 'x86_64'}),
    'ios': ('SZUConnect-iOS-Unsigned.zip', 'SZUConnect.app', 'iphoneos', {'arm64'}),
    'signed-ios': ('SZUConnect-iOS-Development.ipa', 'Payload/SZUConnect.app', 'iphoneos', {'arm64'}),
    'sim': ('SZUConnect-iOS-Simulator.zip', 'SZUConnect.app', 'iphonesimulator', {'arm64', 'x86_64'}),
}
MAC_ENTITLEMENTS = 'SZUConnect-macOS.entitlements'
LOCATION_ENTITLEMENT = 'com.apple.security.personal-information.location'


def require(condition, message):
    if not condition:
        raise ValueError(message)


def sha256(path):
    digest = hashlib.sha256()
    with path.open('rb') as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b''):
            digest.update(chunk)
    return digest.hexdigest()


def command(*args):
    result = subprocess.run(args, capture_output=True)
    require(result.returncode == 0, f'{args[0]} inspection failed')
    return result.stdout


def source(root):
    project = json.loads(command('plutil', '-convert', 'json', '-o', '-',
                                 str(root / 'SZUConnect.xcodeproj/project.pbxproj')))
    objects = project['objects']
    def settings(target_name):
        targets = [o for o in objects.values() if o.get('isa') == 'PBXNativeTarget' and o.get('name') == target_name]
        require(len(targets) == 1, f'missing or ambiguous target: {target_name}')
        configs = objects[targets[0]['buildConfigurationList']]['buildConfigurations']
        releases = [objects[c]['buildSettings'] for c in configs if objects[c]['name'] == 'Release']
        require(len(releases) == 1, f'missing Release settings: {target_name}')
        return releases[0]
    mac, ios = settings('SZUConnect-macOS'), settings('SZUConnect-iOS')
    require(all(mac[key] == ios[key] for key in ('MARKETING_VERSION', 'CURRENT_PROJECT_VERSION')),
            'platform versions differ')
    # This app's Release signing input is a fixed, root-local source file.
    require(mac.get('CODE_SIGN_ENTITLEMENTS') == MAC_ENTITLEMENTS,
            f'mac: CODE_SIGN_ENTITLEMENTS must be {MAC_ENTITLEMENTS}')
    entitlements_path = root / MAC_ENTITLEMENTS
    require(entitlements_path.is_file() and not entitlements_path.is_symlink(),
            'mac: missing or non-local entitlements source file')
    entitlements = plistlib.loads(entitlements_path.read_bytes())
    require(isinstance(entitlements, dict) and entitlements.get(LOCATION_ENTITLEMENT) is True,
            f'mac: source {LOCATION_ENTITLEMENT} must be true')
    # Exported source ZIPs have no .git; never accidentally inherit a parent repository.
    top = subprocess.run(['git', '-C', str(root), 'rev-parse', '--show-toplevel'], capture_output=True)
    is_checkout = top.returncode == 0 and Path(top.stdout.decode().strip()).resolve() == root.resolve()
    commit = command('git', '-C', str(root), 'rev-parse', 'HEAD').decode().strip() if is_checkout else None
    dirty = bool(command('git', '-C', str(root), 'status', '--porcelain', '--untracked-files=normal')) if is_checkout else None
    files = {}
    for name in ('Sources', 'Resources', 'Assets.xcassets', 'SZUConnect.xcodeproj', 'Scripts', 'Tests',
                 'Info-iOS.plist', 'Info-macOS.plist', MAC_ENTITLEMENTS):
        path = root / name
        for file in sorted(path.rglob('*') if path.is_dir() else [path]):
            if file.is_file() and '__pycache__' not in file.parts and 'xcuserdata' not in file.parts:
                files[str(file.relative_to(root))] = sha256(file)
    return {
        'commit': commit, 'dirty': dirty,
        'files': files,
        'version': mac['MARKETING_VERSION'], 'build': mac['CURRENT_PROJECT_VERSION'],
        'mac': {'bundleID': mac['PRODUCT_BUNDLE_IDENTIFIER'], 'minimumOS': mac['MACOSX_DEPLOYMENT_TARGET']},
        'ios': {'bundleID': ios['PRODUCT_BUNDLE_IDENTIFIER'], 'minimumOS': ios['IPHONEOS_DEPLOYMENT_TARGET']},
    }


def write_json(path, data):
    temporary = path.with_suffix('.tmp')
    temporary.write_text(json.dumps(data, indent=2) + '\n')
    temporary.replace(path)


def extract(archive, destination):
    # Validate before ditto so framework symlinks retain their native representation.
    with zipfile.ZipFile(archive) as z:
        seen, links = set(), set()
        for item in z.infolist():
            path = PurePosixPath(item.filename)
            require(not path.is_absolute() and '..' not in path.parts and '\\' not in item.filename,
                    'unsafe ZIP path')
            require(str(path) not in seen, 'duplicate ZIP path')
            seen.add(str(path))
            mode = item.external_attr >> 16
            require(stat.S_IFMT(mode) in (0, stat.S_IFREG, stat.S_IFDIR, stat.S_IFLNK), 'unsafe ZIP type')
            if stat.S_ISLNK(mode):
                target = z.read(item).decode()
                resolved = os.path.normpath(str(path.parent / target))
                require(not target.startswith('/') and '\\' not in target and
                        resolved != '..' and not resolved.startswith('../'), 'unsafe ZIP symlink')
                links.add(path)
        require(not any(parent in links for name in seen for parent in PurePosixPath(name).parents),
                'ZIP member traverses symlink')
    command('ditto', '-x', '-k', str(archive), str(destination))


def verify(directory, root):
    report = directory / 'artifact-verification.json'
    report.unlink(missing_ok=True)
    (directory / 'iOS-signing.json').unlink(missing_ok=True)
    signing_summary = None
    manifest = json.loads((directory / 'build-manifest.json').read_text())
    require(manifest['schemaVersion'] == 1 and manifest['status'] == 'success', 'build is not successful')
    current = source(root)
    # Byte-for-byte source evidence remains authoritative after exporting or merging the same tree.
    require(all(manifest['source'][k] == current[k] for k in current if k not in ('dirty', 'commit')), 'source mismatch')
    entries = manifest['artifacts']
    kinds = [entry['kind'] for entry in entries]
    require(len(kinds) == 3 and set(kinds) in ({'mac', 'ios', 'sim'}, {'mac', 'signed-ios', 'sim'}),
            'expected mac, one iOS device archive, and simulator')
    require({p.name for p in directory.iterdir() if p.suffix in ('.zip', '.ipa')} ==
            {ARTIFACTS[k][0] for k in kinds}, 'unexpected or missing archives')
    results = []
    with tempfile.TemporaryDirectory(prefix='szu-verify-', dir='/private/tmp') as scratch:
        for entry in entries:
            kind = entry['kind']
            name, relative, platform, arches = ARTIFACTS[kind]
            archive = directory / name
            require(sha256(archive) == entry['sha256'], f'{kind}: SHA256 mismatch')
            destination = Path(scratch) / kind
            destination.mkdir()
            extract(archive, destination)
            app = destination / relative
            content = app / 'Contents' if kind == 'mac' else app
            info = plistlib.loads((content / 'Info.plist').read_bytes())
            contract = current['mac' if kind == 'mac' else 'ios']
            for field, expected in [('CFBundleIdentifier', contract['bundleID']),
                                    ('CFBundleShortVersionString', current['version']),
                                    ('CFBundleVersion', current['build']),
                                    ('LSMinimumSystemVersion' if kind == 'mac' else 'MinimumOSVersion', contract['minimumOS']),
                                    ('DTPlatformName', platform), ('CFBundleExecutable', 'SZUConnect')]:
                require(info.get(field) == expected, f'{kind}: {field} mismatch')
            if kind == 'mac':
                for field in ('NSLocationUsageDescription', 'NSLocationWhenInUseUsageDescription'):
                    require(isinstance(info.get(field), str) and info[field].strip(),
                            f'mac: {field} must be nonempty')
            license_name = 'AutoSZUWeb-LICENSE.txt'
            license_file = content / ('Resources/' + license_name if kind == 'mac' else license_name)
            require(license_file.is_file() and sha256(license_file) == current['files'].get('Resources/' + license_name),
                    f'{kind}: missing or changed license notice')
            binary = content / ('MacOS/SZUConnect' if kind == 'mac' else 'SZUConnect')
            actual_arches = set(command('lipo', '-archs', str(binary)).decode().split())
            require(actual_arches == arches, f'{kind}: architecture mismatch')
            display = subprocess.run(['codesign', '-d', '--verbose=4', str(app)], capture_output=True)
            signature = entry['signature']
            allowed = {'mac': {'adhoc', 'certificate'}, 'ios': {'unsigned'},
                       'signed-ios': {'development'}, 'sim': {'unsigned'}}[kind]
            require(signature in allowed, f'{kind}: signature mode mismatch')
            if signature == 'unsigned':
                linker_adhoc = (display.returncode == 0 and b'Signature=adhoc' in display.stderr and
                                b'linker-signed' in display.stderr)
                require(linker_adhoc or (display.returncode != 0 and b'not signed at all' in display.stderr),
                        f'{kind}: expected unsigned app')
            else:
                require(display.returncode == 0, f'{kind}: missing signature')
                details = display.stderr.decode()
                require(('Signature=adhoc' in details) == (signature == 'adhoc'), f'{kind}: signature type mismatch')
                if signature != 'adhoc':
                    require('Authority=' in details, f'{kind}: missing certificate authority')
                command('codesign', '--verify', '--deep', '--strict', str(app))
            if kind == 'mac':
                ent = plistlib.loads(command('codesign', '-d', '--entitlements', ':-', '--xml', str(app)))
                require(isinstance(ent, dict) and ent.get(LOCATION_ENTITLEMENT) is True,
                        f'mac: signed {LOCATION_ENTITLEMENT} must be true')
            if kind == 'signed-ios':
                ent = plistlib.loads(command('codesign', '-d', '--entitlements', ':-', str(app)))
                profile = plistlib.loads(command('security', 'cms', '-D', '-i', str(app / 'embedded.mobileprovision')))
                team = manifest['team']
                app_id = ent.get('application-identifier', '')
                profile_ent = profile['Entitlements']
                prefix = profile.get('ApplicationIdentifierPrefix', [])
                require(team and ent.get('com.apple.developer.team-identifier') == team and
                        team in profile['TeamIdentifier'] and
                        any(app_id == p + '.' + contract['bundleID'] for p in prefix), 'iOS signing identity mismatch')
                pattern = profile_ent.get('application-identifier', '')
                require(app_id == pattern or (pattern.endswith('.*') and app_id.startswith(pattern[:-1])),
                        'iOS profile application mismatch')
                require(ent.get('get-task-allow') is True and profile_ent.get('get-task-allow') is True and
                        profile.get('ProvisionedDevices') and 'iOS' in profile.get('Platform', []),
                        'iOS profile is not development')
                require(profile['ExpirationDate'] > datetime.datetime.now(datetime.timezone.utc).replace(tzinfo=None),
                        'iOS profile expired')
                signing_summary = {
                    'applicationIdentifier': app_id,
                    'expirationUTC': profile['ExpirationDate'].replace(tzinfo=datetime.timezone.utc).isoformat(),
                    'registeredDeviceCount': len(profile['ProvisionedDevices']),
                    'getTaskAllow': True, 'platform': profile['Platform']}
            results.append({'kind': kind, 'sha256': entry['sha256'],
                            'signature': 'linker-adhoc' if signature == 'unsigned' and linker_adhoc else signature,
                            'signatureVerified': signature != 'unsigned', 'architectures': sorted(actual_arches)})
    if signing_summary is not None:
        write_json(directory / 'iOS-signing.json', signing_summary)
    write_json(report, {'runID': manifest['runID'], 'artifacts': results})
    return results


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--artifacts-dir', type=Path, required=True)
    parser.add_argument('--source-root', type=Path, default=Path(__file__).resolve().parents[1])
    args = parser.parse_args()
    try:
        verify(args.artifacts_dir.resolve(), args.source_root.resolve())
    except (ValueError, OSError, KeyError, TypeError, zipfile.BadZipFile, plistlib.InvalidFileException) as error:
        parser.exit(1, f'Artifact verification failed: {error}\n')
    print('Artifacts verified; unsigned artifacts received no signature verification.')


if __name__ == '__main__':
    main()
