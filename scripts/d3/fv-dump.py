#!/usr/bin/env python3
"""List and extract the FFS files of a UEFI firmware volume inside a vendor UEFI image.

The Odin 3's `uefi` partition (from the public LineageOS OTA) is an ELF whose payload is a firmware volume:
`_FVH` at +0x28 of the FV header, FFS files with an EFI_FFS_FILE_HEADER, and sections inside them. The
vendor stores the DXE volume as a GUID-defined LZMA section (EE4E5898-3914-4259-9D6E-DC7BD79403CF), so the
walker decompresses (LZMA or the Tiano format of EFI_SECTION_COMPRESSION) and descends.

  python3 scripts/d3/fv-dump.py uefi.img --list --tools <BaseTools bin dir>
  python3 scripts/d3/fv-dump.py uefi.img --extract out/drivers --tools <BaseTools bin dir>
"""
import argparse
import struct
import subprocess
import tempfile
from pathlib import Path

FILE_TYPES = {0x01: 'RAW', 0x02: 'FREEFORM', 0x03: 'SECURITY_CORE', 0x04: 'PEI_CORE', 0x05: 'DXE_CORE',
              0x06: 'PEIM', 0x07: 'DRIVER', 0x08: 'COMBINED_PEIM_DRIVER', 0x09: 'APPLICATION',
              0x0A: 'SMM', 0x0B: 'FIRMWARE_VOLUME_IMAGE', 0x0C: 'COMBINED_SMM_DXE', 0x0D: 'SMM_CORE',
              0x0E: 'MM_STANDALONE', 0x0F: 'MM_CORE_STANDALONE'}
COMPRESSION, GUID_DEFINED, PE32, TE, PIC, DXE_DEPEX, UI, FV_IMAGE, RAW_SECTION = 0x01, 0x02, 0x10, 0x12, 0x11, 0x13, 0x15, 0x17, 0x19
LZMA_GUID = 'EE4E5898-3914-4259-9D6E-DC7BD79403CF'
TOOLS = None


def guid(raw):
    d1, d2, d3 = struct.unpack_from('<IHH', raw, 0)
    return f'{d1:08x}-{d2:04x}-{d3:04x}-{raw[8:10].hex()}-{raw[10:16].hex()}'.upper()


def run(tool, payload):
    if not TOOLS:
        return None
    with tempfile.TemporaryDirectory() as work:
        source, target = Path(work) / 'in.bin', Path(work) / 'out.bin'
        source.write_bytes(payload)
        result = subprocess.run([str(Path(TOOLS) / tool), '-d', '-o', str(target), str(source)],
                                capture_output=True, text=True)
        if result.returncode != 0 or not target.is_file():
            return None
        return target.read_bytes()


def unpack(section_type, body):
    """Return the decompressed bytes of a compression/GUID-defined section, or None."""
    if section_type == COMPRESSION:                    # EFI_SECTION_COMPRESSION: Tiano
        if len(body) < 5 or body[4] != 1:
            return None
        return run('TianoCompress', body[5:])
    if section_type == GUID_DEFINED:                   # EFI_SECTION_GUID_DEFINED
        if len(body) < 24:
            return None
        name = guid(body[4:20])
        data_offset, attributes = struct.unpack_from('<HH', body, 20)
        if name != LZMA_GUID or not attributes & 1:
            return None
        return run('LzmaCompress', body[data_offset:])
    return None


def sections(blob, start, end):
    at = start
    while at + 4 <= end:
        size, = struct.unpack_from('<I', blob, at)
        size &= 0xFFFFFF
        kind = blob[at + 3]
        if size < 4:
            return
        yield kind, blob[at + 4:at + size]
        at += (size + 3) & ~3


def stream(blob):
    """A section stream: yields (label, pe32) and descends into FV images and compressed sections."""
    label, pe32 = None, []
    for section_type, data in sections(blob, 0, len(blob)):
        if section_type == UI:
            label = data.split(b'\0', 1)[0].decode('utf-8', 'replace')
        elif section_type in (PE32, TE, PIC):
            pe32.append(data)
        elif section_type == FV_IMAGE:
            yield from walk(data)
        elif section_type in (COMPRESSION, GUID_DEFINED):
            inner = unpack(section_type, data)
            if inner:
                yield from stream(inner)
    if label or pe32:
        yield label, (pe32[0] if pe32 else None)


def walk(blob):
    """Yield (type, guid, ui_name, pe32) for every FFS file of every firmware volume in blob."""
    heading = blob.find(b'_FVH')
    while heading >= 0:
        base = heading - 0x28
        length, = struct.unpack_from('<Q', blob, base + 0x20)
        header, = struct.unpack_from('<H', blob, base + 0x30)
        at, end = base + header, min(base + length, len(blob))
        while at + 24 <= end:
            kind = blob[at + 18]
            size = int.from_bytes(blob[at + 20:at + 23], 'little')
            if not (kind in FILE_TYPES and 24 <= size <= end - at):
                at += 8
                continue
            for label, pe32 in stream(blob[at + 24:at + size]):
                yield kind, guid(blob[at:at + 16]), label, pe32
            at += (size + 7) & ~7
        heading = blob.find(b'_FVH', heading + 4)


parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('image', type=Path)
parser.add_argument('--list', action='store_true')
parser.add_argument('--extract', type=Path)
parser.add_argument('--tools', type=Path, help='BaseTools bin directory with LzmaCompress and TianoCompress')
args = parser.parse_args()
TOOLS = str(args.tools) if args.tools else None
rows = list(walk(args.image.read_bytes()))
drivers = [row for row in rows if row[3]]
print(f'{args.image.name}: {len(rows)} FFS files, {len(drivers)} with a PE32 image')
if args.list:
    for kind, name, label, pe32 in rows:
        print(f'  {FILE_TYPES.get(kind, hex(kind)):12} {label or "-":32} {name} {len(pe32) if pe32 else 0} B')
if args.extract:
    args.extract.mkdir(parents=True, exist_ok=True)
    for kind, name, label, pe32 in drivers:
        # The vendor's UI sections carry one-letter labels, so the GUID is the safe file name.
        stem = label if label and len(label) > 3 else name
        base = stem.replace('/', '_')
        if not base.lower().endswith('.efi'):
            base += '.efi'
        (args.extract / base).write_bytes(pe32)
    print(f'extracted {len(drivers)} PE32 images to {args.extract}')
