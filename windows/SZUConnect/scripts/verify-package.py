#!/usr/bin/env python3
"""Validate every Windows package entry, CRC and manifest hash without extracting."""
import argparse
import hashlib
import json
from pathlib import PurePosixPath
import stat
import zipfile


def verify_package(path, expected_root):
    root = PurePosixPath(expected_root)
    if (root.name != expected_root or expected_root in ("", ".", "..")
            or "\\" in expected_root or "\x00" in expected_root):
        raise ValueError("Invalid package root")
    with zipfile.ZipFile(path) as archive:
        entries = archive.infolist()
        names = [entry.filename for entry in entries]
        if len(names) != len(set(names)):
            raise ValueError("Duplicate ZIP entries")
        prefix = expected_root + "/"
        if any(not name.startswith(prefix) or ".." in PurePosixPath(name).parts
               or PurePosixPath(name).is_absolute() or "\\" in name
               or PurePosixPath(name).as_posix() != name for name in names):
            raise ValueError("Unsafe or unexpected ZIP path")
        if any(entry.is_dir() or stat.S_IFMT(entry.external_attr >> 16) not in (0, stat.S_IFREG)
               for entry in entries):
            raise ValueError("ZIP entries must be regular files, not directories or symbolic links")
        info_dir = "说明与许可/"
        required = {"SZUConnect.exe"} | {info_dir + name for name in (
            "README.md", "LICENSE", "THIRD-PARTY-NOTICES.md", "curl-COPYING.txt",
            "PACKAGE-MANIFEST.json", "WINDOWS-SMOKE.json")}
        if set(names) != {prefix + name for name in required}:
            raise ValueError("ZIP must contain exactly the executable and six files in the information directory")
        if archive.testzip() is not None:
            raise ValueError("ZIP CRC verification failed")
        manifest_relative = info_dir + "PACKAGE-MANIFEST.json"
        manifest_name = prefix + manifest_relative
        manifest = json.loads(archive.read(manifest_name).decode("utf-8"))
        if manifest.get("schemaVersion") != 1 or manifest.get("architecture") != "x64" or manifest.get("configuration") != "Release":
            raise ValueError("Invalid package manifest")
        if expected_root != f"SZUConnect-{manifest['version']}-{manifest['build']}-win-x64":
            raise ValueError("Package name and manifest version differ")
        expected = {manifest_name}
        for item in manifest["files"]:
            name = item["name"]
            if (not isinstance(name, str) or PurePosixPath(name).is_absolute()
                    or ".." in PurePosixPath(name).parts or "\\" in name
                    or PurePosixPath(name).as_posix() != name
                    or name not in required - {manifest_relative}):
                raise ValueError("Invalid manifest filename")
            entry = prefix + name
            if entry in expected:
                raise ValueError("Duplicate manifest filename")
            expected.add(entry)
            data = archive.read(entry)
            if len(data) != item["size"] or hashlib.sha256(data).hexdigest() != item["sha256"]:
                raise ValueError("Package bytes do not match the manifest")
        if set(names) != expected:
            raise ValueError("ZIP inventory differs from the manifest")
        return len(names)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--archive", required=True)
    parser.add_argument("--expected-root", required=True)
    args = parser.parse_args()
    try:
        count = verify_package(args.archive, args.expected_root)
    except (OSError, ValueError, KeyError, zipfile.BadZipFile) as error:
        parser.exit(1, "Windows package verification failed: " + str(error) + "\n")
    print(f"Windows package verified: {count} files; inventory, CRC and SHA-256 passed")


if __name__ == "__main__":
    main()
