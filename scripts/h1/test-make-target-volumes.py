#!/usr/bin/env python3
"""Host test of make-target-volumes.py: the decode path, on synthetic images, without an IPSW.

  python3 scripts/h1/test-make-target-volumes.py
"""
import importlib.util
import os
from pathlib import Path
import plistlib
import struct
import subprocess
import sys
import tempfile
import zlib

HERE = Path(__file__).resolve().parent
SECTOR = 512


def mish(chunks, first=0, base=0):
    table = bytearray(b'mish' + bytes(204)) + bytearray(40 * len(chunks))
    struct.pack_into('>Q', table, 8, first)
    struct.pack_into('>Q', table, 0x18, base)
    struct.pack_into('>I', table, 0xc8, len(chunks))
    for index, (kind, sector, length, offset, stored) in enumerate(chunks):
        struct.pack_into('>IIQQQQ', table, 0xcc + 40 * index, kind, 0, sector, length, offset, stored)
    return bytes(table)


def udif(path, disk, tables):
    """Write a UDIF whose blkx tables describe `disk`; the chunk bytes are placed at the front."""
    data = bytearray()
    offsets = []
    for table_chunks, payload in tables:
        stitched = []
        for kind, sector, length, raw_bytes in table_chunks:
            offset = len(data)
            data += raw_bytes
            stitched.append((kind, sector, length, offset, len(raw_bytes)))
        offsets.append(stitched)
    body = bytes(data)
    tables = [mish(table, base=0) for table in offsets]
    plist = plistlib.dumps({'resource-fork': {'blkx': [{'Name': f'disk{index}.blkx', 'Data': table}
                                                       for index, table in enumerate(tables)]}})
    plist_offset = len(body)
    trailer = bytearray(512)
    trailer[0:4] = b'koly'
    struct.pack_into('>Q', trailer, 0x18, 0)
    struct.pack_into('>QQ', trailer, 0xd8, plist_offset, len(plist))
    struct.pack_into('>Q', trailer, 0x1ec, len(disk) // SECTOR)
    path.write_bytes(body + plist + bytes(trailer))


def load():
    spec = importlib.util.spec_from_file_location('maketv', HERE / 'make-target-volumes.py')
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def check(name, condition, detail=''):
    print(f'  {"ok  " if condition else "FAIL"} {name}{"  " + detail if not condition and detail else ""}')
    return condition


def main():
    tree = Path(os.environ.get('UNIXNU_TREE', HERE.parents[1] / 'firmware' / 'UniXNU'))
    if not (tree / 'unixnu').is_file():
        sys.exit(f'needs a UniXNU checkout: {tree} (set UNIXNU_TREE)')
    module = load()
    ok = True
    with tempfile.TemporaryDirectory() as temp:
        temp = Path(temp)
        # 1. raw image without koly: the file is its own raw disk.
        raw = temp / 'raw.dmg'
        payload = bytes(range(256)) * (SECTOR * 3 // 256)
        raw.write_bytes(payload)
        digest, size = module.write_raw(raw, temp / 'raw.raw', tree)
        ok &= check('raw passthrough', (temp / 'raw.raw').read_bytes() == payload and size == len(payload),
                    f'size {size}')
        # 2. UDIF: raw chunk, zero-fill chunk and a zlib chunk.
        disk = b'A' * SECTOR + bytes(SECTOR * 2) + b'Z' * SECTOR
        compressed = zlib.compress(b'Z' * SECTOR)
        udif(temp / 'mixed.dmg', disk, [([(1, 0, 1, b'A' * SECTOR),
                                          (0, 1, 2, b''),
                                          (0x80000005, 3, 1, compressed)], None)])
        digest, size = module.write_raw(temp / 'mixed.dmg', temp / 'mixed.raw', tree)
        ok &= check('udif raw + zero-fill + zlib', (temp / 'mixed.raw').read_bytes() == disk,
                    f'{size} bytes')
        # 3. a gap is refused instead of producing a short image.
        udif(temp / 'gap.dmg', b'B' * SECTOR * 2, [([(1, 0, 1, b'B' * SECTOR)], None)])
        try:
            module.write_raw(temp / 'gap.dmg', temp / 'gap.raw', tree)
            ok &= check('gap refused', False, 'no error')
        except Exception as error:  # unixnu.util.Error
            ok &= check('gap refused', 'cover' in str(error), str(error))
        # 4. no IPSW: the command stops with a clear message and writes nothing.
        env = {k: v for k, v in os.environ.items() if k not in ('UNIXNU_IPSW',)}
        result = subprocess.run([sys.executable, str(HERE / 'make-target-volumes.py'), '--out', str(temp / 'o')],
                                capture_output=True, text=True, env=env)
        combined = result.stdout + result.stderr
        ok &= check('no IPSW is an error', result.returncode != 0 and 'no IPSW' in combined, combined.strip())
    print('PASS' if ok else 'FAIL')
    return 0 if ok else 1


if __name__ == '__main__':
    sys.exit(main())
