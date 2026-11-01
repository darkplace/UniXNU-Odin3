#!/usr/bin/env python3
"""Derive the Adreno 830 (Gen8, chip id 0x44050001) hardware-init tables from pinned sources, as the Adreno740 recipe does for A740
(firmware/UniXNU*/scripts/derive-a740-hwinit.py).

Inputs (pinned in a830-inputs.json, fetched from git.kernel.org at the commit recorded there):
  - a8xx_catalog.c: a830_nonctxt_regs, a830_protect_regs, a830_pwrup_reglist_regs, a830_ifpc_reglist_regs, a830_dyn_pwrup_reglist_regs
  - a6xx.xml / adreno_common.xml: register offsets with the exact A8XX variant, and the adreno_pipe enum.
The catalog is GPL-2.0 and the register XML MIT. Only hardware *values* (offsets, constants, masks) are carried into the generated header;
no driver code is copied.

  python3 -I derive-a830-hwinit.py            write include/A830HwInit.h
  python3 -I derive-a830-hwinit.py --check    compare with the committed header (exit 1 on a difference)
"""
import hashlib, json, re, sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[2]
PINS = json.loads((HERE / 'a830-inputs.json').read_text())['linux']
SRC = ROOT / 'firmware/ref-linux' / PINS['commit'] / 'drivers/gpu/drm/msm'
OUT = HERE / 'include/A830HwInit.h'
ORDER = ['A2XX', 'A3XX', 'A4XX', 'A5XX', 'A6XX', 'A7XX', 'A8XX']

def variant_matches(spec, target='A8XX'):
    if spec is None:
        return True
    for part in spec.split(','):
        part = part.strip()
        if '-' in part:
            low, high = part.split('-', 1)
            lo = ORDER.index(low) if low else 0
            hi = ORDER.index(high) if high else len(ORDER) - 1
            if lo <= ORDER.index(target) <= hi:
                return True
        elif part == target:
            return True
    return False

def load_registers():
    """name -> {(offset, array length or None, stride)} for the A8XX variant. a6xx.xml wins over adreno_common.xml (which carries the
    pre-A6XX register of the same name); a name that still has two different offsets inside one file is reported when it is used."""
    regs = {}
    for name in ('a6xx.xml', 'adreno_common.xml'):
        text = (SRC / 'registers/adreno' / name).read_text()
        found = {}
        for m in re.finditer(r'<(reg32|reg64)\s+offset="(0x[0-9a-fA-F]+)"\s+name="([A-Z0-9_]+)"([^>]*)>', text):
            variants = re.search(r'variants="([^"]+)"', m.group(4))
            if variant_matches(variants.group(1) if variants else None):
                found.setdefault(m.group(3), set()).add((int(m.group(2), 16), None, 0))
        for m in re.finditer(r'<array\s+offset="(0x[0-9a-fA-F]+)"\s+name="([A-Z0-9_]+)"\s+stride="(\d+)"\s+length="(\d+)"([^>]*)>', text):
            variants = re.search(r'variants="([^"]+)"', m.group(5))
            if variant_matches(variants.group(1) if variants else None):
                found.setdefault(m.group(2), set()).add((int(m.group(1), 16), int(m.group(4)), int(m.group(3))))
        for key, value in found.items():
            regs.setdefault(key, value)
    return regs

def reg_value(expr, regs):
    """`REG_A8XX_NAME`, `REG_A8XX_NAME + 1`, `REG_A8XX_NAME(7)` or `REG_A8XX_NAME(7) + 1` -> dword offset."""
    m = re.fullmatch(r'REG_A[678]XX_([A-Z0-9_]+?)(?:\((\d+)\))?\s*(?:\+\s*(\d+))?', expr.strip())
    if not m:
        raise SystemExit(f'unparsed register expression: {expr!r}')
    name, index, plus = m.group(1), m.group(2), int(m.group(3) or 0)
    if name not in regs:
        raise SystemExit(f'register {name} has no A8XX definition in the pinned XML')
    if len(regs[name]) != 1:
        raise SystemExit(f'register {name} is ambiguous for A8XX: {sorted(regs[name], key=str)}')
    offset, length, stride = next(iter(regs[name]))
    if index is not None:
        if length is None or int(index) >= length:
            raise SystemExit(f'{name}({index}) out of range')
        offset += int(index) * stride
    return offset + plus

def balanced(text, start):
    depth = 0
    for i in range(start, len(text)):
        depth += text[i] == '{'
        depth -= text[i] == '}'
        if depth == 0:
            return text[start:i + 1]
    raise SystemExit('unbalanced braces')

def array_body(catalog, name):
    m = re.search(r'static const [^=]*\b' + name + r'\[\]\s*=\s*\{', catalog)
    if not m:
        raise SystemExit(f'array {name} not found')
    return balanced(catalog, m.end() - 1)[1:-1]

def strip_comments(text):
    return re.sub(r'//[^\n]*', '', re.sub(r'/\*.*?\*/', '', text, flags=re.S))

def split_top(text):
    items, depth, cur = [], 0, ''
    for ch in text:
        if ch == ',' and depth == 0:
            items.append(cur.strip()); cur = ''
            continue
        depth += ch in '({'
        depth -= ch in ')}'
        cur += ch
    if cur.strip():
        items.append(cur.strip())
    return items

