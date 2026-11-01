#!/usr/bin/env python3
"""Resolve user-space PCs of the iPadOS 17.7.11 dyld shared cache (SystemOS) to image + symbol.

Read-only. Runs where the SystemOS cryptex is mounted (the MacBook: ~/odin3/mnt/sys). Uses the cache header (images), the
sub-cache mappings and the separate `dyld_shared_cache_arm64.symbols` file (local-symbol nlists per image).

    python3 -I resolve-user-pc.py <com.apple.dyld dir> 0x1a0215764 [...]

A PC below 0x180000000 is not in the cache; give the PC as printed on the page (`LAST USER TRAPS`)."""
import mmap
import struct
import sys


_open_files = []


def open_map(path):
    handle = open(path, 'rb')
    _open_files.append(handle)  # mmap on a bare fileno() breaks once the file object is collected
    return mmap.mmap(handle.fileno(), 0, access=mmap.ACCESS_READ)


def main():
    directory = sys.argv[1].rstrip('/') + '/'
    main_cache = open_map(directory + 'dyld_shared_cache_arm64')
    symbols_file = open_map(directory + 'dyld_shared_cache_arm64.symbols')
    base = 0x180000000
    images_off, images_count = struct.unpack_from('<II', main_cache, 0x1c0)
    images = []
    for index in range(images_count):
        address, _, _, path_off = struct.unpack_from('<QQQI', main_cache, images_off + index * 32)
        end = main_cache.find(b'\0', path_off)
        images.append((address, main_cache[path_off:end].decode()))
    images.sort()
    # local symbols
    local_off, local_size = struct.unpack_from('<QQ', symbols_file, 0x48)
    info = struct.unpack_from('<IIIIII', symbols_file, local_off)
    nlist_off, nlist_count, strings_off, strings_size, entries_off, entries_count = info
    entries = {}
    for index in range(entries_count):
        dylib_off, start, count = struct.unpack_from('<QII', symbols_file, local_off + entries_off + index * 16)
        entries[base + dylib_off] = (start, count)
    for argument in sys.argv[2:]:
        pc = int(argument, 0)
        image = None
        for address, path in images:
            if address <= pc:
                image = (address, path)
            else:
                break
        if not image:
            print(f'{argument}: not in the shared cache'); continue
        best = None
        start, count = entries.get(image[0], (0, 0))
        for index in range(start, start + count):
            strx, ntype, nsect, ndesc, value = struct.unpack_from(
                '<IBBHQ', symbols_file, local_off + nlist_off + index * 16)
            if ntype & 0xe0:  # debug (stab)
                continue
            if value <= pc and (best is None or value > best[0]):
                name_at = local_off + strings_off + strx
                best = (value, symbols_file[name_at:symbols_file.find(b'\0', name_at)].decode())
        offset = pc - image[0]
        if best:
            print(f'{argument}: {image[1]} + {offset:#x}  ({best[1]} + {pc - best[0]:#x})')
        else:
            print(f'{argument}: {image[1]} + {offset:#x}  (no local symbol; {count} local symbols for this image)')


main()
