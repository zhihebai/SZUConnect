#!/usr/bin/env python3
"""Offline fixtures: no Xcode builds, app execution, credentials, or network."""
import copy
import datetime
import importlib.util
import json
import os
import plistlib
import shutil
import stat
import subprocess
import sys
import tempfile
import unittest
import zipfile
from pathlib import Path
from unittest.mock import patch

sys.dont_write_bytecode = True
ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('verifier', ROOT / 'Scripts/verify_artifacts.py')
v = importlib.util.module_from_spec(spec)
spec.loader.exec_module(v)
REAL_RUN = subprocess.run


class ArtifactTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(dir='/private/tmp')
        self.addCleanup(self.temp.cleanup)
        self.directory = Path(self.temp.name)
        self.source = v.source(ROOT)
        self.manifest = {'schemaVersion': 1, 'status': 'success', 'runID': 'fixture',
                         'source': self.source, 'team': 'TEAM', 'artifacts': []}
        self.mac_ent = {'com.apple.security.personal-information.location': True}
        self.ios_ent = {'application-identifier': 'PREFIX.' + self.source['ios']['bundleID'],
                        'com.apple.developer.team-identifier': 'TEAM', 'get-task-allow': True}
        self.profile = {'Entitlements': copy.deepcopy(self.ios_ent), 'TeamIdentifier': ['TEAM'],
                        'ApplicationIdentifierPrefix': ['PREFIX'], 'ProvisionedDevices': ['fixture-only'],
                        'Platform': ['iOS'], 'ExpirationDate': datetime.datetime.now() + datetime.timedelta(days=2)}
        self.bad_arch = False
        self.bad_signature = False
        self.modes = {}

    def fixture(self, signed=False, certificate=False, info_override=None, missing_license=False):
        for kind in ('mac', 'signed-ios' if signed else 'ios', 'sim'):
            name, app, platform, _ = v.ARTIFACTS[kind]
            contract = self.source['mac' if kind == 'mac' else 'ios']
            info = {'CFBundleIdentifier': contract['bundleID'], 'CFBundleVersion': self.source['build'],
                    'CFBundleShortVersionString': self.source['version'], 'CFBundleExecutable': 'SZUConnect',
                    'DTPlatformName': platform,
                    'LSMinimumSystemVersion' if kind == 'mac' else 'MinimumOSVersion': contract['minimumOS']}
            if kind == 'mac':
                info.update(NSLocationUsageDescription='用于读取所选校园 Wi-Fi 名称。',
                            NSLocationWhenInUseUsageDescription='用于读取所选校园 Wi-Fi 名称。')
            if info_override and kind == 'mac':
                for field, value in info_override.items():
                    if value is None:
                        info.pop(field, None)
                    else:
                        info[field] = value
            content = app + ('/Contents' if kind == 'mac' else '')
            with zipfile.ZipFile(self.directory / name, 'w') as z:
                z.writestr(content + '/Info.plist', plistlib.dumps(info))
                z.writestr(content + ('/MacOS/SZUConnect' if kind == 'mac' else '/SZUConnect'), b'fixture')
                if not missing_license:
                    for license_name, source_name in v.LICENSE_SOURCES.items():
                        if kind != 'mac' and license_name == 'curl-COPYING.txt':
                            continue
                        z.writestr(content + ('/Resources/' if kind == 'mac' else '/') + license_name,
                                   (ROOT / source_name).read_bytes())
            mode = ('certificate' if certificate else 'adhoc') if kind == 'mac' else ('development' if signed and kind == 'signed-ios' else 'unsigned')
            self.modes[kind] = mode
            self.manifest['artifacts'].append({'kind': kind, 'signature': mode, 'sha256': v.sha256(self.directory / name)})
        self.save()

    def save(self):
        v.write_json(self.directory / 'build-manifest.json', self.manifest)

    def tools(self, args, **kwargs):
        if args[0] in ('git', 'ditto', 'plutil'):
            return REAL_RUN(args, **kwargs)
        kind = next(k for k in self.modes if k in Path(args[-1]).parts)
        output, error, code = b'', b'', 0
        if args[0] == 'lipo':
            output = b'arm64' if self.bad_arch else ' '.join(v.ARTIFACTS[kind][3]).encode()
        elif args[0] == 'security':
            output = plistlib.dumps(self.profile)
        elif '--entitlements' in args:
            # macOS codesign needs --xml to emit parseable plist on current tools.
            if kind == 'mac' and '--xml' not in args:
                output = b'[Dict]\n\t[Key] com.apple.security.personal-information.location\n\t[Value]\n\t\t[Bool] true\n'
            else:
                output = plistlib.dumps(self.mac_ent if kind == 'mac' else self.ios_ent)
        elif '--verify' in args:
            code = 1 if self.bad_signature else 0
        elif self.modes[kind] == 'unsigned':
            code, error = 1, b'code object is not signed at all'
        elif self.modes[kind] == 'linker-adhoc':
            error = b'Signature=adhoc\nflags=0x20002(adhoc,linker-signed)'
        else:
            error = b'Signature=adhoc' if self.modes[kind] == 'adhoc' else b'Authority=Apple Development: Fixture'
        return subprocess.CompletedProcess(args, code, output, error)

    def verify(self):
        with patch.object(v.subprocess, 'run', side_effect=self.tools):
            return v.verify(self.directory, ROOT)

    def test_success_unsigned_and_adhoc(self):
        self.fixture()
        results = self.verify()
        self.assertEqual([r['signatureVerified'] for r in results], [True, False, False])

    def test_success_development_and_certificate(self):
        self.fixture(signed=True, certificate=True)
        self.assertEqual([r['signatureVerified'] for r in self.verify()], [True, True, False])

    def test_missing_license_is_rejected(self):
        self.fixture(missing_license=True)
        with self.assertRaisesRegex(ValueError, 'license notice'):
            self.verify()

    def test_mac_location_entitlement_required_for_all_signature_modes(self):
        for certificate in (False, True):
            for value in (None, False, 'true', 1):
                with self.subTest(certificate=certificate, value=value):
                    self.manifest['artifacts'] = []
                    self.mac_ent = {'com.apple.security.personal-information.location': True}
                    self.fixture(signed=True, certificate=certificate)
                    self.verify()
                    if value is None:
                        self.mac_ent.clear()
                    else:
                        self.mac_ent['com.apple.security.personal-information.location'] = value
                    with self.assertRaisesRegex(ValueError, 'mac:.*location'):
                        self.verify()
                    self.assertFalse((self.directory / 'artifact-verification.json').exists())
                    self.assertFalse((self.directory / 'iOS-signing.json').exists())

    def test_mac_location_usage_descriptions_required(self):
        for field in ('NSLocationUsageDescription', 'NSLocationWhenInUseUsageDescription'):
            for value in (None, '', ' \n ', 1):
                with self.subTest(field=field, value=value):
                    self.manifest['artifacts'] = []
                    self.fixture(info_override={field: value})
                    with self.assertRaisesRegex(ValueError, field):
                        self.verify()

    def test_source_location_entitlements_in_manifest(self):
        path = 'SZUConnect-macOS.entitlements'
        self.assertEqual(self.source['files'].get(path), v.sha256(ROOT / path))

    def test_source_location_entitlements_file_required(self):
        root, _, _ = self.build_fixture()
        (root / 'SZUConnect-macOS.entitlements').unlink()
        with self.assertRaisesRegex(ValueError, 'mac:.*entitlements'):
            v.source(root)

    def test_source_location_entitlement_must_be_true(self):
        root, _, _ = self.build_fixture()
        path = root / 'SZUConnect-macOS.entitlements'
        for ent in ({}, {'com.apple.security.personal-information.location': False},
                    {'com.apple.security.personal-information.location': 'true'},
                    {'com.apple.security.personal-information.location': 1}):
            with self.subTest(entitlements=ent):
                path.write_bytes(plistlib.dumps(ent))
                with self.assertRaisesRegex(ValueError, 'mac:.*location'):
                    v.source(root)

    def test_source_location_entitlements_setting_is_local_required_input(self):
        root, _, _ = self.build_fixture()
        project = root / 'SZUConnect.xcodeproj/project.pbxproj'
        original = json.loads(v.command('plutil', '-convert', 'json', '-o', '-', str(project)))
        for value in (None, '', '../SZUConnect-macOS.entitlements', '/tmp/SZUConnect-macOS.entitlements',
                      '$(SRCROOT)/SZUConnect-macOS.entitlements', 'other.entitlements'):
            with self.subTest(value=value):
                data = copy.deepcopy(original)
                for obj in data['objects'].values():
                    settings = obj.get('buildSettings', {})
                    if settings.get('PRODUCT_BUNDLE_IDENTIFIER') == self.source['mac']['bundleID']:
                        if value is None:
                            settings.pop('CODE_SIGN_ENTITLEMENTS', None)
                        else:
                            settings['CODE_SIGN_ENTITLEMENTS'] = value
                project.write_bytes(plistlib.dumps(data))
                with self.assertRaisesRegex(ValueError, 'mac:.*CODE_SIGN_ENTITLEMENTS'):
                    v.source(root)

    def test_linker_signature_is_not_bundle_verification(self):
        self.fixture()
        self.modes['sim'] = 'linker-adhoc'
        result = self.verify()[-1]
        self.assertEqual(result['signature'], 'linker-adhoc')
        self.assertFalse(result['signatureVerified'])

    def test_hash_and_stale_report(self):
        self.fixture()
        self.verify()
        (self.directory / v.ARTIFACTS['mac'][0]).write_bytes(b'changed')
        with self.assertRaisesRegex(ValueError, 'SHA256'):
            self.verify()
        self.assertFalse((self.directory / 'artifact-verification.json').exists())

    def test_source_mismatch(self):
        self.fixture()
        self.manifest['source'] = dict(self.source, files={'wrong': 'digest'})
        self.save()
        with self.assertRaisesRegex(ValueError, 'source mismatch'):
            self.verify()

    def test_git_metadata_is_trace_information(self):
        self.fixture()
        self.manifest['source'] = dict(self.source, commit='prior-merge-commit', dirty=False)
        self.save()
        self.assertEqual(len(self.verify()), 3)

    def test_exported_source_and_xcode_formatting(self):
        repository = self.directory / 'export'
        export = repository / 'macos/SZUConnect'
        shutil.copytree(ROOT, export, ignore=shutil.ignore_patterns('.git', 'build', '__pycache__'))
        shutil.copy(ROOT / '../../LICENSE', repository / 'LICENSE')
        (repository / 'docs/licenses').mkdir(parents=True)
        shutil.copy(ROOT / '../../docs/licenses/curl-COPYING.txt', repository / 'docs/licenses/curl-COPYING.txt')
        exported = v.source(export)
        self.assertIsNone(exported['commit'])
        self.assertEqual(exported['files'], self.source['files'])
        project = export / 'SZUConnect.xcodeproj/project.pbxproj'
        quoted = '"MARKETING_VERSION" = "' + self.source['version'] + '"'
        project.write_text(project.read_text().replace(quoted, 'MARKETING_VERSION = ' + self.source['version']))
        self.assertEqual(v.source(export)['version'], self.source['version'])

    def test_signing_summary_has_no_device_identifiers(self):
        self.fixture(signed=True)
        self.verify()
        summary = json.loads((self.directory / 'iOS-signing.json').read_text())
        self.assertEqual(summary['registeredDeviceCount'], 1)
        self.assertNotIn('fixture-only', json.dumps(summary))
        self.manifest['status'] = 'failed'
        self.save()
        with self.assertRaises(ValueError):
            self.verify()
        self.assertFalse((self.directory / 'iOS-signing.json').exists())

    def test_non_success_and_schema(self):
        self.fixture()
        for field, value in [('status', 'running'), ('status', 'failed'), ('schemaVersion', 2)]:
            with self.subTest(value=value):
                self.manifest.update(status='success', schemaVersion=1)
                self.manifest[field] = value
                self.save()
                with self.assertRaisesRegex(ValueError, 'not successful'):
                    self.verify()

    def test_bundle_contract(self):
        for field in ('CFBundleIdentifier', 'CFBundleVersion', 'CFBundleShortVersionString',
                      'LSMinimumSystemVersion', 'DTPlatformName', 'CFBundleExecutable'):
            with self.subTest(field=field):
                self.manifest['artifacts'] = []
                self.fixture(info_override={field: 'wrong'})
                with self.assertRaisesRegex(ValueError, field):
                    self.verify()

    def test_architecture(self):
        self.fixture()
        self.bad_arch = True
        with self.assertRaisesRegex(ValueError, 'architecture'):
            self.verify()

    def test_signature_failure_and_type(self):
        self.fixture()
        self.bad_signature = True
        with self.assertRaisesRegex(ValueError, 'inspection failed'):
            self.verify()
        self.bad_signature = False
        self.modes['mac'] = 'certificate'
        with self.assertRaisesRegex(ValueError, 'signature type'):
            self.verify()
        self.modes['mac'] = 'adhoc'
        self.modes['ios'] = 'adhoc'
        with self.assertRaisesRegex(ValueError, 'expected unsigned'):
            self.verify()

    def test_profile_failures(self):
        self.fixture(signed=True)
        original = copy.deepcopy(self.profile)
        for field, value in [('ExpirationDate', datetime.datetime(2000, 1, 1)),
                             ('TeamIdentifier', ['OTHER']), ('ProvisionedDevices', [])]:
            with self.subTest(field=field):
                self.profile = dict(original, **{field: value})
                with self.assertRaises(ValueError):
                    self.verify()
        self.profile = original
        self.ios_ent['get-task-allow'] = False
        with self.assertRaisesRegex(ValueError, 'not development'):
            self.verify()

    def test_archive_set(self):
        self.fixture()
        (self.directory / 'extra.zip').touch()
        with self.assertRaisesRegex(ValueError, 'unexpected'):
            self.verify()
        (self.directory / 'extra.zip').unlink()
        self.manifest['artifacts'].append(self.manifest['artifacts'][0])
        self.save()
        with self.assertRaisesRegex(ValueError, 'expected mac'):
            self.verify()

    def test_zip_paths_and_symlinks(self):
        cases = [('../escape', b'x', False), ('/escape', b'x', False),
                 ('app/link', b'../../escape', True), ('app/link', b'/escape', True)]
        for name, data, link in cases:
            with self.subTest(name=name, data=data):
                archive = self.directory / 'unsafe.zip'
                with zipfile.ZipFile(archive, 'w') as z:
                    item = zipfile.ZipInfo(name)
                    item.external_attr = (stat.S_IFLNK | 0o777) << 16 if link else 0
                    z.writestr(item, data)
                with self.assertRaisesRegex(ValueError, 'unsafe ZIP'):
                    v.extract(archive, self.directory / 'extract')
        with zipfile.ZipFile(archive, 'w') as z:
            item = zipfile.ZipInfo('app/link')
            item.external_attr = (stat.S_IFLNK | 0o777) << 16
            z.writestr(item, 'target')
            z.writestr('app/link/file', b'x')
        with self.assertRaisesRegex(ValueError, 'traverses symlink'):
            v.extract(archive, self.directory / 'extract')

    def build_fixture(self):
        repository = self.directory / 'source'
        root = repository / 'macos/SZUConnect'
        (root / 'Scripts').mkdir(parents=True)
        (root / 'Tests').mkdir()
        shutil.copytree(ROOT / 'SZUConnect.xcodeproj', root / 'SZUConnect.xcodeproj')
        shutil.copytree(ROOT / 'Resources', root / 'Resources')
        shutil.copy(ROOT / '../../LICENSE', repository / 'LICENSE')
        (repository / 'docs/licenses').mkdir(parents=True)
        shutil.copy(ROOT / '../../docs/licenses/curl-COPYING.txt', repository / 'docs/licenses/curl-COPYING.txt')
        for name in ('build.sh', 'verify_artifacts.py'):
            shutil.copy(ROOT / 'Scripts' / name, root / 'Scripts' / name)
        for name in ('Info-iOS.plist', 'Info-macOS.plist', 'SZUConnect-macOS.entitlements'):
            shutil.copy(ROOT / name, root / name)
        output = self.directory / 'output with spaces'
        output.mkdir()
        (output / 'build-manifest.json').write_text('{"status":"success"}')
        (output / 'SZUConnect-macOS.zip').write_bytes(b'old')
        (root / 'Tests/run-offline.sh').write_text('cat "$SZU_OUTPUT_DIR/build-manifest.json"\nexit 23\n')
        fake_bin = self.directory / 'bin'
        fake_bin.mkdir()
        (fake_bin / 'git').write_text('#!/bin/sh\ncase "$*" in *rev-parse*) echo fixture;; esac\n')
        (fake_bin / 'git').chmod(0o755)
        env = dict(os.environ, PATH=str(fake_bin) + ':' + os.environ['PATH'], SZU_OUTPUT_DIR=str(output))
        return root, output, env

    def test_build_failure_invalidates_old_success(self):
        root, output, env = self.build_fixture()
        result = REAL_RUN(['bash', str(root / 'Scripts/build.sh')], env=env, capture_output=True, text=True)
        self.assertEqual(result.returncode, 23, result.stderr)
        self.assertEqual(json.loads((output / 'build-manifest.json').read_text())['status'], 'failed')
        self.assertEqual(json.loads((output / 'offline-tests.log').read_text())['status'], 'running')
        self.assertFalse((output / 'SZUConnect-macOS.zip').exists())
        self.assertFalse((output / '.szu-build-lock').exists())
        # Failed staging is intentionally retained for diagnosis; remove this test's copy.
        scratch = result.stderr.split('staging retained: ')[1].splitlines()[0]
        shutil.rmtree(scratch)

    def test_build_publish_success_with_mock_build_tools(self):
        self.fixture()
        root, output, env = self.build_fixture()
        (root / 'Tests/run-offline.sh').write_text('exit 0\n')
        (root / 'Tests/run-connection-policy.sh').write_text('echo policy-fixture\n')
        (root / 'Tests/run-transport.sh').write_text('echo transport-fixture\n')
        (root / 'Tests/run-website-diagnostics.sh').write_text('echo website-fixture\n')
        (root / 'Tests/run-app-model.sh').write_text('echo model-fixture\n')
        (root / 'Tests/run-health.sh').write_text('echo health-fixture\n')
        (root / 'Tests/run-event-store.sh').write_text('echo event-store-fixture\n')
        (root / 'Tests/run-network-identity.sh').write_text('echo identity-fixture\n')
        (root / 'Tests/run-network-memory.sh').write_text('echo memory-fixture\n')
        (root / 'Tests/run-memory-plan.sh').write_text('echo memory-plan-fixture\n')
        (root / 'Tests/run-network-memory-model.sh').write_text('echo memory-model-fixture\n')
        (root / 'Tests/test_artifact_scripts.py').write_text('print("fixture tests")\n')
        fake_bin = self.directory / 'bin'
        scripts = {
            'xcodebuild': '#!/bin/sh\necho mock-build-only\n',
            'lipo': '#!/bin/sh\ncase "$*" in */ios/*) echo arm64;; *) echo "arm64 x86_64";; esac\n',
            'codesign': '#!/usr/bin/env python3\nimport plistlib, sys\n'
                        'if "--entitlements" in sys.argv:\n'
                        '    assert "--xml" in sys.argv\n'
                        '    sys.stdout.buffer.write(plistlib.dumps({"com.apple.security.personal-information.location": True}))\n'
                        'elif "/mac/" in sys.argv[-1]: print("Signature=adhoc", file=sys.stderr)\n'
                        'else:\n'
                        '    print("code object is not signed at all", file=sys.stderr)\n'
                        '    sys.exit(1)\n',
            'ditto': '#!/usr/bin/env python3\nimport os, shutil, sys\nfrom pathlib import Path\n'
                     'if sys.argv[1] == "-x": os.execv("/usr/bin/ditto", ["ditto"] + sys.argv[1:])\n'
                     'shutil.copy(Path(os.environ["FIXTURE_ARCHIVES"]) / Path(sys.argv[-1]).name, sys.argv[-1])\n',
        }
        for name, script in scripts.items():
            (fake_bin / name).write_text(script)
            (fake_bin / name).chmod(0o755)
        env['FIXTURE_ARCHIVES'] = str(self.directory)
        env.pop('SZU_TEAM', None)
        env.pop('SZU_SIGN_IDENTITY', None)
        result = REAL_RUN(['bash', str(root / 'Scripts/build.sh')], env=env, capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stderr)
        manifest = json.loads((output / 'build-manifest.json').read_text())
        self.assertEqual(manifest['status'], 'success')
        self.assertEqual(len(manifest['artifacts']), 3)
        self.assertIn('connection-policy-tests.log', manifest['logs'])
        self.assertIn('transport-tests.log', manifest['logs'])
        self.assertIn('app-model-tests.log', manifest['logs'])
        self.assertIn('network-memory-tests.log', manifest['logs'])
        self.assertIn('memory-plan-tests.log', manifest['logs'])
        self.assertIn('network-memory-model-tests.log', manifest['logs'])
        self.assertIn('artifact-verification.log', manifest['logs'])
        self.assertTrue((output / 'artifact-verification.json').exists())
        result = REAL_RUN([sys.executable, str(root / 'Scripts/verify_artifacts.py'),
                           '--artifacts-dir', str(output), '--source-root', str(root)],
                          env=env, capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stderr)
        with self.subTest('partial publication failure removes copied archives'):
            (fake_bin / 'cp').write_text('#!/bin/sh\ncase "$1" in *iOS-Unsigned.zip) exit 24;; esac\nexec /bin/cp "$@"\n')
            (fake_bin / 'cp').chmod(0o755)
            result = REAL_RUN(['bash', str(root / 'Scripts/build.sh')], env=env, capture_output=True, text=True)
            self.assertEqual(result.returncode, 24, result.stderr)
            self.assertEqual(json.loads((output / 'build-manifest.json').read_text())['status'], 'failed')
            self.assertFalse(list(output.glob('*.zip')))
            self.assertFalse((output / 'artifact-verification.json').exists())
            scratch = result.stderr.split('staging retained: ')[1].splitlines()[0]
            shutil.rmtree(scratch)