def const(expr):
    expr = expr.strip()
    out = 0
    for term in expr.split('|'):
        term = term.strip()
        m = re.fullmatch(r'BIT\((\d+)\)', term)
        out |= (1 << int(m.group(1))) if m else int(term, 0)
    return out

def pipe_mask(expr, pipes):
    mask = 0
    for term in expr.split('|'):
        m = re.fullmatch(r'\s*BIT\((PIPE_[A-Z0-9_]+)\)\s*', term)
        if not m or m.group(1) not in pipes:
            raise SystemExit(f'unknown pipe term {term!r}')
        mask |= 1 << pipes[m.group(1)]
    return mask

def derive():
    catalog = strip_comments((SRC / 'adreno/a8xx_catalog.c').read_text())
    regs = load_registers()
    common = (SRC / 'registers/adreno/adreno_common.xml').read_text()
    pipes = {m.group(2): int(m.group(1)) for m in re.finditer(r'<value value="(\d+)" name="(PIPE_[A-Z0-9_]+)"/>', common)}
    tables = {}
    for list_name in ('a830_pwrup_reglist_regs', 'a830_ifpc_reglist_regs'):
        tables[list_name] = [reg_value(e, regs) for e in split_top(array_body(catalog, list_name)) if e]
    for list_name in ('a830_nonctxt_regs', 'a830_dyn_pwrup_reglist_regs'):
        rows = []
        for e in split_top(array_body(catalog, list_name)):
            if not e.strip('{} \n\t'):
                continue
            parts = split_top(e.strip()[1:-1])
            rows.append((reg_value(parts[0], regs), const(parts[1]), pipe_mask(parts[2], pipes)))
        tables[list_name] = rows
    protect = []
    for e in split_top(array_body(catalog, 'a830_protect_regs')):
        m = re.fullmatch(r'A6XX_PROTECT_(NORDWR|RDONLY)\((0x[0-9a-fA-F]+),\s*(0x[0-9a-fA-F]+)\)', e)
        if not m:
            raise SystemExit(f'unparsed protect entry {e!r}')
        start, length = int(m.group(2), 16), int(m.group(3), 16)
        value = ((1 << 31) if m.group(1) == 'NORDWR' else 0) | ((length & 0x3fff) << 18) | (start & 0x3ffff)
        protect.append((m.group(1), start, length, value))
    return tables, protect, pipes

def render(tables, protect, pipes):
    sha = hashlib.sha256((SRC / 'adreno/a8xx_catalog.c').read_bytes()).hexdigest()
    lines = ['/* SPDX-License-Identifier: MIT */', '#ifndef A830_HWINIT_H', '#define A830_HWINIT_H', '#include <stdint.h>',
             '/* Generated by devices/odin3-pagani/gpu/derive-a830-hwinit.py. Do not edit by hand.',
             ' * Values only (dword register offsets, constants, masks) from the pinned a8xx_catalog.c (sha256 %s)' % sha,
             ' * and the pinned register XML with exact A8XX variant selection. Pipe masks use the adreno_common.xml enum:',
             ' *   ' + ' '.join(f'{k}={v}' for k, v in sorted(pipes.items(), key=lambda kv: kv[1])) + ' */',
             'typedef struct { uint32_t offset, value, pipes; } A830RegPipe; /* pipes: bit n set = enum adreno_pipe value n */', '']
    for name in ('a830_pwrup_reglist_regs', 'a830_ifpc_reglist_regs'):
        macro = name.upper()
        lines.append(f'static const uint32_t {name}[] = {{')
        lines += [f'    0x{v:05x},' for v in tables[name]]
        lines += ['};', f'#define {macro}_COUNT {len(tables[name])}u', '']
    for name in ('a830_nonctxt_regs', 'a830_dyn_pwrup_reglist_regs'):
        lines.append(f'static const A830RegPipe {name}[] = {{')
        lines += [f'    {{0x{o:05x}, 0x{v:08x}, 0x{p:02x}}},' for o, v, p in tables[name]]
        lines += ['};', f'#define {name.upper()}_COUNT {len(tables[name])}u', '']
    lines.append('/* CP_PROTECT_GLOBAL(n) values: bit31 = read and write blocked, bits 31:18 length, bits 17:0 start. */')
    lines.append('static const uint32_t a830_protect_values[] = {')
    lines += [f'    0x{v:08x}, /* {kind} 0x{s:05x} +0x{n:04x} */' for kind, s, n, v in protect]
    lines += ['};', f'#define A830_PROTECT_COUNT {len(protect)}u', '#endif', '']
    return '\n'.join(lines)

def main():
    text = render(*derive())
    if '--check' in sys.argv:
        if not OUT.exists() or OUT.read_text() != text:
            print('A830HwInit.h differs from its sources'); sys.exit(1)
        print('PASS: A830HwInit.h matches the pinned sources'); return
    OUT.parent.mkdir(parents=True, exist_ok=True)
    OUT.write_text(text)
    print(f'wrote {OUT} ({len(text)} B)')

if __name__ == '__main__':
    main()
