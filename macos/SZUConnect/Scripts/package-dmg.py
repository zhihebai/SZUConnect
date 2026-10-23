#!/usr/bin/env python3
"""Package a verified Mac app without dropping its resource-access entitlements. No app is launched."""
import argparse
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import plistlib
import subprocess
import tempfile

LOCATION = 'com.apple.security.personal-information.location'
VOLUME_ICON_NAME = '.VolumeIcon.icns'
PROJECT_ROOT = Path(__file__).resolve().parents[1]
LICENSE_SOURCES = {
    'LICENSE': PROJECT_ROOT / '../../LICENSE',
    'AutoSZUWeb-LICENSE.txt': PROJECT_ROOT / 'Resources/AutoSZUWeb-LICENSE.txt',
    'curl-COPYING.txt': PROJECT_ROOT / '../../docs/licenses/curl-COPYING.txt',
}
_layout_spec = importlib.util.spec_from_file_location('szu_dmg_layout', Path(__file__).with_name('dmg_layout.py'))
layout = importlib.util.module_from_spec(_layout_spec)
_layout_spec.loader.exec_module(layout)

def run(*arguments):
    return subprocess.run(arguments, check=True, capture_output=True).stdout

def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()

def verify_bundled_licenses(app):
    resources = app / 'Contents/Resources'
    result = {}
    for name, original in LICENSE_SOURCES.items():
        bundled = resources / name
        if not bundled.is_file() or bundled.is_symlink() or bundled.read_bytes() != original.read_bytes():
            raise ValueError('Missing or changed bundled license: ' + name)
        result[name] = sha(bundled)
    return result

def read_entitlements(app, destination):
    run('codesign', '-d', '--entitlements', str(destination), '--xml', str(app))
    return plistlib.loads(destination.read_bytes())

def signature_details(app):
    result = subprocess.run(['codesign', '-d', '--verbose=4', str(app)], check=True, capture_output=True)
    return result.stderr.decode()

def verify_notarized_app(app):
    run('xcrun', 'stapler', 'validate', str(app))
    run('syspolicy_check', 'distribution', str(app))

def verify_volume_icon(root, expected_hash):
    icon = root / VOLUME_ICON_NAME
    if not icon.is_file() or icon.is_symlink() or sha(icon) != expected_hash:
        raise ValueError('The volume icon differs from the app icon')
    finder_info = bytes.fromhex(run('xattr', '-px', 'com.apple.FinderInfo', str(root)).decode('ascii'))
    # Finder.h: kHasCustomIcon is 0x0400 in the root folder's Finder flags.
    if len(finder_info) != 32 or not int.from_bytes(finder_info[8:10], 'big') & 0x0400:
        raise ValueError('The volume custom-icon flag is missing')

def create_installation_image(stage, image, temp, version):
    """Set Finder layout on a private writable image, then compress it for distribution."""
    artwork = temp / 'dmg-artwork'
    run('xcrun', 'clang', '-fobjc-arc', '-Wno-deprecated-declarations',
        '-framework', 'AppKit', '-framework', 'CoreServices',
        str(Path(__file__).parent / 'dmg/Artwork.m'), '-o', str(artwork))
    background = stage / '.background' / layout.BACKGROUND_NAME
    background.parent.mkdir()
    run(str(artwork), 'draw', '-', str(background))
    icon_source = stage / '深大随连.app/Contents/Resources/AppIcon.icns'
    if not icon_source.is_file() or icon_source.is_symlink():
        raise ValueError('The app icon is unavailable')
    (stage / VOLUME_ICON_NAME).write_bytes(icon_source.read_bytes())
    icon_hash = sha(icon_source)
    writable = temp / 'layout.dmg'
    run('hdiutil', 'create', '-volname', '深大随连 ' + version, '-srcfolder', str(stage),
        '-fs', 'HFS+', '-format', 'UDRW', str(writable))
    mounted = plistlib.loads(run('hdiutil', 'attach', '-nobrowse', '-plist', str(writable)))
    point = next(Path(item['mount-point']) for item in mounted['system-entities'] if 'mount-point' in item)
    try:
        run('xcrun', 'SetFile', '-a', 'C', str(point))
        verify_volume_icon(point, icon_hash)
        alias = temp / 'background.alias'
        run(str(artwork), 'alias', str(point / '.background' / layout.BACKGROUND_NAME), str(alias))
        alias.write_bytes(layout.volume_local_alias(alias.read_bytes()))
        run(str(artwork), 'check-alias', str(alias), str(point / '.background' / layout.BACKGROUND_NAME))
        layout.write_layout(point, alias.read_bytes())
        guide = dict(instruction=layout.INSTRUCTION, windowSize=[layout.WIDTH, layout.HEIGHT],
                     iconLocations={name: list(position) for name, position in layout.ICON_LOCATIONS.items()},
                     backgroundSHA256=sha(point / '.background' / layout.BACKGROUND_NAME),
                     finderLayoutSHA256=sha(point / '.DS_Store'),
                     volumeTitle='深大随连 ' + version,
                     volumeIconSHA256=icon_hash, volumeHasCustomIcon=True)
    finally:
        run('hdiutil', 'detach', str(point))
    run('hdiutil', 'convert', str(writable), '-format', 'UDZO', '-imagekey', 'zlib-level=9', '-o', str(image))
    return guide

