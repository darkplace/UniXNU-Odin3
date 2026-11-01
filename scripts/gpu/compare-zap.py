#!/usr/bin/env python3
"""Compare the Odin's factory GPU firmware (OTA `vendor` partition, ext4) with linux-firmware's gen80000 set.

Reads the ext4 image with `debugfs` (no mount, no root), dumps /firmware/gen80000_* to firmware/ota/vendor-fw/, and reports for each file
size and SHA-256 on both sides; for the ZAP (a Qualcomm MBN: ELF32 + hash/signature segment) it also compares the ELF headers, the loadable code
segment, the hash table and prints the X.509 chain (subject, issuer, validity, SHA-256) found in the signature segment. Read-only.

  python3 -I compare-zap.py [--root <project>] [--json docs/builds/odin3-zap-compare.json]"""
import argparse, hashlib, json, struct, subprocess
from pathlib import Path

FILES = {'gen80000_gmu.bin': 'qcom/gen80000_gmu.bin', 'gen80000_sqe.fw': 'qcom/gen80000_sqe.fw',
         'gen80000_aqe.fw': 'qcom/gen80000_aqe.fw', 'gen80000_zap.mbn': 'qcom/sm8750/gen80000_zap.mbn'}

def sha(data):
    return hashlib.sha256(data).hexdigest()

def elf_segments(data):
    phoff, = struct.unpack_from('<I', data, 28)
    count, = struct.unpack_from('<H', data, 44)
    return [dict(zip(('type', 'offset', 'vaddr', 'paddr', 'filesz', 'memsz', 'flags', 'align'), struct.unpack_from('<8I', data, phoff + i * 32)))
            for i in range(count)]

def certificates(blob):
    out, i = [], 0
    while True:
        i = blob.find(b'\x30\x82', i)
        if i < 0:
            return out
        length = struct.unpack('>H', blob[i + 2:i + 4])[0] + 4
        if i + length <= len(blob):
            r = subprocess.run(['openssl', 'x509', '-inform', 'DER', '-noout', '-subject', '-issuer', '-startdate', '-enddate'],
                               input=blob[i:i + length], capture_output=True)
            if r.returncode == 0:
                out.append({'offset': i, 'bytes': length, 'sha256': sha(blob[i:i + length]),
                            'fields': dict(line.split('=', 1) for line in r.stdout.decode().strip().splitlines())})
                i += length
                continue
        i += 1

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--root', type=Path, default=Path(__file__).resolve().parents[4])
    ap.add_argument('--json', type=Path)
    args = ap.parse_args()
    ota = args.root / 'firmware/ota'
    vendor_img = ota / 'vendor.img'
    out_dir = ota / 'vendor-fw'
    out_dir.mkdir(exist_ok=True)
    report = {'vendor_img_sha256': sha(vendor_img.read_bytes()), 'vendor_img_bytes': vendor_img.stat().st_size, 'files': {}}
    for name, lf in FILES.items():
        subprocess.run(['debugfs', '-R', f'dump /firmware/{name} {out_dir / name}', str(vendor_img)], check=True, capture_output=True)
        a = (out_dir / name).read_bytes()
        b = (args.root / 'firmware/gpu-fw' / lf).read_bytes()
        report['files'][name] = {'vendor_path': f'/vendor/firmware/{name}', 'vendor_bytes': len(a), 'vendor_sha256': sha(a),
                                 'linux_firmware_path': lf, 'linux_firmware_bytes': len(b), 'linux_firmware_sha256': sha(b), 'identical': a == b}
    za, zb = (out_dir / 'gen80000_zap.mbn').read_bytes(), (args.root / 'firmware/gpu-fw' / FILES['gen80000_zap.mbn']).read_bytes()
    sa, sb = elf_segments(za), elf_segments(zb)
    def part(d, s, i):
        return d[s[i]['offset']:s[i]['offset'] + s[i]['filesz']]
    meta = {'elf_and_program_headers_equal': part(za, sa, 0) == part(zb, sb, 0) and [(x['type'], x['offset'], x['vaddr'], x['filesz'], x['flags']) for x in sa] == [(x['type'], x['offset'], x['vaddr'], x['filesz'], x['flags']) for x in sb],
            'code_segment_equal': part(za, sa, 1) == part(zb, sb, 1), 'code_segment_bytes': sa[1]['filesz'], 'code_segment_sha256': sha(part(za, sa, 1)),
            'hash_table_equal': part(za, sa, 2)[0x18:0x18 + 0xe0] == part(zb, sb, 2)[0x18:0x18 + 0xe0],
            'differing_bytes': sum(x != y for x, y in zip(za, zb)), 'first_difference_offset': next((i for i, (x, y) in enumerate(zip(za, zb)) if x != y), None),
            'vendor_chain': certificates(part(za, sa, 2)), 'linux_firmware_chain': certificates(part(zb, sb, 2))}
    report['zap'] = meta
    text = json.dumps(report, indent=1)
    if args.json:
        args.json.write_text(text + '\n')
    print(text)

main()
