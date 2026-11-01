#!/usr/bin/env python3
"""Append the Odin board input provider after Pagani's exact HID ABI qualification.

Embedded in NativeMobileDisplay/Stage7Probe; no new or renamed public component ID.
The existing Pagani peripheral bindings resolve the same IOHIDDevice/Event methods.
"""
import hashlib
import json
from pathlib import Path
import runpy
import subprocess

root = Path(__file__).resolve().parents[1]
subprocess.run(['python3', str(root / 'scripts/build-stage7-driver.py'), 'OdinTouch'], check=True)
directory = root / 'build/pagani-peripherals'
obj = directory / 'OdinTouch.o'
late = directory / 'OdinTouch.late.o'
audit = runpy.run_path(str(root / 'scripts/build-pagani-peripherals.py'))
symbols, slots = audit['symbols_and_slots'](obj, 'OdinTouch', 207)
_, reference = audit['symbols_and_slots'](directory / 'PaganiInput.o', 'PaganiInput', 207)
owned = [i for i, name in enumerate(slots) if name.startswith(('__ZN9OdinTouch', '__ZNK9OdinTouch'))]
assert owned == [0, 1, 7, 14, 86, 87, 111, 171, 173, 180], owned
assert all(name == reference[i] for i, name in enumerate(slots) if i not in owned)
bindings = json.loads((directory / 'abi-audit.json').read_text())
undefined = {name for name, _, kind in symbols if kind == 1 and 'IOHIDDevice' in name}
assert undefined <= bindings['bindings'].keys(), undefined - bindings['bindings'].keys()
subprocess.run(['llvm-objcopy', '--remove-section=__DATA,__mod_init_func',
                '--globalize-symbol=__GLOBAL__sub_I_OdinTouch.cpp', str(obj), str(late)], check=True)
archive = root / 'build/native-legacy-display/libNativeLegacyDisplay.late.a'
subprocess.run(['xcrun', 'ar', 'rcs', str(archive), str(late)], check=True)
sources = list((root / 'drivers/Stage7Probe').glob('*Touch*')) + [root / 'drivers/Stage7Probe/GeniI2c.h', root / 'drivers/Stage7Probe/FocalTech.h', root / 'drivers/Stage7Probe/GpuSurvey.h']
report = {'object_sha256': hashlib.sha256(late.read_bytes()).hexdigest(),
          'archive_sha256': hashlib.sha256(archive.read_bytes()).hexdigest(),
          'original_sha256': bindings['original_sha256'], 'hid_slots': 207, 'owned_slots': owned,
          'sources': {str(p.relative_to(root)): hashlib.sha256(p.read_bytes()).hexdigest() for p in sources},
          'hardware_qualified': False, 'default_enabled': False}
(directory / 'odin-touch-build.json').write_text(json.dumps(report, indent=2) + '\n')
display_report_path = archive.with_name('build-report.json')
display_report = json.loads(display_report_path.read_text())
# Preserve all original ABI qualification, then extend the integrity closure with
# the newly qualified object and its actual inputs. Never bypass the link guard.
display_report['archive_sha256'] = report['archive_sha256']
display_report['embedded_odin_touch'] = report
for path in sources + [Path(__file__)]:
    display_report['source_sha256'][str(path.relative_to(root))] = hashlib.sha256(path.read_bytes()).hexdigest()
display_report_path.write_text(json.dumps(display_report, indent=2) + '\n')
print('PASS: OdinTouch embedded archive, same qualified iPadOS 17.7.11 HID bindings; runtime unqualified')
