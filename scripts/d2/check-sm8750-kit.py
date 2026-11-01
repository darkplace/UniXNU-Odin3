#!/usr/bin/env python3
"""Compile-check the SM8750 kit headers against the kernel ABI of this board.

The package in this repository's package/ is header-only (GccUfs.h, PeripheralIO.h, Rpmh.h,
SmmuTakeover.h: static inline C). This replays the compiler invocation XNU recorded for
IOPlatformExpert.cpp with one translation unit that includes all four, so a header that drifts from the
kernel ABI fails here and not in a driver.

Run inside the toolchain image (it needs /opt/unixnu-linux):

  python3 scripts/d2/check-sm8750-kit.py
"""
import argparse
import json
import os
import subprocess
import sys
from pathlib import Path

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--tree', type=Path,
                    default=Path(os.environ.get('UNIXNU_TREE', '../UniXNU')).expanduser() / 'out/odin3/ipados-17.7.11-21H461/odin3/tree')
parser.add_argument('--package', type=Path,
                    default=Path(__file__).resolve().parents[2] / 'package')
args = parser.parse_args()

record_path = args.tree / 'build/xnu/ios-obj/DEVELOPMENT_ARM64_BCM2837/iokit/DEVELOPMENT/IOPlatformExpert.cpo.json'
if not record_path.is_file():
    raise SystemExit(f'{record_path} missing: build the kernel first')
record = json.loads(record_path.read_text().strip().rstrip(','))
work = args.tree / 'build/sm8750-kit-check'
work.mkdir(parents=True, exist_ok=True)
source = work / 'KitCheck.cpp'
headers = ['PeripheralIO.h', 'GccUfs.h', 'Rpmh.h', 'SmmuTakeover.h']
source.write_text(''.join(f'#include "{header}"\n' for header in headers)
                  + 'int sm8750_kit_check(void) { return 0; }\n')

command, index, arguments = [], 0, record['arguments']
while index < len(arguments):
    value = arguments[index]
    if value == record['file']:
        command.append(str(source))
    elif value == '-o':
        command += ['-o', str(work / 'KitCheck.o')]
        index += 1
    else:
        command.append(value)
    index += 1
meta = args.tree / 'build/xnu/ios-obj/DEVELOPMENT_ARM64_BCM2837/iokit/DEVELOPMENT'
if (meta / 'meta_features.h').is_file():
    command += ['-I', str(meta)]
command += ['-I', str(args.package), '-Wno-macro-redefined', '-Wno-unused-variable',
            '-Wno-unused-but-set-variable', '-Wno-missing-field-initializers', '-Wno-unused-parameter']

print('compile-check', ', '.join(headers), flush=True)
result = subprocess.run(command, cwd=args.tree, capture_output=True, text=True)
if result.returncode != 0:
    print(result.stderr[-4000:])
    raise SystemExit('the SM8750 kit does not compile against the kernel ABI')
object_path = work / 'KitCheck.o'
print(json.dumps({'ok': True, 'object': str(object_path), 'bytes': object_path.stat().st_size,
                  'headers': headers}, indent=2))
