#!/usr/bin/env python3
"""Read the Adreno 830 hardware description out of the Odin 3's own Android vendor_boot DTBs.

Input: the `vendor_boot` partition of the LineageOS odin3 OTA (extracted by scripts/d3/extract-ota-partition.py). Each
concatenated DTB is one SoC variant (qcom,msm-id); the script prints, for every variant that has a KGSL node, the chip id,
register windows, interrupts, clocks, GMU, GPU SMMU context banks, ZAP region and the power levels. Read-only; needs `dtc`/`fdtget`.

  python3 -I extract-odin-gpu-dt.py firmware/ota/vendor_boot.img [--json out.json]
"""
import argparse, hashlib, json, struct, subprocess, sys, tempfile
from pathlib import Path

def dtbs(image):
    d = image.read_bytes()
    assert d[:8] == b'VNDRBOOT', 'not a vendor_boot image'
    version, page = struct.unpack_from('<II', d, 8)
    assert version == 4, version
    ramdisk = struct.unpack_from('<I', d, 24)[0]
    header_end = 8 + 4 * 5 + 2048 + 4 + 16
    header, dtb_size = struct.unpack_from('<II', d, header_end)
    align = lambda x: (x + page - 1) // page * page
    start = align(header) + align(ramdisk)
    blob = d[start:start + dtb_size]
    i = 0
    while i + 8 <= len(blob) and blob[i:i + 4] == b'\xd0\x0d\xfe\xed':
        size = struct.unpack('>I', blob[i + 4:i + 8])[0]
        yield blob[i:i + size]
        i += size

def get(path, node, prop, kind='x'):
    r = subprocess.run(['fdtget', '-t', kind, str(path), node, prop], capture_output=True, text=True)
    return r.stdout.split() if r.returncode == 0 else None

def main():
    ap = argparse.ArgumentParser(); ap.add_argument('image', type=Path); ap.add_argument('--json', type=Path)
    args = ap.parse_args()
    out = {'vendor_boot_sha256': hashlib.sha256(args.image.read_bytes()).hexdigest(), 'variants': []}
    with tempfile.TemporaryDirectory() as tmp:
        for index, blob in enumerate(dtbs(args.image)):
            p = Path(tmp) / f'{index}.dtb'; p.write_bytes(blob)
            gpu = '/soc/qcom,kgsl-3d0@3d00000'
            chip = get(p, gpu, 'qcom,chipid')
            if not chip: continue
            v = {'index': index, 'dtb_sha256': hashlib.sha256(blob).hexdigest(), 'bytes': len(blob),
                 'model': (get(p, '/', 'model', 's') or [''])[0] if False else ' '.join(get(p, '/', 'model', 's') or []),
                 'msm_id': get(p, '/', 'qcom,msm-id'), 'chipid': chip[0], 'gpu_model': ' '.join(get(p, gpu, 'qcom,gpu-model', 's') or []),
                 'compatible': get(p, gpu, 'compatible', 's'), 'reg': get(p, gpu, 'reg'), 'reg_names': get(p, gpu, 'reg-names', 's'),
                 'interrupts': get(p, gpu, 'interrupts', 'u'), 'ubwc_mode': get(p, gpu, 'qcom,ubwc-mode'),
                 'gmu_reg': get(p, '/soc/qcom,gmu@3d37000', 'reg'), 'gmu_irq': get(p, '/soc/qcom,gmu@3d37000', 'interrupts', 'u'),
                 'gmu_clock_names': get(p, '/soc/qcom,gmu@3d37000', 'clock-names', 's'),
                 'gmu_power_domain_names': get(p, '/soc/qcom,gmu@3d37000', 'power-domain-names', 's'),
                 'smmu_reg': get(p, '/soc/kgsl-smmu@3da0000', 'reg'), 'qtb_reg': get(p, '/soc/kgsl-smmu@3da0000/gpu_qtb@3de8000', 'reg'),
                 'zap_region_reg': None}
            regions = subprocess.run(['fdtget', '-l', str(p), '/reserved-memory'], capture_output=True, text=True).stdout.split()
            for r in regions:
                if r.startswith('gpu_microcode'):
                    v['zap_region'] = r; v['zap_region_reg'] = get(p, '/reserved-memory/' + r, 'reg')
            dump = subprocess.run(['dtc', '-q', '-I', 'dtb', '-O', 'dts', str(p)], capture_output=True, text=True).stdout
            start = dump.find('qcom,kgsl-3d0@3d00000 {')
            while start >= 0 and 'qcom,chipid' not in dump[start:start + 3000]:
                start = dump.find('qcom,kgsl-3d0@3d00000 {', start + 10)
            freqs = []
            for chunk in dump[start:start + 40000].split('qcom,gpu-pwrlevel@')[1:]:
                f = [l for l in chunk.splitlines() if 'qcom,gpu-freq' in l]
                if f: freqs.append(int(f[0].split('<')[1].split('>')[0], 16))
            v['pwrlevel_hz'] = freqs
            out['variants'].append(v)
    text = json.dumps(out, indent=1)
    if args.json: args.json.write_text(text + '\n')
    print(text)

main()
