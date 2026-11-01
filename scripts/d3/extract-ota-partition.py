#!/usr/bin/env python3
"""Extract one partition from an Android A/B OTA payload held in a ZIP, over HTTP ranges.

The LineageOS zip stores payload.bin uncompressed, so a partition's data can be fetched without downloading
the whole file: parse the payload manifest for the partition's operations, range-read only their data blobs,
decompress (REPLACE, REPLACE_BZ, REPLACE_XZ) and write the image at its block offsets. The partition hash
from the manifest is verified when present.

  python3 scripts/d3/extract-ota-partition.py <zip-url> uefi out/uefi.img [--payload-offset N]

Supported operations: REPLACE (0), REPLACE_BZ (1), REPLACE_XZ (8), ZERO (6). Anything else is refused: a
partial extraction is worse than none.
"""
import argparse
import bz2
import hashlib
import lzma
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

BLOCK = 4096
REPLACE, REPLACE_BZ, MOVE, BSDIFF, SOURCE_COPY, SOURCE_BSDIFF, ZERO, DISCARD, REPLACE_XZ = 0, 1, 2, 3, 4, 5, 6, 7, 8


def varint(buf, i):
    shift = value = 0
    while True:
        byte = buf[i]
        i += 1
        value |= (byte & 0x7f) << shift
        if not byte & 0x80:
            return value, i
        shift += 7


def fields(buf, start, end):
    i = start
    while i < end:
        key, i = varint(buf, i)
        number, wire = key >> 3, key & 7
        if wire == 0:
            value, i = varint(buf, i)
            yield number, wire, value
        elif wire == 2:
            length, i = varint(buf, i)
            yield number, wire, buf[i:i + length]
            i += length
        elif wire == 5:
            yield number, wire, buf[i:i + 4]
            i += 4
        elif wire == 1:
            yield number, wire, buf[i:i + 8]
            i += 8
        else:
            raise SystemExit(f'unsupported protobuf wire type {wire}')


def fetch(url, start, length):
    result = subprocess.run(['curl', '-sfL', '--max-time', '600', '-r', f'{start}-{start + length - 1}', url],
                            capture_output=True)
    if result.returncode != 0 or len(result.stdout) != length:
        raise SystemExit(f'range {start}+{length} failed: rc={result.returncode} bytes={len(result.stdout)}')
    return result.stdout


parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('url')
parser.add_argument('partition')
parser.add_argument('output', type=Path)
parser.add_argument('--payload-offset', type=int, default=None,
                    help='offset of payload.bin inside the zip (default: search the first 1 MiB)')
parser.add_argument('--header-bytes', type=int, default=1 << 20)
args = parser.parse_args()

head_bytes = fetch(args.url, 0, args.header_bytes)
offset = args.payload_offset
if offset is None:
    offset = head_bytes.find(b'CrAU')
    if offset < 0:
        raise SystemExit('payload.bin (CrAU) not found in the first MiB; pass --payload-offset')
version, = struct.unpack_from('>Q', head_bytes, offset + 4)
manifest_size, = struct.unpack_from('>Q', head_bytes, offset + 12)
signature_size, = struct.unpack_from('>I', head_bytes, offset + 20)
if version != 2:
    raise SystemExit(f'payload version {version}, expected 2')
if offset + 24 + manifest_size > len(head_bytes):
    head_bytes = fetch(args.url, 0, offset + 24 + manifest_size)
manifest = head_bytes[offset + 24:offset + 24 + manifest_size]
data_start = offset + 24 + manifest_size + signature_size
print(f'payload at {offset}, version 2, manifest {manifest_size} B, data at {data_start}')

entry = None
for number, wire, value in fields(manifest, 0, len(manifest)):
    if number != 13 or wire != 2:
        continue
    name = None
    for n2, w2, v2 in fields(value, 0, len(value)):
        if n2 == 1 and w2 == 2:
            name = v2.decode()
    if name == args.partition:
        entry = value
        break
if entry is None:
    raise SystemExit(f'partition {args.partition} not in the manifest')

size = hash_hex = None
operations = []
for number, wire, value in fields(entry, 0, len(entry)):
    if number == 7 and wire == 2:                     # new_partition_info
        for n2, w2, v2 in fields(value, 0, len(value)):
            if n2 == 1 and w2 == 0:
                size = v2
            elif n2 == 2 and w2 == 2:
                hash_hex = v2.hex()
    elif number == 8 and wire == 2:                   # operations
        kind = None
        data_offset = data_length = 0
        dst = []
        for n2, w2, v2 in fields(value, 0, len(value)):
            if n2 == 1 and w2 == 0:
                kind = v2
            elif n2 == 2 and w2 == 0:
                data_offset = v2
            elif n2 == 3 and w2 == 0:
                data_length = v2
            elif n2 == 6 and w2 == 2:
                start = count = 0
                for n3, w3, v3 in fields(v2, 0, len(v2)):
                    if n3 == 1:
                        start = v3
                    elif n3 == 2:
                        count = v3
                dst.append((start, count))
        operations.append((kind, data_offset, data_length, dst))
print(f'{args.partition}: {size} B, {len(operations)} operations, hash {hash_hex}')

image = bytearray(size or 0)
for kind, data_offset, data_length, dst in operations:
    if kind == ZERO:
        data = b''
    else:
        raw = fetch(args.url, data_start + data_offset, data_length)
        if kind == REPLACE:
            data = raw
        elif kind == REPLACE_BZ:
            data = bz2.decompress(raw)
        elif kind == REPLACE_XZ:
            data = lzma.decompress(raw)
        else:
            raise SystemExit(f'operation type {kind} is not supported (partial extraction refused)')
    position = 0
    for start, count in dst:
        chunk = data[position:position + count * BLOCK] if data else bytes(count * BLOCK)
        image[start * BLOCK:start * BLOCK + len(chunk)] = chunk
        position += count * BLOCK
    if data and position != len(data):
        raise SystemExit('operation data does not match its destination extents')

args.output.parent.mkdir(parents=True, exist_ok=True)
args.output.write_bytes(image)
digest = hashlib.sha256(image).hexdigest()
print(f'wrote {args.output} ({len(image)} B), sha256 {digest}')
if hash_hex and hash_hex != digest:
    raise SystemExit(f'partition hash mismatch: manifest {hash_hex}, image {digest}')
print('PASS: partition hash matches the manifest')
