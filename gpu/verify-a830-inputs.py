#!/usr/bin/env python3
"""Check the Adreno 830 bring-up inputs against their pins (SHA-256, size) and the SoC DTS against the Odin vendor DT evidence.

  python3 -I gpu/verify-a830-inputs.py [--root <project>]
Exit status 0 only when every present pin matches; a missing blob is reported (never silently skipped) and fails the check
unless --allow-missing is given."""
import argparse, hashlib, json, sys
from pathlib import Path

def main():
    ap = argparse.ArgumentParser(); ap.add_argument('--root', type=Path, default=Path(__file__).resolve().parents[3])
    ap.add_argument('--allow-missing', action='store_true'); args = ap.parse_args()
    pins = json.loads((Path(__file__).with_name('a830-inputs.json')).read_text())
    bad = 0
    for rel, pin in pins['firmware']['files'].items():
        path = args.root / 'firmware/gpu-fw' / rel
        if not path.exists():
            print(f'MISSING {rel}'); bad += 0 if args.allow_missing else 1; continue
        data = path.read_bytes(); digest = hashlib.sha256(data).hexdigest()
        ok = len(data) == pin['bytes'] and digest == pin['sha256']
        print(('OK      ' if ok else 'MISMATCH ') + f'{rel} {len(data)} B {digest}'); bad += not ok
    vf = pins['odin_vendor_firmware']
    for name, pin in vf['files'].items():
        path = args.root / 'firmware/ota/vendor-fw' / name
        if not path.exists():
            print(f'MISSING ota/vendor-fw/{name}'); bad += 0 if args.allow_missing else 1; continue
        data = path.read_bytes(); ok = len(data) == pin['bytes'] and hashlib.sha256(data).hexdigest() == pin['sha256']
        print(('OK      ' if ok else 'MISMATCH ') + f'ota/vendor-fw/{name} {len(data)} B {hashlib.sha256(data).hexdigest()}'); bad += not ok
    for name, key in (('vendor_boot.img', 'vendor_boot_sha256'), ('dtbo.img', 'dtbo_sha256'), ('vendor.img', 'vendor_img_sha256')):
        path = args.root / 'firmware/ota' / name
        if not path.exists():
            print(f'MISSING firmware/ota/{name}'); bad += 0 if args.allow_missing else 1; continue
        digest = hashlib.sha256(path.read_bytes()).hexdigest(); ok = digest == (pins['odin_dt'].get(key) or vf.get(key))
        print(('OK      ' if ok else 'MISMATCH ') + f'ota/{name} {digest}'); bad += not ok
    # The SoC DTS stages the same windows the Odin vendor DT carries.
    evidence = json.loads((args.root / 'docs/builds/odin3-a830-dt-evidence.json').read_text())
    dtsi = (args.root / 'firmware/UniXNU-sm8750-common/dts/sm8750.dtsi').read_text()
    want = {'gpu': ('0x3d00000', '0x40000'), 'rscc': ('0x3d50000', '0x10000'), 'cx_dbgc': ('0x3d61000', '0x3000'), 'cx_misc': ('0x3d9e000', '0x2000'),
            'gmu': ('0x3d37000', '0x68000'), 'gmu_ao': ('0x3d40000', '0x10000'), 'gpucc': ('0x3d90000', '0x9800'), 'smmu': ('0x3da0000', '0x40000')}
    for name, (base, size) in want.items():
        found = f'0 {base} 0 {size}' in dtsi
        print(('OK      ' if found else 'MISSING ') + f'sm8750.dtsi {name} {base}+{size}'); bad += not found
    for v in evidence['variants']:
        reg = [int(x, 16) for x in v['reg']]
        ok = reg[:2] == [0x3d00000, 0x40000] and [int(x, 16) for x in v['gmu_reg']][:2] == [0x3d37000, 0x68000]
        print(('OK      ' if ok else 'MISMATCH ') + f'vendor DT variant {v["index"]} {v["gpu_model"]} chipid {v["chipid"]}'); bad += not ok
    sys.exit(1 if bad else 0)
main()
