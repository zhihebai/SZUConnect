#!/usr/bin/env python3
"""Create and verify a reproducible public source ZIP after a complete tree scan."""
from __future__ import annotations

import argparse
import hashlib
import os
from pathlib import Path
import re
import sys
import tempfile
import zipfile

from public_scan import MAX_FILE_BYTES, ROOT, external_output, read_deny_terms, scan_content, scan_tree


MANIFEST = "SOURCE-MANIFEST.sha256"


def sha256(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def public_package(root: Path, output: Path, prefix: str = "SZUConnect-source", deny_terms: tuple[str, ...] = ()) -> int:
    root = root.resolve()
    output = external_output(output, root)
    if not re.fullmatch(r"[A-Za-z0-9][A-Za-z0-9._-]*", prefix):
        raise ValueError("Prefix must be one simple directory name")
    if output.exists():
        raise ValueError("Refusing to overwrite existing output")
    findings, files = scan_tree(root, deny_terms)
    if findings:
        categories = sorted({finding.category for finding in findings})
        raise ValueError(f"Source scan failed: {len(findings)} findings ({', '.join(categories)})")
    entries: dict[str, bytes] = {}
    executable: set[str] = set()
    for path in files:
        rel = path.relative_to(root).as_posix()
        if rel == MANIFEST:
            continue
        # Files cannot change into links or escape the source tree after scanning.
        if path.is_symlink() or root not in path.resolve().parents:
            raise ValueError("Source changed after scan; found a link or external file")
        if "\n" in rel or "\r" in rel or "\\" in rel:
            raise ValueError("Unsupported filename in source manifest")
        data = path.read_bytes()
        if len(data) > MAX_FILE_BYTES or scan_content(Path(rel), data, deny_terms):
            raise ValueError("Source changed after scan; content no longer passes public checks")
        entries[rel] = data
        if path.stat().st_mode & 0o111:
            executable.add(rel)
    hashes = {rel: sha256(data) for rel, data in sorted(entries.items())}
    entries[MANIFEST] = "".join(f"{digest}  {rel}\n" for rel, digest in hashes.items()).encode("utf-8")
    output.parent.mkdir(parents=True, exist_ok=True)
    handle, temporary = tempfile.mkstemp(prefix=".source-package-", dir=output.parent)
    os.close(handle)
    staging = Path(temporary)
    try:
        with zipfile.ZipFile(staging, "w", compression=zipfile.ZIP_DEFLATED, compresslevel=9) as archive:
            for rel, data in sorted(entries.items()):
                info = zipfile.ZipInfo(f"{prefix}/{rel}", date_time=(1980, 1, 1, 0, 0, 0))
                info.create_system = 3
                info.compress_type = zipfile.ZIP_DEFLATED
                info.external_attr = (0o100755 if rel in executable else 0o100644) << 16
                archive.writestr(info, data, compresslevel=9)
        with zipfile.ZipFile(staging) as archive:
            expected = {f"{prefix}/{rel}" for rel in entries}
            if set(archive.namelist()) != expected or len(archive.namelist()) != len(expected):
                raise ValueError("Archive inventory differs from source manifest")
            for rel, data in entries.items():
                if sha256(archive.read(f"{prefix}/{rel}")) != sha256(data):
                    raise ValueError("Archive hash verification failed")
        # Exclusive publication prevents races and never overwrites existing files.
        os.link(staging, output)
    finally:
        staging.unlink(missing_ok=True)
    return len(entries)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, default=ROOT)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--prefix", default="SZUConnect-source")
    parser.add_argument("--deny-file", type=Path)
    args = parser.parse_args()
    try:
        root = args.root.expanduser().resolve()
        if not root.is_dir():
            raise ValueError("Source root must be an existing directory")
        if args.deny_file:
            external_output(args.deny_file, root)
        count = public_package(root, args.output, args.prefix, read_deny_terms(args.deny_file))
    except (OSError, ValueError, zipfile.BadZipFile) as error:
        print(f"Public source package failed: {error}", file=sys.stderr)
        return 1
    print(f"Verified public source ZIP: {args.output}; {count} entries")
    return 0


if __name__ == "__main__":
    sys.exit(main())
