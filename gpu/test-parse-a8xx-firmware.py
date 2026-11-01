#!/usr/bin/env python3
"""Host tests for parse-a8xx-firmware.py: the load model on synthetic images, then the pinned real images."""
import importlib.util, struct, subprocess, sys
from pathlib import Path
here = Path(__file__).resolve().parent
spec = importlib.util.spec_from_file_location('p', here / 'parse-a8xx-firmware.py'); p = importlib.util.module_from_spec(spec); spec.loader.exec_module(p)

def block(addr, payload, kind=1, value=0):
    return struct.pack('<4I', addr, len(payload), kind, value) + payload

# index headers (size 0) are skipped; the four load targets classify; the debug BO and a hole do not load
image = block(0x0, b'\0' * 8) + block(0x4000, b'\1' * 16) + block(0x10004000, b'\2' * 4) + block(0x60000000, b'\3' * 8) + block(0x1234, b'', 1, 99)
blocks, loaded, bad = p.check_gmu(image)
assert [b['region'] for b in loaded] == ['itcm', 'icache', 'dtcm', 'dummy'] and not bad and len(blocks) == 5
# a block nobody maps is the reference driver's "failed to match fw block"
_, _, bad = p.check_gmu(block(0x50000000, b'\0' * 4))
assert [hex(b['addr']) for b in bad] == ['0x50000000']
# a block that starts in ITCM but leaves it, and one past the icache BO
_, _, bad = p.check_gmu(block(0x3ff8, b'\0' * 16)); assert bad
_, _, bad = p.check_gmu(block(0x1000000, b'\0' * 4)); assert bad
# truncation is an error, never a silent partial load
for broken in (image[:-3], image[:5], block(0, b'\0' * 8)[:-1]):
    try: p.parse_gmu(broken); raise SystemExit('truncated image accepted')
    except ValueError: pass
# ELF parser refuses non-ELF
try: p.parse_elf(b'nope' * 20); raise SystemExit('bad ELF accepted')
except ValueError: pass
# the pinned images, through the command line
r = subprocess.run([sys.executable, '-I', str(here / 'parse-a8xx-firmware.py')], capture_output=True, text=True)
assert r.returncode == 0, r.stdout + r.stderr
assert 'unmatched []' in r.stdout and '4 loaded = 88656 B' in r.stdout
print('PASS: GMU load model (synthetic + pinned gen80000_gmu.bin), SQE/AQE word alignment, ZAP ELF segments')