class BundledLicenseTests(unittest.TestCase):
    def setUp(self):
        spec = importlib.util.spec_from_file_location('dmg_packager', ROOT / 'Scripts/package-dmg.py')
        self.packager = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(self.packager)
        self.temp = tempfile.TemporaryDirectory(dir='/private/tmp')
        self.addCleanup(self.temp.cleanup)
        self.app = Path(self.temp.name) / 'Fixture.app'
        self.resources = self.app / 'Contents/Resources'
        self.resources.mkdir(parents=True)
        for name, original in self.packager.LICENSE_SOURCES.items():
            shutil.copy(original, self.resources / name)

    def test_complete_license_bytes_match_sources(self):
        hashes = self.packager.verify_bundled_licenses(self.app)
        self.assertEqual(set(hashes), {'LICENSE', 'AutoSZUWeb-LICENSE.txt', 'curl-COPYING.txt'})
        for name, original in self.packager.LICENSE_SOURCES.items():
            self.assertEqual(hashes[name], v.sha256(original))

    def test_each_missing_or_truncated_license_is_rejected(self):
        for name, original in self.packager.LICENSE_SOURCES.items():
            bundled = self.resources / name
            for replacement in (None, original.read_bytes()[:40]):
                with self.subTest(name=name, replacement=replacement):
                    bundled.unlink(missing_ok=True)
                    if replacement is not None:
                        bundled.write_bytes(replacement)
                    with self.assertRaisesRegex(ValueError, 'bundled license: ' + name):
                        self.packager.verify_bundled_licenses(self.app)
                    shutil.copy(original, bundled)

    def test_license_symlink_cannot_depend_on_external_file(self):
        bundled = self.resources / 'LICENSE'
        bundled.unlink()
        bundled.symlink_to(self.packager.LICENSE_SOURCES['LICENSE'])
        with self.assertRaisesRegex(ValueError, 'bundled license: LICENSE'):
            self.packager.verify_bundled_licenses(self.app)


if __name__ == '__main__':
    unittest.main()
