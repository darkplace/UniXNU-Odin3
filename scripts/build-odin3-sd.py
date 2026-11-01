#!/usr/bin/env python3
"""Build OdinSd, the Odin 3 microSD verified root provider, in the storage-provider slot of the kernel.

The kernel's personality table and link script know one storage provider, NativeUfs: the class is instantiated on
IOPlatformDevice nodes (iokit/KernelConfigTables.cpp, XNU_NATIVE_UFS) and its archive is
build/native-ufs/libNativeUfs.a (CONFIG_XNU_NATIVE_UFS_ARCHIVE). OdinSd.cpp defines that class for the sdhci node, so the
rest of the Pagani root chain (NativeVirtioBlock root-binding helper, NativeRamBlock, owner root trust) links unchanged.

  1. compile drivers/OdinSd/OdinSd.cpp with the recorded IOKit flags -> build/native-ufs/NativeUfs.o
  2. build-ufs-storage-bindings.py binds its IOBlockStorageDevice imports to the pinned kernelcache
     -> build/native-ufs/libNativeUfs.a + bindings.json (consumed by the NativeVirtioBlock/NativeRamBlock components)

Run inside the toolchain image from the build tree:  python3 scripts/build-odin3-sd.py --kernelcache FILE
"""
import argparse
import json
import runpy
import struct
import subprocess
import sys
from pathlib import Path

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--kernelcache', type=Path, required=True)
args = parser.parse_args()
root = Path(__file__).resolve().parents[1]
# The device repository is reachable through the kit link (tree/kits/odin3 -> <device>/package).
driver = (root / 'kits/odin3/..').resolve() / 'drivers/OdinSd'
source = driver / 'OdinSd.cpp'
if not source.is_file():
    raise SystemExit(f'{source} not found')
compile_command = runpy.run_path(str(root / 'scripts/build-arm-platform.py'))['compile_command']
record_path = root / 'build/xnu/ios-obj/DEVELOPMENT_ARM64_BCM2837/iokit/DEVELOPMENT/IOPlatformExpert.cpo.json'
record = json.loads(record_path.read_text().strip().rstrip(','))
storage_headers = root / 'build/storage-abi/include'
if not (storage_headers / 'IOKit/storage/IOBlockStorageDevice.h').is_file():
    subprocess.run([sys.executable, str(root / 'scripts/prepare-storage-headers.py')], cwd=root, check=True)
output = root / 'build/native-ufs'
output.mkdir(parents=True, exist_ok=True)
obj = output / 'NativeUfs.o'
command = compile_command(record, source, obj)
meta = root / 'build/xnu/ios-obj/DEVELOPMENT_ARM64_BCM2837/iokit/DEVELOPMENT'
if (meta / 'meta_features.h').is_file():
    command += ['-I', str(meta)]
command += ['-I', str(storage_headers), '-I', str(driver), '-I', str(root / 'kits/PaganiVerifiedRoot/include'),
            '-I', str(root / 'kits'), '-DNATIVE_VERIFIED_BLOCK=1', '-DNATIVE_VERIFIED_BLOCK_MULTI_POLICY=1',
            '-mgeneral-regs-only', '-Wno-macro-redefined', '-Wno-unused-variable', '-Wno-unused-but-set-variable',
            '-Wno-missing-field-initializers', '-Wno-unused-parameter', '-Wno-c++11-narrowing', '-Wno-unused-function']
(output / 'compile-command.json').write_text(json.dumps([str(c) for c in command], indent=2) + '\n')
result = subprocess.run(command, cwd=record['directory'], capture_output=True, text=True)
(output / 'compile.log').write_text(result.stdout + result.stderr)
if result.returncode != 0:
    print((result.stdout + result.stderr)[-8000:])
    raise SystemExit('OdinSd does not compile')
if struct.unpack('<4I', obj.read_bytes()[:16]) != (0xfeedfacf, 0x100000c, 0, 1):
    raise SystemExit('OdinSd: expected an ordinary ARM64 Mach-O object')
defined = subprocess.check_output(['xcrun', 'nm', '-U', '-j', obj], text=True).split()
for symbol in ('_native_ufs_block_authenticates', '_native_ufs_block_reads', '_native_ufs_block_failures'):
    if symbol not in defined:
        raise SystemExit(f'OdinSd does not define {symbol}')
subprocess.run([sys.executable, str(root / 'scripts/build-ufs-storage-bindings.py'), '--kernelcache', str(args.kernelcache)],
               cwd=root, check=True)
