#!/usr/bin/env python3
"""Validate a single-file Windows ZIP against external build evidence."""
import argparse
import hashlib
import json
from pathlib import Path, PurePosixPath
import stat
import struct
import zipfile


EMBEDDED_DOCUMENTS = {
    401: "docs/PACKAGE-WINDOWS.md",
    402: "LICENSE",
    403: "docs/DISTRIBUTION-NOTICES.txt",
    404: "docs/licenses/curl-COPYING.txt",
}


def digest(data):
    return hashlib.sha256(data).hexdigest()


def pe_resources(data):
    """Read numeric PE resources without loading or executing the program."""
    def read(fmt, offset):
        if offset < 0 or offset + struct.calcsize(fmt) > len(data):
            raise ValueError("Truncated PE resource data")
        return struct.unpack_from(fmt, data, offset)

    if data[:2] != b"MZ":
        raise ValueError("Executable has no DOS header")
    pe, = read("<I", 0x3c)
    signature, machine, sections = read("<IHH", pe)
    optional_size, = read("<H", pe + 20)
    optional = pe + 24
    if signature != 0x4550 or machine != 0x8664 or read("<H", optional)[0] != 0x20b:
        raise ValueError("The package requires a Windows x64 PE32+ executable")
    if optional_size < 136 or read("<I", optional + 108)[0] < 3:
        raise ValueError("Missing PE resource directory")
    resource_rva, resource_size = read("<II", optional + 128)
    section_table = optional + optional_size

    def rva_offset(rva, size):
        for index in range(sections):
            virtual_size, address, raw_size, raw = read("<IIII", section_table + index * 40 + 8)
            delta = rva - address
            if 0 <= delta and delta + size <= min(max(virtual_size, raw_size), raw_size):
                offset = raw + delta
                if offset + size <= len(data):
                    return offset
        raise ValueError("PE resource lies outside file-backed sections")

    base = rva_offset(resource_rva, resource_size)

    def directory(relative):
        if relative < 0 or relative + 16 > resource_size:
            raise ValueError("Invalid PE resource directory offset")
        named, numbered = read("<HH", base + relative + 12)
        count = named + numbered
        if relative + 16 + count * 8 > resource_size:
            raise ValueError("Invalid PE resource entry count")
        return [read("<II", base + relative + 16 + index * 8) for index in range(count)]

    result = {}
    for kind, type_offset in directory(0):
        if kind & 0x80000000 or kind not in (10, 16):
            continue
        if not type_offset & 0x80000000:
            raise ValueError("Invalid PE resource type")
        for identifier, name_offset in directory(type_offset & 0x7fffffff):
            if identifier & 0x80000000:
                continue
            if not name_offset & 0x80000000:
                raise ValueError("Invalid PE resource identifier")
            versions = []
            for language, entry in directory(name_offset & 0x7fffffff):
                if language & 0x80000000 or entry & 0x80000000 or entry + 16 > resource_size:
                    raise ValueError("Invalid PE resource data entry")
                address, size, _, reserved = read("<IIII", base + entry)
                if reserved:
                    raise ValueError("Invalid PE resource reserved field")
                offset = rva_offset(address, size)
                versions.append(data[offset:offset + size])
            if not versions or any(value != versions[0] for value in versions):
                raise ValueError("Missing or inconsistent localized PE resources")
            if (kind, identifier) in result:
                raise ValueError("Duplicate PE resource identifier")
            result[kind, identifier] = versions[0]
    return result


def verify_version(resources, version, build):
    numeric = tuple(int(part) for part in version.split(".")) + (int(build),)
    if len(numeric) != 4 or any(part < 0 or part > 65535 for part in numeric):
        raise ValueError("Invalid application version")
    resource = resources[16, 1]
    signature = struct.pack("<II", 0xfeef04bd, 0x10000)
    offset = resource.find(signature)
    if offset < 0 or offset + 52 > len(resource):
        raise ValueError("Missing executable fixed version resource")
    file_ms, file_ls, product_ms, product_ls = struct.unpack_from("<IIII", resource, offset + 8)
    expected = (numeric[0] << 16 | numeric[1], numeric[2] << 16 | numeric[3])
    if (file_ms, file_ls) != expected or (product_ms, product_ls) != expected:
        raise ValueError("Executable version differs from the tested manifest version")


