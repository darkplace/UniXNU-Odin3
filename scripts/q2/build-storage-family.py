#!/usr/bin/env python3
"""Build libIOStorageFamily.a from Apple's open-source IOStorageFamily, as the kernel link needs it.

The QEMU device repository links NativeVirtioBlock/NativeRamBlock, which subclass IOBlockStorageDevice,
but nothing in its archive list compiles that family: on the sheng board the UFS archive carries it. The
family sources are already fetched (refer/IOStorageFamily) and the kernel step prepared their public headers
under build/storage-abi/include, and build-storage-abi.py left the exact compiler invocation of
IOBlockStorageDevice.cpp in build/storage-abi/compile-command.json. This replays that invocation for every
kernel-side source of the family.

Run inside scripts/q2/with-toolchain-ns.sh:

  python3 scripts/q2/build-storage-family.py --tree <build tree>
"""
import argparse
import json
import os
import shutil
import subprocess
import sys
from pathlib import Path

TREE = Path(os.environ.get('PROJECT', '.')) / 'firmware/UniXNU/out/qemu/tree'
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--tree', type=Path, default=TREE)
parser.add_argument('--output', type=Path, default=None)
args = parser.parse_args()
tree = args.tree
record_path = tree / 'build/storage-abi/compile-command.json'
if not record_path.is_file():
    raise SystemExit(f'{record_path} missing: run scripts/build-storage-abi.py first')
arguments = json.loads(record_path.read_text())
record_source = next(value for value in arguments if value.endswith('IOBlockStorageDevice.cpp'))
family = Path(record_source).parent
output = args.output or tree / 'build/storage-family'
output.mkdir(parents=True, exist_ok=True)
# DriverKit-only source: not part of the kernel-side family.
skip = {'IOUserBlockStorageDevice_kext.cpp'}
objects, failed = [], []
for source in sorted(family.glob('*.cpp')):
    if source.name in skip:
        print(f'skip {source.name} (DriverKit)')
        continue
    target = output / (source.stem + '.o')
    command = []
    index = 0
    while index < len(arguments):
        value = arguments[index]
        if value == record_source:
            command.append(str(source))
        elif value == '-o':
            command += ['-o', str(target)]
            index += 1
        else:
            command.append(value)
        index += 1
    # The record carries "-include meta_features.h", a per-component generated header the XNU build leaves in
    # each object directory; the compiler must be able to find it from here.
    meta = tree / 'build/xnu/ios-obj/DEVELOPMENT_ARM64_BCM2837/iokit/DEVELOPMENT'
    if (meta / 'meta_features.h').is_file():
        command += ['-I', str(meta)]
    # Apple's own family sources are not warning-clean under this clang; the kernel build compiles with
    # BUILD_WERROR=0 and the audit record keeps -Werror. These are warnings about Apple's code, not ours.
    command += ['-Wno-macro-redefined', '-Wno-unused-variable', '-Wno-unused-but-set-variable',
                '-Wno-missing-field-initializers', '-Wno-register', '-Wno-unused-parameter']
    print('compile', source.name, flush=True)
    result = subprocess.run(command, cwd=tree, capture_output=True, text=True)
    if result.returncode != 0:
        failed.append(source.name)
        print(result.stderr[-1500:], flush=True)
        continue
    objects.append(target)
if not objects:
    raise SystemExit('no family object compiled')
archive = output / 'libIOStorageFamily.a'
archive.unlink(missing_ok=True)
subprocess.run(['xcrun', 'ar', 'rcs', str(archive)] + [str(path) for path in objects], check=True)
print(json.dumps({'archive': str(archive), 'bytes': archive.stat().st_size,
                  'objects': [path.name for path in objects], 'failed': failed}, indent=2))
if failed:
    print('PARTIAL: some family sources did not compile; the link decides whether they are needed')
