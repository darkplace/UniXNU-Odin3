#!/usr/bin/env python3
"""Expand the original OS UDIF to the raw disk QEMU serves on Linux (no hdiutil).

scripts/unixnu/udif.py reads the same chunks `hdiutil attach` exposes as /dev/rdiskN, so the expanded file
is byte-for-byte the disk the owner policy signed. Run inside the toolchain image (it needs `lzfse`):

  python3 scripts/q2/expand-os-raw.py <tree>/.cache/firmware/target-volumes/044-76040-038.dmg
"""
import argparse
import hashlib
import json
import sys
from pathlib import Path

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('image', type=Path)
parser.add_argument('--verification', type=Path,
                    help='build/verified-root-os/verification.json to compare the digest with')
args = parser.parse_args()

paths = []
for base in (args.image.absolute().parent, args.image.resolve().parent):
    paths.extend([base, *base.parents])
tree = next((path for path in paths if (path / 'scripts/unixnu/udif.py').is_file()), None)
if tree is None:
    raise SystemExit(f'no build tree with scripts/unixnu/udif.py above {args.image}')
sys.path.insert(0, str(tree / 'scripts'))
from unixnu import udif                                    # noqa: E402

source = udif.Image(args.image)
raw = args.image.with_suffix('.raw')
print(f'source {args.image} {args.image.stat().st_size} B -> raw {source.size} B')
if not (raw.is_file() and raw.stat().st_size == source.size):
    partial = raw.with_suffix('.partial')
    digest = hashlib.sha256()
    written = 0
    with partial.open('wb') as handle:
        for block in source.blocks():
            handle.write(block)
            digest.update(block)
            written += len(block)
    if written != source.size:
        raise SystemExit(f'expanded {written} bytes, the image is {source.size}')
    partial.rename(raw)
    print('expanded', raw, raw.stat().st_size, digest.hexdigest())
else:
    print('already expanded', raw)
digest = hashlib.sha256()
with raw.open('rb') as handle:
    for chunk in iter(lambda: handle.read(8 << 20), b''):
        digest.update(chunk)
report = {'raw': str(raw), 'size': raw.stat().st_size, 'sha256': digest.hexdigest()}
print(json.dumps(report, indent=2))
if args.verification and args.verification.is_file():
    expected = json.loads(args.verification.read_text())
    print('policy image_bytes', expected['image_bytes'], 'image_sha256', expected['image_sha256'])
    if expected['image_bytes'] != report['size'] or expected['image_sha256'] != report['sha256']:
        raise SystemExit('the expanded image does not match the signed policy')
    print('PASS: the expanded raw disk is exactly the image the owner policy signed')
raw.with_suffix('.json').write_text(json.dumps(report, indent=2) + '\n')
