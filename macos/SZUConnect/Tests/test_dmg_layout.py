#!/usr/bin/env python3
"""Offline checks for the installation image layout; no app, credentials or mounts."""
import importlib.util
from pathlib import Path
import plistlib
import struct
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('dmg_layout', ROOT / 'Scripts/dmg_layout.py')
layout = importlib.util.module_from_spec(spec)
spec.loader.exec_module(layout)
package_spec = importlib.util.spec_from_file_location('dmg_package', ROOT / 'Scripts/package-dmg.py')
package = importlib.util.module_from_spec(package_spec)
package_spec.loader.exec_module(package)


def records_from_store(data):
    """Read the allocator map and leaf independently of the write-side record helper."""
    version, magic, offset, size, duplicate = struct.unpack_from('>I4sIII', data)
    if ((version, magic, duplicate) != (1, b'Bud1', offset)
            or size < 32 or size & (size - 1) != 0):
        raise ValueError('Invalid Finder header')
    block_count = struct.unpack_from('>I', data, offset + 4)[0]
    addresses = struct.unpack_from('>' + 'I' * block_count, data, offset + 12)
    toc = offset + 12 + ((block_count + 255) // 256) * 1024
    assert struct.unpack_from('>I', data, toc)[0] == 1
    assert data[toc + 4:toc + 9] == b'\x04DSDB'
    superblock = struct.unpack_from('>I', data, toc + 9)[0]
    root, levels, count, nodes, page_size = struct.unpack_from('>5I', data, (addresses[superblock] & ~31) + 4)
    if (levels, nodes, page_size) != (0, 1, 4096):
        raise ValueError('Invalid Finder leaf-tree height')
    cursor = (addresses[root] & ~31) + 4
    next_node, record_count = struct.unpack_from('>II', data, cursor)
    assert next_node == 0 and record_count == count
    cursor += 8
    records = {}
    for _ in range(count):
        length = struct.unpack_from('>I', data, cursor)[0]
        cursor += 4
        filename = data[cursor:cursor + length * 2].decode('utf-16be')
        cursor += length * 2
        code, kind = struct.unpack_from('>4s4s', data, cursor)
        cursor += 8
        if kind == b'blob':
            length = struct.unpack_from('>I', data, cursor)[0]
            cursor += 4
            value = data[cursor:cursor + length]
            cursor += length
        elif kind == b'long':
            value = struct.unpack_from('>I', data, cursor)[0]
            cursor += 4
        elif kind == b'type':
            value = data[cursor:cursor + 4]
            cursor += 4
        else:
            raise ValueError('Unexpected Finder record')
        records[(filename, code.decode())] = value
    return records


class InstallationLayoutTests(unittest.TestCase):
    def setUp(self):
        self.alias = b'fixture-background-alias'
        self.data = layout.store_bytes(self.alias)
        self.records = records_from_store(self.data)

    def test_opens_in_icon_view_without_sidebar_or_toolbar(self):
        # vstl is the documented "always open" preference, not icvl.
        self.assertEqual(self.records[('.', 'vstl')], b'icnv')
        self.assertNotIn(('.', 'icvl'), self.records)
        window = plistlib.loads(self.records[('.', 'bwsp')])
        self.assertEqual(window['WindowBounds'], '{{180, 120}, {680, 440}}')
        self.assertEqual(window['ViewStyle'], 'icnv')
        for key in ('ShowSidebar', 'ShowToolbar', 'ShowStatusBar', 'ShowPathbar', 'ShowTabView'):
            self.assertFalse(window[key])

    def test_finder_allocator_header_describes_whole_allocated_root_block(self):
        _, _, offset, size, duplicate = struct.unpack_from('>I4sIII', self.data)
        self.assertEqual((offset, size, duplicate), (2048, 2048, 2048))
        malformed = bytearray(self.data)
        struct.pack_into('>I', malformed, 12, 1265)
        with self.assertRaisesRegex(ValueError, 'Invalid Finder header'):
            records_from_store(malformed)

    def test_single_leaf_uses_native_finder_height_zero(self):
        self.assertEqual(struct.unpack_from('>5I', self.data, 36), (2, 0, 6, 1, 4096))
        malformed = bytearray(self.data)
        struct.pack_into('>I', malformed, 40, 1)
        with self.assertRaisesRegex(ValueError, 'leaf-tree height'):
            records_from_store(malformed)

    def test_background_and_native_icon_positions_match_guidance(self):
        icons = plistlib.loads(self.records[('.', 'icvp')])
        self.assertEqual(icons['backgroundType'], 2)
        self.assertEqual(icons['backgroundImageAlias'], self.alias)
        self.assertEqual((icons['iconSize'], icons['textSize']), (104.0, 14.0))
        self.assertEqual(icons['arrangeBy'], 'none')
        self.assertEqual(struct.unpack('>4I', self.records[('深大随连.app', 'Iloc')])[:2], (170, 180))
        self.assertEqual(struct.unpack('>4I', self.records[('应用程序', 'Iloc')])[:2], (510, 180))
        self.assertEqual(len(self.records), 6)

    def test_no_build_machine_identity_in_finder_metadata(self):
        for secret in (b'/Users/', b'/private/', b'/var/', b'localhost'):
            self.assertNotIn(secret, self.data)
        self.assertEqual(self.data, layout.store_bytes(self.alias))

    def test_missing_alias_and_excessive_layout_fail_closed(self):
        with self.assertRaisesRegex(ValueError, 'Missing Finder'):
            layout.store_bytes(b'')
        with self.assertRaisesRegex(ValueError, 'exceeds'):
            layout.store_bytes(b'x' * 4096)

    def test_existing_layout_and_symlink_are_not_overwritten(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            layout.write_layout(root, self.alias)
            with self.assertRaisesRegex(ValueError, 'not be overwritten'):
                layout.write_layout(root, self.alias)
            destination = root / '.DS_Store'
            self.assertEqual(destination.read_bytes(), self.data)
            destination.unlink()
            destination.symlink_to(root / 'missing')
            with self.assertRaisesRegex(ValueError, 'not be overwritten'):
                layout.write_layout(root, self.alias)


class VolumeIconTests(unittest.TestCase):
    def test_custom_icon_flag_is_distinct_from_hidden_flag(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            icon = root / package.VOLUME_ICON_NAME
            icon.write_bytes(b'offline-icon-fixture')
            expected = package.sha(icon)
            for flags, valid in ((0x0400, True), (0x4400, True), (0, False), (0x4000, False)):
                finder_info = bytearray(32)
                finder_info[8:10] = flags.to_bytes(2, 'big')
                with self.subTest(flags=flags), patch.object(package, 'run', return_value=bytes(finder_info).hex().encode()):
                    if valid:
                        package.verify_volume_icon(root, expected)
                    else:
                        with self.assertRaisesRegex(ValueError, 'custom-icon flag'):
                            package.verify_volume_icon(root, expected)

    def test_changed_or_external_icon_is_rejected(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            icon = root / package.VOLUME_ICON_NAME
            icon.write_bytes(b'original-offline-icon')
            expected = package.sha(icon)
            icon.write_bytes(b'changed-offline-icon')
            with self.assertRaisesRegex(ValueError, 'differs from the app icon'):
                package.verify_volume_icon(root, expected)
            icon.unlink()
            icon.symlink_to(root / 'missing')
            with self.assertRaisesRegex(ValueError, 'differs from the app icon'):
                package.verify_volume_icon(root, expected)


class BackgroundAliasPrivacyTests(unittest.TestCase):
    @staticmethod
    def unicode_name(name):
        data = name.encode('utf-16be')
        return struct.pack('>H', len(data) // 2) + data

    def alias(self, extra=(), replace=None, trailing=b'\0\0\0\0'):
        header = bytearray(150)
        struct.pack_into('>H', header, 6, 2)
        # Fictional volume/file IDs are retained for Finder's native lookup.
        struct.pack_into('>I', header, 46, 1234)
        struct.pack_into('>I', header, 114, 5678)
        fields = [(14, self.unicode_name(layout.BACKGROUND_NAME)),
                  (15, self.unicode_name('深大随连 1.0.0')),
                  (16, bytes(8)), (17, bytes(8)),
                  (18, b'/.background/install.png')]
        fields = [(tag, (replace or {}).get(tag, value)) for tag, value in fields] + list(extra)
        data = bytes(header) + b''.join(struct.pack('>hH', tag, len(value)) + value
                                        + bytes(len(value) % 2) for tag, value in fields)
        data += struct.pack('>hH', -1, 0) + trailing
        data = bytearray(data)
        struct.pack_into('>H', data, 4, len(data))
        return bytes(data)

    def test_backing_image_paths_are_removed_without_changing_the_target(self):
        private = b'/Users/' + b'fixture-builder/Projects/private-work/layout.dmg'
        raw = self.alias(extra=[(19, b'/Volumes/fixture-volume'), (20, private), (21, private)])
        cleaned = layout.volume_local_alias(raw)
        expected = self.alias(trailing=b'')
        self.assertEqual(cleaned, expected)
        self.assertEqual(layout.volume_local_alias(cleaned), cleaned)
        self.assertEqual(cleaned[:4] + cleaned[6:150], raw[:4] + raw[6:150])
        records = records_from_store(layout.store_bytes(cleaned))
        icons = plistlib.loads(records[('.', 'icvp')])
        self.assertEqual(icons['backgroundImageAlias'], expected)
        self.assertNotIn(private, layout.store_bytes(cleaned))

    def test_header_size_version_and_terminal_data_are_checked(self):
        original = self.alias()
        for offset, value in ((4, len(original) - 1), (6, 3)):
            malformed = bytearray(original)
            struct.pack_into('>H', malformed, offset, value)
            with self.subTest(offset=offset), self.assertRaisesRegex(ValueError, 'header'):
                layout.volume_local_alias(malformed)
        for data in (b'', bytes(153), self.alias(trailing=b'private')):
            with self.subTest(data=data[-12:]), self.assertRaises(ValueError):
                layout.volume_local_alias(data)

    def test_duplicate_or_truncated_fields_are_rejected(self):
        for extra in ([(18, b'/.background/install.png')], [(20, b'one'), (20, b'two')]):
            with self.subTest(extra=extra), self.assertRaisesRegex(ValueError, 'field'):
                layout.volume_local_alias(self.alias(extra=extra))
        malformed = bytearray(self.alias(trailing=b''))
        struct.pack_into('>H', malformed, 152, len(malformed))
        with self.assertRaisesRegex(ValueError, 'field'):
            layout.volume_local_alias(malformed)
        missing_terminator = bytearray(self.alias(trailing=b'')[:-4])
        struct.pack_into('>H', missing_terminator, 4, len(missing_terminator))
        with self.assertRaisesRegex(ValueError, 'terminator'):
            layout.volume_local_alias(missing_terminator)

    def test_only_the_fixed_volume_local_background_path_is_accepted(self):
        for path in (b'/Users/' + b'fixture-builder/install.png', b'../install.png',
                     b'/.background/../install.png', b'/.background/other.png', b''):
            with self.subTest(path=path), self.assertRaisesRegex(ValueError, 'within this image'):
                layout.volume_local_alias(self.alias(replace={18: path}))

    def test_names_and_timestamps_remain_well_formed(self):
        for replacements in ({14: self.unicode_name('other.png')},
                             {15: self.unicode_name('fixture-builder')},
                             {15: self.unicode_name('深大随连 /private/fixture')},
                             {14: b'\0\x02\0x'}, {14: b'\0\x01\xd8\0'}, {16: b'short'}):
            with self.subTest(replacements=replacements), self.assertRaises(ValueError):
                layout.volume_local_alias(self.alias(replace=replacements))


if __name__ == '__main__':
    unittest.main()
