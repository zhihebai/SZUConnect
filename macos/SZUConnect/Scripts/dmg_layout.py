"""Fixed Finder icon-view layout for the Mac installation image.

This writes only a small single-leaf .DS_Store, not a general Finder database.
The Bud1 allocator / DSDB record format is also documented by the ds_store
project: https://ds-store.readthedocs.io/ . No third-party runtime is required.
"""
from pathlib import Path
import plistlib
import struct

WIDTH, HEIGHT = 680, 440
APPLICATIONS_LINK = '应用程序'
BACKGROUND_NAME = 'install.png'
INSTRUCTION = '将「深大随连」拖到「应用程序」'
ICON_LOCATIONS = {'深大随连.app': (170, 180), APPLICATIONS_LINK: (510, 180)}


def volume_local_alias(data):
    """Keep the v2 Finder alias target without its source disk-image location.

    Alias Manager may include a nested alias to the writable image's backing
    file. Finder only needs the target on this image, not that build-host path.
    Preserve volume/file IDs and the volume-local target fields, and reject
    malformed records rather than publishing partially parsed metadata.
    """
    if (len(data) < 154 or len(data) > 65535
            or struct.unpack_from('>HH', data, 4) != (len(data), 2)):
        raise ValueError('Invalid Finder background alias header')
    cursor, fields, selected = 150, {}, []
    while cursor + 4 <= len(data):
        tag, length = struct.unpack_from('>hH', data, cursor)
        cursor += 4
        if tag == -1:
            if length != 0 or any(data[cursor:]):
                raise ValueError('Invalid Finder background alias terminator')
            break
        end = cursor + length
        padded_end = end + length % 2
        if tag in fields or padded_end > len(data):
            raise ValueError('Invalid Finder background alias field')
        value = data[cursor:end]
        fields[tag] = value
        if tag in (14, 15, 16, 17, 18):
            selected.append(struct.pack('>hH', tag, length) + value + bytes(length % 2))
        cursor = padded_end
    else:
        raise ValueError('Missing Finder background alias terminator')
    if fields.get(18) != ('/.background/' + BACKGROUND_NAME).encode('utf-8'):
        raise ValueError('Finder background alias must stay within this image')
    for tag in (14, 15):
        value = fields.get(tag, b'')
        if len(value) < 2 or len(value) != 2 + 2 * struct.unpack_from('>H', value)[0]:
            raise ValueError('Invalid Finder background alias Unicode name')
        try:
            name = value[2:].decode('utf-16be')
        except UnicodeDecodeError as error:
            raise ValueError('Invalid Finder background alias Unicode name') from error
        if ((tag == 14 and name != BACKGROUND_NAME)
                or (tag == 15 and (not name.startswith('深大随连 ')
                                   or any(character in name for character in '/:\\')))):
            raise ValueError('Unexpected Finder background alias name')
    for tag in (16, 17):
        if tag in fields and len(fields[tag]) != 8:
            raise ValueError('Invalid Finder background alias timestamp')
    result = bytearray(data[:150] + b''.join(selected) + struct.pack('>hH', -1, 0))
    struct.pack_into('>H', result, 4, len(result))
    return bytes(result)


def _uint(*values):
    return struct.pack('>' + 'I' * len(values), *values)


def _record(name, code, kind, value):
    encoded = name.encode('utf-16be')
    result = _uint(len(encoded) // 2) + encoded + code.encode('ascii') + kind.encode('ascii')
    if kind == 'blob':
        result += _uint(len(value)) + value
    elif kind == 'long':
        result += _uint(value)
    elif kind == 'type':
        result += value
    else:
        raise ValueError('Unsupported Finder record type')
    return result


def layout_records(background_alias):
    if len(background_alias) < 8:
        raise ValueError('Missing Finder background alias')
    window = {'ShowStatusBar': False, 'WindowBounds': '{{180, 120}, {680, 440}}',
              'ContainerShowSidebar': False, 'PreviewPaneVisibility': False,
              'ShowTabView': False, 'ShowToolbar': False, 'ShowPathbar': False,
              'ShowSidebar': False, 'SidebarWidth': 0, 'ViewStyle': 'icnv'}
    icons = {'viewOptionsVersion': 1, 'backgroundType': 2,
             'backgroundImageAlias': background_alias,
             'backgroundColorRed': 1.0, 'backgroundColorGreen': 1.0, 'backgroundColorBlue': 1.0,
             'gridOffsetX': 0.0, 'gridOffsetY': 0.0, 'gridSpacing': 100.0,
             'arrangeBy': 'none', 'showIconPreview': True, 'showItemInfo': False,
             'labelOnBottom': True, 'textSize': 14.0, 'iconSize': 104.0,
             'scrollPositionX': 0.0, 'scrollPositionY': 0.0}
    records = [('.', 'bwsp', 'blob', plistlib.dumps(window, fmt=plistlib.FMT_BINARY)),
               ('.', 'icvp', 'blob', plistlib.dumps(icons, fmt=plistlib.FMT_BINARY)),
               # vstl is Finder's "Always open in icon view" setting. icvl
               # does not select the view style on current macOS.
               ('.', 'vstl', 'type', b'icnv'), ('.', 'vSrn', 'long', 1)]
    for name, position in ICON_LOCATIONS.items():
        records.append((name, 'Iloc', 'blob', _uint(*position, 0xffffffff, 0xffff0000)))
    return sorted(records, key=lambda record: (record[0].lower(), record[1]))


def store_bytes(background_alias):
    records = layout_records(background_alias)
    # Fixed allocated blocks: 32-byte header, 32-byte DSDB, 2048-byte
    # allocator map, and one 4096-byte leaf. Bud1 offsets exclude its first word.
    leaf = _uint(0, len(records)) + b''.join(_record(*record) for record in records)
    if len(leaf) > 4096:
        raise ValueError('Installation layout exceeds its single Finder page')
    allocated = {0, 5, 11, 12}
    free_lists = b''.join(_uint(1, 1 << power) if power not in allocated and 5 <= power < 31
                          else _uint(0) for power in range(32))
    directory = (_uint(3, 0, 2048 | 11, 32 | 5, 4096 | 12) + bytes(253 * 4)
                 + _uint(1) + b'\x04DSDB' + _uint(1) + free_lists)
    if len(directory) > 2048:
        raise ValueError('Installation allocator map is too large')
    data = bytearray(8196)
    # Conventional opaque Finder allocator header fields.
    unknown = bytes.fromhex('0000100c000000870000200b00000000')
    # The allocator header records the allocated block size, not its used
    # payload length. Finder rejects non-power-of-two root blocks even when
    # a permissive DS_Store reader can decode their records.
    data[:36] = struct.pack('>I4sIII16s', 1, b'Bud1', 2048, 2048, 2048, unknown)
    # DSDB stores tree height: a root that is itself a leaf has height zero.
    data[36:56] = _uint(2, 0, len(records), 1, 4096)
    data[2052:2052 + len(directory)] = directory
    data[4100:4100 + len(leaf)] = leaf
    return bytes(data)


def write_layout(root, background_alias):
    root = Path(root)
    destination = root / '.DS_Store'
    if destination.exists() or destination.is_symlink():
        raise ValueError('Existing Finder layout must not be overwritten')
    destination.write_bytes(store_bytes(background_alias))
