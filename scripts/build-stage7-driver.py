#!/usr/bin/env python3
"""Pagani Stage-7 compiler recipe, with the local clang-24 header warning fix.

Retains the actual XNU ABI/compile record and all object deployment checks.
Only -Wunnecessary-virtual-specifier is disabled (OSSymbol is final in XNU).
"""
import argparse
import json
import runpy
from pathlib import Path
import struct
import subprocess


def c_command(command):
    arguments = iter(command)
    result = []
    for value in arguments:
        if value == '-x':
            next(arguments)
        elif value not in ('-fapple-kext', '-fno-rtti', '-fno-exceptions', '-fno-threadsafe-statics',
                           '-fcxx-modules', '-fvisibility-inlines-hidden', '-fsized-deallocation',
                           '-fno-c++-static-destructors') and not value.startswith(('-x', '-std=', '-stdlib=', '--driver-mode=')):
            result.append(value)
    result[1:1] = ['--driver-mode=gcc', '-x', 'c', '-std=gnu11']
    return result


root = Path(__file__).resolve().parents[1]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('component', choices=['NativeLegacyDisplay', 'NativeKeyStoreEvents', 'Stage7Probe',
                                        'PaganiInput', 'PaganiS3910', 'PaganiGeniSpi', 'PaganiPanel', 'OdinTouch', 'OdinFan'])
component = parser.parse_args().component
directory = root / 'build' / ('native-keystore-events' if component == 'NativeKeyStoreEvents' else 'native-legacy-display')
if component.startswith('Pagani') or component in ('OdinTouch', 'OdinFan'):
    directory = root / 'build/pagani-peripherals'
directory.mkdir(parents=True, exist_ok=True)
source = (root / 'drivers/Stage7Probe/Stage7Probe.cpp' if component == 'Stage7Probe' else
          root / 'drivers' / ('NativeMobileDisplay' if component == 'NativeLegacyDisplay' else component) / (component + '.cpp'))
if component in ('PaganiS3910', 'PaganiGeniSpi'):
    source = root / ('drivers/PaganiInput/S3910.c' if component == 'PaganiS3910' else 'drivers/QcomGeniSpi/GeniSpi.c')
if component in ('OdinTouch', 'OdinFan'):
    source = root / 'drivers/Stage7Probe' / (component + '.cpp')
record = json.loads((root / 'build/xnu/ios-obj/DEVELOPMENT_ARM64_BCM2837/iokit/DEVELOPMENT/IOPlatformExpert.cpo.json').read_text().strip().rstrip(','))
obj = directory / (component + '.o')
compile_command = runpy.run_path(str(root / 'scripts/build-arm-platform.py'))['compile_command']
arguments = iter(compile_command(record, source, obj))
command = []
for value in arguments:
    if value == '-Xclang':
        option = next(arguments)
        if not option.startswith('-W'):
            command += [value, option]
    elif not value.startswith('-W'):
        command.append(value)
if source.suffix == '.c':
    command = c_command(command)
command += ['-mgeneral-regs-only', '-Wall', '-Wextra', '-Werror', '-Wno-unused-parameter',
            '-Wno-c99-designator', '-Wno-reorder-init-list', '-Wno-deprecated-enum-enum-conversion',
            '-Wno-unnecessary-virtual-specifier',
            '-I', str(directory), '-I', str(root / '.cache/xnu-work/osfmk')]
if component.startswith('Pagani') or component in ('OdinTouch', 'OdinFan'):
    command += ['-I', str(root / 'kits/Sm8750Platform'), '-I', str(root / 'kits/oneplus13t'),
                '-I', str(root / 'drivers/QcomGeniSpi')]
subprocess.run(command, cwd=record['directory'], check=True)
data = obj.read_bytes()
assert struct.unpack_from('<4I', data) == (0xfeedfacf, 0x100000c, 0, 1), 'Require ARM64 Mach-O object'
offset, deployment = 32, None
for _ in range(struct.unpack_from('<I', data, 16)[0]):
    cmd, size = struct.unpack_from('<II', data, offset)
    if cmd == 0x32:
        deployment = struct.unpack_from('<II', data, offset + 8)
    offset += size
assert deployment == (2, 0x110000), 'Require iOS 17 target'
print(component + ': compile and object ABI checks passed')
