#!/usr/bin/env python3
"""Package verifier regression tests; fixtures never execute a program."""
import copy
import importlib.util
import json
from pathlib import Path
import struct
import tempfile
import unittest
import zipfile


spec = importlib.util.spec_from_file_location("package_verifier", Path(__file__).with_name("verify-package.py"))
verifier = importlib.util.module_from_spec(spec)
spec.loader.exec_module(verifier)


def executable_fixture(documents):
    section = bytearray()

    def reserve(size):
        start = len(section)
        section.extend(bytes(size))
        return start

    def directory(children):
        offset = reserve(16 + 8 * len(children))
        struct.pack_into("<HH", section, offset + 12, 0, len(children))
        for index, (identifier, child) in enumerate(children.items()):
            if isinstance(child, dict):
                target = directory(child) | 0x80000000
            else:
                target = reserve(16)
                data_offset = reserve(len(child))
                section[data_offset:data_offset + len(child)] = child
                struct.pack_into("<IIII", section, target, 0x1000 + data_offset, len(child), 0, 0)
            struct.pack_into("<II", section, offset + 16 + 8 * index, identifier, target)
        return offset

    version = struct.pack("<13I", 0xfeef04bd, 0x10000, 0x10000, 33, 0x10000, 33, *([0] * 7))
    directory({10: {identifier: {1033: data} for identifier, data in documents.items()}, 16: {1: {1033: version}}})
    header = bytearray(512)
    header[:2] = b"MZ"
    struct.pack_into("<I", header, 0x3c, 0x80)
    struct.pack_into("<IHH", header, 0x80, 0x4550, 0x8664, 1)
    struct.pack_into("<H", header, 0x80 + 20, 240)
    optional = 0x80 + 24
    struct.pack_into("<H", header, optional, 0x20b)
    struct.pack_into("<I", header, optional + 108, 16)
    struct.pack_into("<II", header, optional + 128, 0x1000, len(section))
    struct.pack_into("<IIII", header, optional + 240 + 8, len(section), 0x1000, len(section), 512)
    return bytes(header + section)


class PackageVerificationTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="szu-package-verification-")
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.documents = {identifier: (source + "\ncomplete license fixture\n").encode()
                          for identifier, source in verifier.EMBEDDED_DOCUMENTS.items()}
        for identifier, source in verifier.EMBEDDED_DOCUMENTS.items():
            path = self.root / "source" / source
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(self.documents[identifier])
        self.exe = self.root / "expected.exe"
        self.data = executable_fixture(self.documents)
        self.exe.write_bytes(self.data)
        self.package_root = "SZUConnect-1.0.0-win-x64"
        self.archive = self.root / "package.zip"
        self.write_zip()
        self.smoke = {"schemaVersion": 2, "version": "1.0.0", "build": "33", "mode": "offlineSmoke",
                      "passed": True, "fakeAuthPassed": True, "failure": "", "formalServicesCreated": 0,
                      "isolation": {"passed": True, **dict.fromkeys(("productionStorageAccesses", "vaultAccesses",
                                     "networkCalls", "wifiCalls", "watchersStarted", "autostartWrites"), 0)}}
        self.manifest = {"schemaVersion": 2, "version": "1.0.0", "build": "33", "architecture": "x64",
                         "configuration": "Release", "packageRoot": self.package_root,
                         "files": [{"name": "SZUConnect.exe", "size": len(self.data), "sha256": verifier.digest(self.data)}],
                         "embeddedDocuments": [{"resourceId": identifier, "source": source, "size": len(self.documents[identifier]),
                                                "sha256": verifier.digest(self.documents[identifier])}
                                               for identifier, source in verifier.EMBEDDED_DOCUMENTS.items()]}
        self.manifest_path = self.root / "package.manifest.json"
        self.save_manifest()

    def write_zip(self, extra=None, filename=None, data=None):
        with zipfile.ZipFile(self.archive, "w", compression=zipfile.ZIP_STORED) as archive:
            entry = zipfile.ZipInfo()
            entry.filename = filename or self.package_root + "/SZUConnect.exe"
            archive.writestr(entry, data if data is not None else self.data)
            if extra:
                archive.writestr(*extra)

    def save_manifest(self):
        data = json.dumps(self.smoke).encode()
        (self.root / "package.smoke.json").write_bytes(data)
        self.manifest["nativeOfflineSmoke"] = {"name": "package.smoke.json", "size": len(data),
                                                "sha256": verifier.digest(data), "executableSha256": verifier.digest(self.data)}
        self.manifest_path.write_text(json.dumps(self.manifest), encoding="utf-8")

    def verify(self):
        return verifier.verify_package(self.archive, self.package_root, self.manifest_path, self.exe, self.root / "source")

    def test_complete_external_evidence_passes(self):
        self.assertEqual(self.verify(), 1)

    def test_expected_executable_alone_passes(self):
        self.assertEqual(verifier.verify_package(self.archive, self.package_root, expected_exe=self.exe), 1)

    def test_missing_external_evidence_is_rejected(self):
        with self.assertRaisesRegex(ValueError, "external manifest or expected executable"):
            verifier.verify_package(self.archive, self.package_root)

    def test_extra_user_visible_file_is_rejected(self):
        self.write_zip(extra=(self.package_root + "/LICENSE", b"license"))
        with self.assertRaisesRegex(ValueError, "only SZUConnect.exe"):
            self.verify()

    def test_wrong_or_unsafe_zip_paths_are_rejected(self):
        for filename in ("other/SZUConnect.exe", self.package_root + "/../SZUConnect.exe", self.package_root + "\\SZUConnect.exe"):
            with self.subTest(filename=filename):
                self.write_zip(filename=filename)
                with self.assertRaises(ValueError):
                    self.verify()

    def test_standalone_difference_is_rejected(self):
        self.exe.write_bytes(self.data + b"changed")
        with self.assertRaisesRegex(ValueError, "standalone"):
            self.verify()

    def test_manifest_byte_hash_difference_is_rejected(self):
        self.manifest["files"][0]["sha256"] = "0" * 64
        self.save_manifest()
        with self.assertRaisesRegex(ValueError, "executable bytes"):
            self.verify()

    def test_manifest_version_cannot_attest_a_different_executable(self):
        self.manifest["build"] = "34"
        self.save_manifest()
        with self.assertRaisesRegex(ValueError, "Executable version"):
            self.verify()

    def test_receipt_version_and_isolation_are_enforced(self):
        baseline = copy.deepcopy(self.smoke)
        for field, value in (("version", "1.1.0"), ("passed", False), ("formalServicesCreated", 1)):
            with self.subTest(field=field):
                self.smoke = {**baseline, field: value}
                self.save_manifest()
                with self.assertRaisesRegex(ValueError, "Native smoke receipt"):
                    self.verify()
        self.smoke = copy.deepcopy(baseline)
        self.smoke["isolation"]["networkCalls"] = 1
        self.save_manifest()
        with self.assertRaisesRegex(ValueError, "formal-service isolation"):
            self.verify()

    def test_receipt_bytes_and_tested_hash_are_enforced(self):
        (self.root / "package.smoke.json").write_bytes(b"changed")
        with self.assertRaisesRegex(ValueError, "receipt or tested executable"):
            self.verify()
        self.save_manifest()
        self.manifest["nativeOfflineSmoke"]["executableSha256"] = "0" * 64
        self.manifest_path.write_text(json.dumps(self.manifest), encoding="utf-8")
        with self.assertRaisesRegex(ValueError, "receipt or tested executable"):
            self.verify()

    def test_changed_source_notice_is_rejected(self):
        (self.root / "source" / "LICENSE").write_bytes(b"changed")
        with self.assertRaisesRegex(ValueError, "Embedded document differs from source"):
            self.verify()

    def test_missing_embedded_notice_is_rejected(self):
        documents = dict(self.documents)
        del documents[404]
        data = executable_fixture(documents)
        self.write_zip(data=data)
        self.exe.write_bytes(data)
        with self.assertRaisesRegex(ValueError, "missing complete embedded"):
            self.verify()

    def test_zip_crc_corruption_is_rejected(self):
        data = bytearray(self.archive.read_bytes())
        offset = data.index(self.data)
        data[offset + len(self.data) - 1] ^= 1
        self.archive.write_bytes(data)
        with self.assertRaisesRegex(ValueError, "CRC"):
            self.verify()


if __name__ == "__main__":
    unittest.main()
