#!/usr/bin/env python3
"""Link stubs for the boot progress layer on a board without the Pagani UFS driver.

drivers/BootProgress/BootProgress.c:30-32 declares native_owner_cache_stage, native_ufs_block_reads and
native_ufs_block_failures as weak externs and reads them only after testing their address and the stage (:745-746).
The Pagani NativeUfs driver defines them; ld64 does not accept an undefined weak reference in the kernel link, so
this board (no UFS driver) links zero-valued definitions. native_owner_cache_stage == 0 keeps the storage row off.
"""
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
out = ROOT / 'build/odin3-link-stubs'
out.mkdir(parents=True, exist_ok=True)
source = out / 'LinkStubs.c'
source.write_text('''#include <stdint.h>
volatile int native_owner_cache_stage;
volatile uint64_t native_ufs_block_reads;
volatile uint64_t native_ufs_block_failures;
''')
obj = out / 'LinkStubs.o'
subprocess.run(['xcrun', 'clang', '-target', 'arm64-apple-ios17.0', '-ffreestanding', '-fno-stack-protector',
                '-Wno-incompatible-sysroot', '-Werror', '-O1', '-c', source, '-o', obj], check=True)
archive = out / 'libOdin3LinkStubs.a'
archive.unlink(missing_ok=True)
subprocess.run(['xcrun', 'ar', 'rcs', archive, obj], check=True)
print(f'PASS: {archive}')
