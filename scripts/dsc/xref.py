#!/usr/bin/env python3
"""Find ADRP+ADD / ADR references to a VM address inside a subcache file and optionally dump a byte window.
Usage: xref.py <subcache-file> <file-base-vmaddr> <lo-vm> <hi-vm> <target-vm> [dump-out vmstart vmend]
Read-only."""
import struct, sys, mmap
f = open(sys.argv[1], 'rb'); m = mmap.mmap(f.fileno(), 0, access=mmap.ACCESS_READ)
base, lo, hi, tgt = [int(x, 0) for x in sys.argv[2:6]]   # base = vm address of file offset 0
res = []
for va in range(lo, hi, 4):
    w = struct.unpack_from('<I', m, va - base)[0]
    if (w & 0x9f000000) == 0x90000000:        # ADRP
        immlo = (w >> 29) & 3; immhi = (w >> 5) & 0x7ffff
        imm = (immhi << 2) | immlo
        if imm & 0x100000: imm -= 0x200000
        page = (va & ~0xfff) + (imm << 12)
        if page == (tgt & ~0xfff):
            rd = w & 31
            for k in range(1, 6):
                w2 = struct.unpack_from('<I', m, va - base + 4*k)[0]
                if (w2 & 0xff800000) == 0x91000000 and ((w2 >> 5) & 31) == rd:
                    if ((w2 >> 10) & 0xfff) == (tgt & 0xfff): res.append(va)
    elif (w & 0x9f000000) == 0x10000000:      # ADR
        immlo = (w >> 29) & 3; immhi = (w >> 5) & 0x7ffff
        imm = (immhi << 2) | immlo
        if imm & 0x100000: imm -= 0x200000
        if va + imm == tgt: res.append(va)
print([hex(x) for x in res])
if len(sys.argv) > 6:
    s, e = int(sys.argv[7], 0), int(sys.argv[8], 0)
    open(sys.argv[6], 'wb').write(m[s - base:e - base])
