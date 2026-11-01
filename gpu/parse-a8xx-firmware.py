#!/usr/bin/env python3
"""Parse the Adreno 830 (Gen8) GMU, SQE/AQE and ZAP images and check them against how the reference driver loads them.

GMU image format and the load rules are those of Linux a6xx_gmu.c (`struct block_header {addr, size, type, value, data[size]}`,
`a6xx_gmu_fw_load`, `a6xx_gmu_memory_probe`) for a7xx/a8xx, pinned in docs/builds/odin3-adreno830-20261029.md:
  - blocks of size 0 are skipped (the image starts with an index of such headers);
  - a block lands in ITCM [0x0, +16K), DTCM [0x10004000, +16K), the instruction cache BO [0x4000, +16M-16K) or the dummy BO
    [0x60000000, +16K); anything else is "failed to match fw block".
Pure host check; nothing here talks to hardware.

  python3 -I parse-a8xx-firmware.py [--root <project>] [--json]   (exit 1 when an image does not fit the model)"""
import argparse, json, struct, sys
from pathlib import Path

REGIONS = (  # name, base, size  (a8xx: dtcm_base 0x10004000, icache 16M-16K at 0x4000, dummy 16K at 0x60000000; debug 28K at 0x60400000)
    ('itcm', 0x00000000, 0x4000), ('dtcm', 0x10004000, 0x4000), ('icache', 0x00004000, (16 << 20) - 0x4000),
    ('dummy', 0x60000000, 0x4000), ('debug', 0x60400000, 7 * 0x1000))
# Linux matches ITCM/DTCM first, then icache, dcache, dummy; `debug` is allocated but not a load target, so it is reported apart.
LOAD_ORDER = ('itcm', 'dtcm', 'icache', 'dummy')

def in_range(addr, base, size):
    return base <= addr < base + size

def parse_gmu(data):
    blocks, offset, index = [], 0, 0
    while offset < len(data):
        if offset + 16 > len(data):
            raise ValueError(f'truncated block header at {offset:#x}')
        addr, size, kind, value = struct.unpack_from('<4I', data, offset)
        if offset + 16 + size > len(data):
            raise ValueError(f'block {index} at {offset:#x} runs past the image ({size:#x} bytes)')
        region = None
        if size:
            for name in LOAD_ORDER:
                _, base, span = next(r for r in REGIONS if r[0] == name)
                if in_range(addr, base, span):
                    region = name; break
            if region and addr + size > next(r for r in REGIONS if r[0] == region)[1] + next(r for r in REGIONS if r[0] == region)[2]:
                region = region + '+overflow'
        blocks.append({'index': index, 'offset': offset, 'addr': addr, 'size': size, 'type': kind, 'value': value, 'region': region})
        offset += 16 + size; index += 1
    return blocks

def check_gmu(data):
    blocks = parse_gmu(data)
    loaded = [b for b in blocks if b['size']]
    bad = [b for b in loaded if b['region'] is None or b['region'].endswith('+overflow')]
    return blocks, loaded, bad

def parse_elf(data):
    if data[:4] != b'\x7fELF' or data[4] != 1:
        raise ValueError('not an ELF32 image')
    phoff, = struct.unpack_from('<I', data, 28); phentsize, phnum = struct.unpack_from('<HH', data, 42)
    segments = []
    for i in range(phnum):
        p_type, p_off, p_vaddr, p_paddr, p_filesz, p_memsz, p_flags, p_align = struct.unpack_from('<8I', data, phoff + i * phentsize)
        segments.append({'type': p_type, 'offset': p_off, 'vaddr': p_vaddr, 'filesz': p_filesz, 'memsz': p_memsz, 'flags': p_flags})
        if p_off + p_filesz > len(data):
            raise ValueError(f'segment {i} runs past the file')
    return {'entry': struct.unpack_from('<I', data, 24)[0], 'machine': struct.unpack_from('<H', data, 18)[0], 'segments': segments}

def main():
    ap = argparse.ArgumentParser(); ap.add_argument('--root', type=Path, default=Path(__file__).resolve().parents[3])
    ap.add_argument('--json', action='store_true'); args = ap.parse_args()
    fw = args.root / 'firmware/gpu-fw/qcom'
    report, status = {}, 0
    gmu = (fw / 'gen80000_gmu.bin').read_bytes()
    blocks, loaded, bad = check_gmu(gmu)
    report['gmu'] = {'bytes': len(gmu), 'blocks': len(blocks), 'index_headers': len(blocks) - len(loaded), 'loaded': len(loaded),
                     'loaded_bytes': sum(b['size'] for b in loaded), 'unmatched': [hex(b['addr']) for b in bad],
                     'by_region': {}}
    for b in loaded:
        report['gmu']['by_region'].setdefault(b['region'], []).append([hex(b['addr']), b['size']])
    status |= bool(bad)
    for name in ('gen80000_sqe.fw', 'gen80000_aqe.fw'):
        d = (fw / name).read_bytes()
        words = struct.unpack_from('<4I', d, 0)
        report[name] = {'bytes': len(d), 'bytes_mod_4': len(d) % 4, 'first_words': [hex(w) for w in words]}
        status |= len(d) % 4 != 0
    zap = (fw / 'sm8750/gen80000_zap.mbn').read_bytes()
    elf = parse_elf(zap)
    report['zap'] = {'bytes': len(zap), 'machine': elf['machine'], 'entry': hex(elf['entry']),
                     'segments': [{k: (hex(v) if k in ('vaddr', 'flags') else v) for k, v in s.items()} for s in elf['segments']]}
    if args.json:
        print(json.dumps(report, indent=1))
    else:
        g = report['gmu']
        print(f"GMU  {g['bytes']} B: {g['blocks']} headers ({g['index_headers']} index, {g['loaded']} loaded = {g['loaded_bytes']} B), unmatched {g['unmatched']}")
        for region, items in g['by_region'].items():
            print(f"     {region:8s} " + ', '.join(f'{a}+{s:#x}' for a, s in items))
        for name in ('gen80000_sqe.fw', 'gen80000_aqe.fw'):
            r = report[name]; print(f"{name[:-3] if name.endswith('.fw') else name}  {r['bytes']} B first words {r['first_words']}")
        z = report['zap']
        print(f"ZAP  {z['bytes']} B ELF32 machine {z['machine']} entry {z['entry']} segments {[(s['type'], s['vaddr'], s['filesz']) for s in z['segments']]}")
    sys.exit(status)

if __name__ == '__main__':
    main()
