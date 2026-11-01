#!/usr/bin/env python3
"""Grow the 32 MiB Data seed to 1 GiB, set the springboard preferences and pack the DBR.

The macOS-only part of step 8 of scripts/build-os-root.py, split out so the rest of the build can run on
Linux. Same operations, same checks: the container is resized with diskutil, the seed is mounted with
owners on so the preferences stay owned by uid 501 (mobile), and drivers/DarwinBoot/Test/packed_ramdisk.py
packs it for DarwinBoot.
"""
import argparse
import plistlib
import subprocess
import sys
from pathlib import Path

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--tree', type=Path, required=True, help='build tree (out/qemu/tree)')
args = parser.parse_args()
ROOT = args.tree.resolve()
data = ROOT / '.cache/firmware/owner-data'
seed = data / 'owner-data-1g.dbr'
if seed.is_file():
    print('already built:', seed)
    raise SystemExit(0)
raw = data / 'owner-data-1g.raw'
with raw.open('wb') as handle:                     # sparse: only the 32 MiB seed is written
    handle.truncate(1 << 30)
    handle.seek(0)
    handle.write((data / 'owner-data-32.cdr').read_bytes())
try:
    attached = plistlib.loads(subprocess.check_output(['hdiutil', 'attach', '-imagekey', 'diskimage-class=CRawDiskImage',
                                                       '-nomount', '-nobrowse', '-plist', raw]))
    store = Path(attached['system-entities'][0]['dev-entry']).name
    try:
        listing = plistlib.loads(subprocess.check_output(['diskutil', 'apfs', 'list', '-plist']))
        container = next(entry['ContainerReference'] for entry in listing['Containers']
                         if any(s['DeviceIdentifier'] == store for s in entry.get('PhysicalStores', [])))
        subprocess.run(['diskutil', 'apfs', 'resizeContainer', container, '0'], check=True)
    finally:
        subprocess.run(['hdiutil', 'detach', store, '-force'], check=True)
    header = raw.open('rb').read(4096)
    blocks = int.from_bytes(header[40:48], 'little')
    if header[32:36] != b'NXSB' or not (1 << 30) - (64 << 20) <= blocks * 4096 <= 1 << 30:
        raise SystemExit('The grown Data seed is not a 1 GiB APFS container')
    mount = data / 'mount'
    attached = plistlib.loads(subprocess.check_output(['hdiutil', 'attach', '-imagekey', 'diskimage-class=CRawDiskImage',
                                                       '-nobrowse', '-owners', 'on', '-mountpoint', mount, '-plist', raw]))
    try:
        preferences = mount / 'mobile/Library/Preferences/com.apple.springboard.plist'
        values = plistlib.loads(preferences.read_bytes()) if preferences.exists() else {}
        values.update(SBDisableAutoDim=True, SBDontDimOrLockOnAC=True, SBDisableIdleSleep=True)
        preferences.write_bytes(plistlib.dumps(values, fmt=plistlib.FMT_BINARY))
        preferences.chmod(0o600)
        if preferences.stat().st_uid != 501:
            raise SystemExit(f'{preferences} does not belong to uid 501 (mobile)')
    finally:
        subprocess.run(['hdiutil', 'detach', attached['system-entities'][0]['dev-entry']], check=True)
    subprocess.run([sys.executable, ROOT / 'drivers/DarwinBoot/Test/packed_ramdisk.py', raw, seed], check=True)
finally:
    raw.unlink()
print('PASS: 1 GiB Data seed packed in', seed)
