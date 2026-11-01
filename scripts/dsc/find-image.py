#!/usr/bin/env python3
"""Map a byte offset inside a dyld shared cache subcache file to (VM address, image path).
Usage: find-image.py <cachedir> <subcache-suffix> <file-offset|string> [...]
Read-only. Run with python3 -I."""
import struct, sys, os, mmap
d, suffix = sys.argv[1], sys.argv[2]
base = os.path.join(d, 'dyld_shared_cache_arm64')
def rd(p):
    f = open(p, 'rb'); return mmap.mmap(f.fileno(), 0, access=mmap.ACCESS_READ)
main = rd(base)
assert main[:16].startswith(b'dyld_v1')
mapoff, mapcnt = struct.unpack_from('<II', main, 0x10)
imgoff, imgcnt = struct.unpack_from('<II', main, 0x1c0)
subOff, subCnt = struct.unpack_from('<II', main, 0x188)
mains = [struct.unpack_from('<QQQQII', main, mapoff + i*32) for i in range(mapcnt)]
print('main mappings', [(hex(a), hex(s), hex(o)) for a, s, o, *_ in mains])
subs = []
for i in range(subCnt):
    ent = main[subOff + i*56: subOff + (i+1)*56]
    uuid, vmoff = ent[:16], struct.unpack_from('<Q', ent, 16)[0]
    sfx = ent[24:56].split(b'\0')[0].decode()
    subs.append((vmoff, sfx))
print(len(subs), 'subcaches; first', subs[:3])
sc = [s for s in subs if s[1] == suffix]
if suffix not in ('', 'main'):
    assert sc, 'suffix not found: ' + suffix
    sub = rd(base + suffix)
    smo, smc = struct.unpack_from('<II', sub, 0x10)
    maps = [struct.unpack_from('<QQQQII', sub, smo + i*32) for i in range(smc)]
else:
    maps = mains
print('subcache mappings', [(hex(a), hex(s), hex(o)) for a, s, o, *_ in maps])
images = []
for i in range(imgcnt):
    addr, mt, ino, pathoff = struct.unpack_from('<QQQI', main, imgoff + i*32)  # dyld_cache_image_info
    p = main[pathoff:main.find(b'\0', pathoff)].decode()
    images.append((addr, p))
images.sort()
def vm(off):
    for a, s, o, *_ in maps:
        if o <= off < o + s: return a + off - o
for arg in sys.argv[3:]:
    off = int(arg, 0)
    a = vm(off)
    im = None
    for ad, p in images:
        if ad <= a: im = (ad, p)
    print(hex(off), '-> vmaddr', hex(a) if a else None, 'image', im[1] if im else None, hex(im[0]) if im else '')
