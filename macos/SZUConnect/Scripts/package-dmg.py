#!/usr/bin/env python3
"""Package a verified Mac app without dropping its resource-access entitlements. No app is launched."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import plistlib
import subprocess
import tempfile

LOCATION = 'com.apple.security.personal-information.location'

def run(*arguments):
    return subprocess.run(arguments, check=True, capture_output=True).stdout

def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()

def read_entitlements(app, destination):
    run('codesign', '-d', '--entitlements', str(destination), '--xml', str(app))
    return plistlib.loads(destination.read_bytes())

def signature_details(app):
    result = subprocess.run(['codesign', '-d', '--verbose=4', str(app)], check=True, capture_output=True)
    return result.stderr.decode()

def verify_notarized_app(app):
    run('xcrun', 'stapler', 'validate', str(app))
    run('syspolicy_check', 'distribution', str(app))

def package(source, image, identity, notices, distribution=False, preserve_signature=False, require_notarized_app=False):
    if image.exists():
        raise ValueError('Choose a new DMG output path; existing artifacts are never overwritten')
    if require_notarized_app and not (distribution and preserve_signature):
        raise ValueError('A notarized app must use --distribution --preserve-signature')
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
            # Remove linker debug maps before re-signing, without changing code.
            run('xcrun', 'strip', '-S', str(app / 'Contents/MacOS/SZUConnect'))
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
        os.symlink('/Applications', stage / 'Applications')
        (stage / '安装说明.txt').write_text(f'''深大随连 macOS 版

安装
将“深大随连.app”拖入 Applications（应用程序），推出磁盘后打开。
替换已有应用前先退出，账号与设置会保留。
适用于 macOS 14+，支持 Apple 芯片和 Intel。

首次使用
连接校园 Wi-Fi 或网线，打开应用检测，按需填写账号并登录。
需要自动恢复时，记住网络并开启“自动重连”。自动找网与打开 Wi-Fi 电源另行选择。
读取 Wi-Fi 名称可能需要定位授权，请本人按系统提示处理。

使用说明
关闭窗口后应用留在菜单栏，从其菜单选择“退出”才会关闭。
账号与数据只存本机，删除应用不会自动清除。管理方法见应用内“关于”。
本项目为非官方工具，版权与许可见“第三方声明.txt”。

{'应用采用 Developer ID 签名，请从可信发布渠道下载。' if distribution else '此构建未通过 Apple 公证。首次打开可能出现无法验证开发者的提示，请仅从可信发布渠道下载，并按安装说明在系统设置中确认打开。'}
''')
        if notices:
            (stage / '第三方声明.txt').write_bytes(notices.read_bytes())
        run('hdiutil', 'create', '-volname', '深大随连 ' + info['CFBundleShortVersionString'], '-srcfolder', str(stage), '-fs', 'HFS+', '-format', 'UDZO', '-imagekey', 'zlib-level=9', str(image))
        if distribution and not preserve_signature:
            run('codesign', '--force', '--sign', identity, '--timestamp', str(image))
            run('codesign', '--verify', '--strict', str(image))
        run('hdiutil', 'verify', str(image))
        mounted = plistlib.loads(run('hdiutil', 'attach', '-readonly', '-nobrowse', '-plist', str(image)))
        point = next(Path(item['mount-point']) for item in mounted['system-entities'] if 'mount-point' in item)
        try:
            mounted_app = point / '深大随连.app'
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
            if os.readlink(point / 'Applications') != '/Applications':
                raise ValueError('Invalid Applications link')
        finally:
            run('hdiutil', 'detach', str(point))
        return dict(status='PASS image/mount/copy/signature/entitlement verification', dmg=str(image), sha256=sha(image),
                    bytes=image.stat().st_size, version=info['CFBundleShortVersionString'], build=info['CFBundleVersion'],
                    architectures=['arm64', 'x86_64'], minimumOS=info['LSMinimumSystemVersion'],
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
    parser.add_argument('--notices', type=Path)
    args = parser.parse_args()
    result = package(args.app.resolve(), args.output.resolve(), args.sign_identity, args.notices,
                     args.distribution, args.preserve_signature, args.require_notarized_app)
    receipt = args.output.with_suffix('.receipt.json')
    receipt.write_text(json.dumps(result, ensure_ascii=False, indent=2) + '\n')
    args.output.with_suffix(args.output.suffix + '.sha256').write_text(result['sha256'] + '  ' + args.output.name + '\n')
    print(receipt)

if __name__ == '__main__':
    main()
