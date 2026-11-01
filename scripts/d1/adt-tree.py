#!/usr/bin/env python3
"""Full dump of an ADT sidecar plus the interrupt-parent resolution XNU's IODTMapInterruptsSharing performs.

  python3 -I scripts/d1/adt-tree.py kernel.macho.adt

Mirrors iokit/Kernel/IODeviceTreeSupport.cpp: IODTFindInterruptParent (:559), IODTGetICellCounts (:606),
AddPHandle (:507). Reports every node with `interrupts` whose parent resolves to nothing.
"""
import struct
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import adtlib  # noqa: E402

data = open(sys.argv[1], 'rb').read()
root, end = adtlib.decode(data)
assert end == len(data)


def printable(value):
    if value and all(32 <= b < 127 or b == 0 for b in value) and value[0] != 0:
        return repr(value.rstrip(b'\0').decode())
    if len(value) % 4 == 0 and len(value) <= 64:
        return '<' + ' '.join(f'{x:#x}' for x in struct.unpack(f'<{len(value) // 4}I', value)) + '>'
    return f'{len(value)} B'


by_phandle, parents, nodes = {}, {}, []


def walk(node, path, parent):
    node['path'] = path
    nodes.append(node)
    parents[id(node)] = parent
    # AddPHandle (IODeviceTreeSupport.cpp:507) registers only nodes with BOTH #interrupt-cells and "AAPL,phandle"
    # (gIODTPHandleKey, :161). A plain "phandle" is invisible to FindPHandle.
    ph = node['properties'].get('AAPL,phandle') if '#interrupt-cells' in node['properties'] else None
    if ph is not None and len(ph) == 4:
        by_phandle.setdefault(struct.unpack('<I', ph)[0], node)
    for child in node['children']:
        walk(child, path.rstrip('/') + '/' + child['name'], node)


walk(root, '/', None)
for node in nodes:
    print(node['path'])
    for key, value in node['properties'].items():
        if key != 'name':
            print(f'    {key:<28}{printable(value)}')

print('\n== interrupt resolution (IODTFindInterruptParent)')
bad = 0
for node in nodes:
    props = node['properties']
    if 'interrupts' not in props:
        continue
    ip = props.get('interrupt-parent')
    if ip is not None and len(ip) >= 4:
        phandle = struct.unpack_from('<I', ip)[0]
        parent = by_phandle.get(phandle)
        how = f'interrupt-parent={phandle:#x}'
    elif 'interrupt-controller' not in props:
        parent, how = parents[id(node)], 'tree parent'
    else:
        parent, how = None, 'has interrupt-controller and no interrupt-parent'
    if parent is None:
        bad += 1
        print(f'  NULL PARENT  {node["path"]}  ({how})')
        continue
    cells = parent['properties'].get('#interrupt-cells')
    icells = struct.unpack('<I', cells)[0] if cells is not None and len(cells) == 4 else None
    ok = icells is not None
    print(f'  {"ok  " if ok else "WARN"} {node["path"]} -> {parent["path"]} ({how}) #interrupt-cells={icells}'
          f' interrupts={len(props["interrupts"]) // 4} cells')
    if not ok:
        bad += 1
print(f'\n{bad} problem(s)')
sys.exit(1 if bad else 0)
