#!/usr/bin/env python3
"""Find executables of the mounted OS whose code at <vmoffset> is a store, to match an EL0 write abort (WnR=1) with FAR=0.
Usage: scan-store-null.py <os-root> <pc-minus-0x100000000> [more offsets]   Read-only; run where the OS volume is mounted."""
import os, struct, sys
root = sys.argv[1]
offs = [int(x, 0) for x in sys.argv[2:]]
def is_store(w):
    # STR/STP/STUR/STRB/STRH families (integer and SIMD): bit 27 set, bit 22 (L) clear within load/store encodings
    if (w & 0x3b000000) == 0x39000000: return (w & 0x00400000) == 0       # unsigned offset
    if (w & 0x3b200c00) in (0x38000000, 0x38000c00, 0x38000400, 0x38200800): return (w & 0x00c00000) == 0
    if (w & 0x3e000000) == 0x28000000: return (w & 0x00400000) == 0       # STP
    if (w & 0x3f000000) == 0x08000000: return (w & 0x00400000) == 0       # exclusive store
    return False
for d, _, files in os.walk(root):
    if '/Library/Developer' in d: continue
    for f in files:
        p = os.path.join(d, f)
        try:
            if os.path.islink(p) or os.path.getsize(p) < 4096: continue
            with open(p, 'rb') as h:
                head = h.read(32)
                magic, cpu, sub, ftype, ncmds, sizeofcmds = struct.unpack_from('<IiiIII', head)
                if magic != 0xfeedfacf or ftype != 2: continue
                h.seek(32); cmds = h.read(sizeofcmds)
                off = 0; text = None
                for _ in range(ncmds):
                    cmd, size = struct.unpack_from('<II', cmds, off)
                    if cmd == 0x19:
                        name = cmds[off+8:off+24].split(b'\0')[0]
                        vmaddr, vmsize, fileoff, filesize = struct.unpack_from('<QQQQ', cmds, off+24)
                        if name == b'__TEXT': text = (vmaddr, vmsize, fileoff, filesize)
                    off += size
                if not text: continue
                hits = []
                for o in offs:
                    if o < text[3]:
                        h.seek(text[2] + o); w = struct.unpack('<I', h.read(4))[0]
                        hits.append((hex(o), '%08x' % w, 'STORE' if is_store(w) else '-'))
                if hits and any(x[2] == 'STORE' for x in hits):
                    print('%-70s text=%#x %s' % (p[len(root):], text[3], hits))
        except Exception:
            pass
