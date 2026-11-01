#!/usr/bin/env python3
"""Disassemble a VA window of the pinned kernelcache (PLK_TEXT_EXEC) with llvm-mc; prints absolute addresses.
Usage: kdis.py <kernelcache> <va-start> <va-end>   Read-only."""
import struct, sys, subprocess
d = open(sys.argv[1], 'rb').read()
TX_VA, TX_FO = 0xfffffff005920000, 17776640
s, e = int(sys.argv[2], 0), int(sys.argv[3], 0)
raw = d[s - TX_VA + TX_FO: e - TX_VA + TX_FO]
txt = '\n'.join('0x%02x 0x%02x 0x%02x 0x%02x' % tuple(raw[i:i+4]) for i in range(0, len(raw), 4))
out = subprocess.run(['llvm-mc', '--disassemble', '-triple=arm64e', '-mattr=+v8.3a,+pauth'], input=txt, capture_output=True, text=True).stdout
va = s
for l in out.splitlines():
    if not l.strip() or l.strip().startswith('.'): continue
    print('%x\t%s' % (va, l.strip())); va += 4
