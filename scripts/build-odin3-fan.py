#!/usr/bin/env python3
"""Append the Odin MAX31760 fan provider (odin-fan=0..3, default off) to the embedded display archive.

Runs after build-odin3-touch.py. IOService only: no HID vtable audit applies. The object is
compiled with the same qualified XNU record as every other Stage-7 driver.
"""
import hashlib
import json
from pathlib import Path
import subprocess

root = Path(__file__).resolve().parents[1]
subprocess.run(['python3', str(root / 'scripts/build-stage7-driver.py'), 'OdinFan'], check=True)
directory = root / 'build/pagani-peripherals'
obj = directory / 'OdinFan.o'
late = directory / 'OdinFan.late.o'
subprocess.run(['llvm-objcopy', '--remove-section=__DATA,__mod_init_func',
                '--globalize-symbol=__GLOBAL__sub_I_OdinFan.cpp', str(obj), str(late)], check=True)
archive = root / 'build/native-legacy-display/libNativeLegacyDisplay.late.a'
subprocess.run(['xcrun', 'ar', 'rcs', str(archive), str(late)], check=True)
sources = [root / 'drivers/Stage7Probe' / name for name in ('OdinFan.cpp', 'FanMax31760.h', 'GeniI2c.h')]
report = {'object_sha256': hashlib.sha256(late.read_bytes()).hexdigest(),
          'archive_sha256': hashlib.sha256(archive.read_bytes()).hexdigest(),
          'sources': {str(p.relative_to(root)): hashlib.sha256(p.read_bytes()).hexdigest() for p in sources},
          'hardware_qualified': False, 'default_enabled': False}
(directory / 'odin-fan-build.json').write_text(json.dumps(report, indent=2) + '\n')
display_report_path = archive.with_name('build-report.json')
display_report = json.loads(display_report_path.read_text())
display_report['archive_sha256'] = report['archive_sha256']
display_report['embedded_odin_fan'] = report
for path in sources + [Path(__file__)]:
    display_report['source_sha256'][str(path.relative_to(root))] = hashlib.sha256(path.read_bytes()).hexdigest()
display_report_path.write_text(json.dumps(display_report, indent=2) + '\n')
print('PASS: OdinFan embedded archive; runtime unqualified, opt-in odin-fan=1..3')
