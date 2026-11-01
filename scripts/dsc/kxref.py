#!/usr/bin/env python3
"""Find ADRP+ADD references to given kernelcache cstring file offsets (PRELINK_TEXT -> PLK_TEXT_EXEC). Read-only."""
import struct, sys
p = sys.argv[1]
d = open(p, 'rb').read()
PT_VA, PT_FO = 0xfffffff00527c000, 10813440
TX_VA, TX_FO = 0xfffffff005920000, 17776640
targets = {int(a, 0): (PT_VA + int(a, 0) - PT_FO) for a in sys.argv[2:]}
tset = {}
for fo, va in targets.items(): tset[va] = fo
pages = {va & ~0xfff for va in tset}
end = 0xfffffff006c00000
for va in range(TX_VA, end, 4):
    w = struct.unpack_from('<I', d, va - TX_VA + TX_FO)[0]
    if (w & 0x9f000000) != 0x90000000: continue
    immlo = (w >> 29) & 3; immhi = (w >> 5) & 0x7ffff
    imm = (immhi << 2) | immlo
    if imm & 0x100000: imm -= 0x200000
    page = (va & ~0xfff) + (imm << 12)
    if page not in pages: continue
    rd = w & 31
    for k in range(1, 4):
        w2 = struct.unpack_from('<I', d, va - TX_VA + TX_FO + 4*k)[0]
        if ((w2 >> 5) & 31) == rd and (w2 & 0xff800000) == 0x91000000:
            t = page + ((w2 >> 10) & 0xfff)
            if t in tset: print(hex(t), 'fo', tset[t], 'ref', hex(va))
