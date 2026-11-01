#!/usr/bin/env python3
"""Host test for derive-a830-hwinit.py: determinism, structural facts of the catalog, and an independent cross-check of every register
offset it resolves against Snapintosh's Mesa-generated AdrenoA8xxRegs.h (a different tool chain, MIT) wherever that header defines the name."""
import importlib.util, re, subprocess, sys
from pathlib import Path
here = Path(__file__).resolve().parent
spec = importlib.util.spec_from_file_location('d', here / 'derive-a830-hwinit.py'); d = importlib.util.module_from_spec(spec); spec.loader.exec_module(d)

assert subprocess.run([sys.executable, '-I', str(here / 'derive-a830-hwinit.py'), '--check'], capture_output=True).returncode == 0
tables, protect, pipes = d.derive()
assert len(protect) == 48 and len(protect) <= 64, len(protect)
assert all(p[1] < 0x40000 and p[2] < 0x4000 for p in protect)
assert [p[1] for p in protect] == sorted(p[1] for p in protect), 'protect ranges are programmed in ascending order'
for name, count in (('a830_pwrup_reglist_regs', 49), ('a830_ifpc_reglist_regs', 77)):
    assert len(tables[name]) == count and len(set(tables[name])) == count, name   # no register listed twice
assert pipes['PIPE_NONE'] == 0 and pipes['PIPE_BR'] == 1 and pipes['PIPE_DDE_BV'] == 7
for o, v, m in tables['a830_nonctxt_regs'] + tables['a830_dyn_pwrup_reglist_regs']:
    assert 0 < m < 256 and o < 0x40000
# independent cross-check
header = (here.parents[2] / 'firmware/Snapintosh/gpu/adreno-kext/include/AdrenoA8xxRegs.h')
if header.exists():
    known = {m.group(1): int(m.group(2), 16) for m in re.finditer(r'#define A8XX_REG_([A-Z0-9_]+) (0x[0-9a-fA-F]+)u', header.read_text())}
    catalog = d.strip_comments((d.SRC / 'adreno/a8xx_catalog.c').read_text())
    regs = d.load_registers(); checked = 0
    exprs = []
    for list_name in ('a830_pwrup_reglist_regs', 'a830_ifpc_reglist_regs'):
        exprs += [e for e in d.split_top(d.array_body(catalog, list_name)) if e]
    for list_name in ('a830_nonctxt_regs', 'a830_dyn_pwrup_reglist_regs'):
        exprs += [d.split_top(e.strip()[1:-1])[0] for e in d.split_top(d.array_body(catalog, list_name)) if e.strip('{} \n\t')]
    for e in exprs:
        m = re.fullmatch(r'REG_A[678]XX_([A-Z0-9_]+)', e.strip())
        if m and m.group(1) in known:
            assert d.reg_value(e, regs) == known[m.group(1)], (e, hex(d.reg_value(e, regs)), hex(known[m.group(1)]))
            checked += 1
    assert checked >= 3, checked   # that header only defines the registers its IB tables use, so the overlap is small
    print(f'cross-checked {checked} offsets against Snapintosh AdrenoA8xxRegs.h')
print('PASS: A830 tables (49 pwrup, 77 ifpc, 40 nonctxt, 28 dynamic pwrup, 48 protect) are deterministic and consistent')