def package(source, image, identity, notices, distribution=False, preserve_signature=False, require_notarized_app=False):
    if image.exists():
        raise ValueError('Choose a new DMG output path; existing artifacts are never overwritten')
    if require_notarized_app and not (distribution and preserve_signature):
        raise ValueError('A notarized app must use --distribution --preserve-signature')
    licenses = verify_bundled_licenses(source)
    run('codesign', '--verify', '--deep', '--strict', str(source))
    if require_notarized_app:
        verify_notarized_app(source)
    info = plistlib.loads((source / 'Contents/Info.plist').read_bytes())
    if info.get('CFBundleIdentifier') != 'local.SZUConnect.mac':
        raise ValueError('Only the formal Mac app may be packaged')
    image.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='szu-dmg-') as scratch:
        temp = Path(scratch)
        entitlements = read_entitlements(source, temp / 'original-entitlements.plist')
        if entitlements.get(LOCATION) is not True:
            raise ValueError('The source app is missing its Location entitlement')
        if preserve_signature and entitlements.get('com.apple.security.get-task-allow', False) is not False:
            raise ValueError('Preserving a signature requires an app without debugging access')
        # Preserve every capability; only a re-signed copy changes its debugging entitlement.
        if not preserve_signature:
            entitlements['com.apple.security.get-task-allow'] = False
        ent_path = temp / 'dmg-entitlements.plist'
        ent_path.write_bytes(plistlib.dumps(entitlements))
        stage = temp / 'stage'
        stage.mkdir()
        app = stage / '深大随连.app'
        run('ditto', '--norsrc', '--noextattr', str(source), str(app))
        if not preserve_signature:
            if not identity:
                raise ValueError('Re-signing requires --sign-identity')
            # Remove debug maps and local symbols before re-signing, without changing code.
            run('xcrun', 'strip', '-S', '-x', str(app / 'Contents/MacOS/SZUConnect'))
            run('codesign', '--force', '--sign', identity, '--options', 'runtime', '--entitlements', str(ent_path),
                '--timestamp' if distribution else '--timestamp=none', str(app))
        run('codesign', '--verify', '--deep', '--strict', str(app))
        details = signature_details(app)
        if distribution and not ('Authority=Developer ID Application:' in details and 'Timestamp=' in details and 'runtime' in details):
            raise ValueError('Distribution requires Developer ID Application, secure timestamp and hardened runtime')
        if require_notarized_app:
            verify_notarized_app(app)
        for arch in ('arm64', 'x86_64'):
            before = run('otool', '-arch', arch, '-t', str(source / 'Contents/MacOS/SZUConnect')).splitlines()[1:]
            after = run('otool', '-arch', arch, '-t', str(app / 'Contents/MacOS/SZUConnect')).splitlines()[1:]
            if before != after:
                raise ValueError('Re-signing changed a code section')
        os.symlink('/Applications', stage / layout.APPLICATIONS_LINK)
        # Only the app and destination are visible; the arrow and Chinese guidance
        # are Finder background artwork, never an extra document or app modification.
        guide = create_installation_image(stage, image, temp, info['CFBundleShortVersionString'])
        if distribution and not preserve_signature:
            run('codesign', '--force', '--sign', identity, '--timestamp', str(image))
            run('codesign', '--verify', '--strict', str(image))
        run('hdiutil', 'verify', str(image))
        mounted = plistlib.loads(run('hdiutil', 'attach', '-readonly', '-nobrowse', '-plist', str(image)))
        point = next(Path(item['mount-point']) for item in mounted['system-entities'] if 'mount-point' in item)
        try:
            visible_items = {path.name for path in point.iterdir() if not path.name.startswith('.')}
            if visible_items != {'深大随连.app', layout.APPLICATIONS_LINK}:
                raise ValueError('The DMG must show only the app and Applications')
            verify_volume_icon(point, guide['volumeIconSHA256'])
            if (sha(point / '.background' / layout.BACKGROUND_NAME) != guide['backgroundSHA256']
                    or sha(point / '.DS_Store') != guide['finderLayoutSHA256']):
                raise ValueError('The mounted installation guidance differs')
            run(str(temp / 'dmg-artwork'), 'check-alias', str(temp / 'background.alias'),
                str(point / '.background' / layout.BACKGROUND_NAME))
            mounted_app = point / '深大随连.app'
            if verify_bundled_licenses(mounted_app) != licenses:
                raise ValueError('The mounted app license files differ')
            run('codesign', '--verify', '--deep', '--strict', str(mounted_app))
            if require_notarized_app:
                verify_notarized_app(mounted_app)
            copied = temp / 'copied.app'
            run('ditto', '--norsrc', '--noextattr', str(mounted_app), str(copied))
            run('codesign', '--verify', '--deep', '--strict', str(copied))
            if require_notarized_app:
                verify_notarized_app(copied)
            actual = read_entitlements(copied, temp / 'verified-entitlements.plist')
            if actual.get(LOCATION) is not True or actual.get('com.apple.security.get-task-allow', False) is not False:
                raise ValueError('DMG copy lost Location capability or retained debugging access')
            if sha(copied / 'Contents/MacOS/SZUConnect') != sha(app / 'Contents/MacOS/SZUConnect'):
                raise ValueError('Mounted/copied executable differs from package input')
            if os.readlink(point / layout.APPLICATIONS_LINK) != '/Applications':
                raise ValueError('Invalid Applications link')
        finally:
            run('hdiutil', 'detach', str(point))
        return dict(status='PASS image/mount/copy/signature/entitlement verification', dmg=str(image), sha256=sha(image),
                    bytes=image.stat().st_size, version=info['CFBundleShortVersionString'], build=info['CFBundleVersion'],
                    architectures=['arm64', 'x86_64'], minimumOS=info['LSMinimumSystemVersion'],
                    visibleItems=['深大随连.app', layout.APPLICATIONS_LINK], bundledLicenses=licenses,
                    installationGuide=guide,
                    appNotarized=require_notarized_app, dmgNotarized=False,
                    locationEntitlement=True, debuggingEntitlement=False, codeSectionsUnchangedByResigning=True,
                    distributionSignature=distribution, signaturePreserved=preserve_signature,
                    appExecutableSHA256=sha(app / 'Contents/MacOS/SZUConnect'),
                    dmgSigned=distribution and not preserve_signature, secondMacTested=False)

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--app', required=True, type=Path)
    parser.add_argument('--output', required=True, type=Path)
    parser.add_argument('--sign-identity')
    parser.add_argument('--distribution', action='store_true', help='Require Developer ID, secure timestamp and hardened runtime')
    parser.add_argument('--preserve-signature', action='store_true', help='Package an exported app without invalidating its signature or stapled ticket')
    parser.add_argument('--require-notarized-app', action='store_true', help='Verify the app ticket and distribution policy at source, stage, mount and copy')
    parser.add_argument('--notices', type=Path, help='Compatibility option; full licenses are verified inside the app, not placed beside it')
    args = parser.parse_args()
    result = package(args.app.resolve(), args.output.resolve(), args.sign_identity, args.notices,
                     args.distribution, args.preserve_signature, args.require_notarized_app)
    receipt = args.output.with_suffix('.receipt.json')
    receipt.write_text(json.dumps(result, ensure_ascii=False, indent=2) + '\n')
    args.output.with_suffix(args.output.suffix + '.sha256').write_text(result['sha256'] + '  ' + args.output.name + '\n')
    print(receipt)

if __name__ == '__main__':
    main()