def verify_smoke(manifest, manifest_path, executable):
    item = manifest["nativeOfflineSmoke"]
    name = item["name"]
    if not isinstance(name, str) or Path(name).name != name or "\\" in name or "/" in name:
        raise ValueError("Invalid external smoke receipt filename")
    data = (manifest_path.parent / name).read_bytes()
    if len(data) != item["size"] or digest(data) != item["sha256"] or item["executableSha256"] != digest(executable):
        raise ValueError("Smoke receipt or tested executable hash differs from the manifest")
    receipt = json.loads(data.decode("utf-8"))
    if (receipt.get("schemaVersion") != 2 or receipt.get("version") != manifest["version"]
            or receipt.get("build") != manifest["build"] or receipt.get("mode") != "offlineSmoke"
            or receipt.get("passed") is not True or receipt.get("fakeAuthPassed") is not True
            or receipt.get("failure") != "" or receipt.get("formalServicesCreated") != 0):
        raise ValueError("Native smoke receipt does not attest this application version")
    isolation = receipt.get("isolation", {})
    if isolation.get("passed") is not True or any(isolation.get(key) != 0 for key in (
            "productionStorageAccesses", "vaultAccesses", "networkCalls", "wifiCalls",
            "watchersStarted", "autostartWrites")):
        raise ValueError("Native smoke receipt does not attest formal-service isolation")


def verify_package(path, expected_root, manifest_path=None, expected_exe=None, source_root=None):
    root = PurePosixPath(expected_root)
    if (root.name != expected_root or expected_root in ("", ".", "..")
            or "\\" in expected_root or "\x00" in expected_root):
        raise ValueError("Invalid package root")
    if manifest_path is None and expected_exe is None:
        raise ValueError("An external manifest or expected executable is required")
    with zipfile.ZipFile(path) as archive:
        entries = archive.infolist()
        expected_name = expected_root + "/SZUConnect.exe"
        if (len(entries) != 1 or entries[0].filename != expected_name
                or entries[0].orig_filename != expected_name):
            raise ValueError("ZIP must contain only SZUConnect.exe inside the product folder")
        entry = entries[0]
        if entry.is_dir() or stat.S_IFMT(entry.external_attr >> 16) not in (0, stat.S_IFREG):
            raise ValueError("ZIP entry must be a regular file")
        if archive.testzip() is not None:
            raise ValueError("ZIP CRC verification failed")
        executable = archive.read(entry)
    if expected_exe is not None and executable != Path(expected_exe).read_bytes():
        raise ValueError("ZIP executable differs from the expected standalone executable")
    resources = pe_resources(executable)
    if any((10, resource_id) not in resources for resource_id in EMBEDDED_DOCUMENTS):
        raise ValueError("Executable is missing complete embedded license documents")
    manifest = None
    if manifest_path is not None:
        manifest_path = Path(manifest_path)
        manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
        if (manifest.get("schemaVersion") != 2 or manifest.get("architecture") != "x64"
                or manifest.get("configuration") != "Release" or manifest.get("packageRoot") != expected_root
                or expected_root != f"SZUConnect-{manifest['version']}-win-x64"):
            raise ValueError("Invalid external package manifest")
        verify_version(resources, manifest["version"], manifest["build"])
        if manifest.get("files") != [{"name": "SZUConnect.exe", "size": len(executable), "sha256": digest(executable)}]:
            raise ValueError("ZIP inventory or executable bytes differ from the external manifest")
        documents = [{"resourceId": resource_id, "source": source, "size": len(resources[10, resource_id]),
                      "sha256": digest(resources[10, resource_id])}
                     for resource_id, source in EMBEDDED_DOCUMENTS.items()]
        if manifest.get("embeddedDocuments") != documents:
            raise ValueError("Embedded documents differ from the external manifest")
        verify_smoke(manifest, manifest_path, executable)
    if source_root is not None:
        for resource_id, source in EMBEDDED_DOCUMENTS.items():
            if resources[10, resource_id] != (Path(source_root) / source).read_bytes():
                raise ValueError("Embedded document differs from source: " + source)
    return len(entries)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--archive", required=True)
    parser.add_argument("--expected-root", required=True)
    parser.add_argument("--manifest", type=Path)
    parser.add_argument("--expected-exe", type=Path)
    parser.add_argument("--source-root", type=Path)
    args = parser.parse_args()
    try:
        count = verify_package(args.archive, args.expected_root, args.manifest, args.expected_exe, args.source_root)
    except (OSError, ValueError, KeyError, TypeError, struct.error, zipfile.BadZipFile) as error:
        parser.exit(1, "Windows package verification failed: " + str(error) + "\n")
    print(f"Windows package verified: {count} file; inventory, CRC, SHA-256 and embedded documents passed")


if __name__ == "__main__":
    main()
