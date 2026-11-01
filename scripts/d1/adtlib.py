"""Decode and check an Apple device tree (ADT) sidecar.

Encoding (UniXNU docs/porting/device-tree.md): every node is
    u32 property_count, u32 child_count,
    property_count × { char name[32]; u32 length; u8 value[length]; padding to 4 },
    then child_count nested nodes. The node's own name is its `name` property.
"""
import os
import struct

DRAM_BASE = 0x80000000
DRAM_SIZE = 0x3E9A5D000
CPUS = 1


def decode(data):
    """Return (root_node, bytes_read)."""
    def node(offset):
        properties, children = struct.unpack_from('<II', data, offset)
        offset += 8
        props = {}
        for _ in range(properties):
            name = data[offset:offset + 32].split(b'\0', 1)[0].decode('utf-8', 'replace')
            length, = struct.unpack_from('<I', data, offset + 32)
            value = data[offset + 36:offset + 36 + length]
            offset += 36 + length
            offset += (-length) % 4
            props[name] = value
        name = props.get('name', b'').split(b'\0', 1)[0].decode('utf-8', 'replace')
        kids = []
        for _ in range(children):
            child, offset = node(offset)
            kids.append(child)
        return {'name': name, 'properties': props, 'children': kids}, offset

    root, end = node(0)
    return root, end


def find(tree, path):
    current = tree
    for part in [p for p in path.split('/') if p]:
        current = next((c for c in current['children'] if c['name'] == part), None)
        if current is None:
            return None
    return current


def checks(tree, adt_bytes):
    """The required nodes of the platform expert plus this board's own values."""
    result = []
    chosen = find(tree, '/chosen')
    if chosen is None:
        return [('chosen', False, 'missing')]
    for key in ('random-seed', 'dram-base', 'dram-size', 'nvram-proxy-data', 'nvram-bank-size'):
        value = chosen['properties'].get(key)
        result.append((f'chosen/{key}', value is not None,
                       f'{len(value)} B' if value else 'missing'))
    if 'dram-base' in chosen['properties'] and 'dram-size' in chosen['properties']:
        base, = struct.unpack('<Q', chosen['properties']['dram-base'])
        size, = struct.unpack('<Q', chosen['properties']['dram-size'])
        result.append(('chosen DRAM is the firmware DDR window',
                       base == DRAM_BASE and size == DRAM_SIZE, f'{hex(base)} + {hex(size)}'))
    memory_map = find(tree, '/chosen/memory-map')
    for key in (('RAMDisk', 'PackedRAMDisk'), ('TrustCache',)):
        # A dense Data seed reserves RAMDisk, a packed one (DBRDISK1) PackedRAMDisk (docs: concepts/payload).
        found = [k for k in key if memory_map is not None and k in memory_map['properties']]
        result.append((f'chosen/memory-map/{"|".join(key)}', bool(found), ', '.join(found)))
    cpus = find(tree, '/cpus')
    cores = [c['name'] for c in cpus['children']] if cpus else []
    result.append(('/cpus', len(cores) == CPUS, ', '.join(cores) or 'missing'))
    arm_io = find(tree, '/arm-io')
    children = [c['name'] for c in arm_io['children']] if arm_io else []
    result.append(('/arm-io', arm_io is not None, ', '.join(children) or 'missing'))
    # The hand-built board exports ufs and ufs-phy; the Pagani-based one only intc (ADT_ARM_IO=intc).
    for name in os.environ.get('ADT_ARM_IO', 'intc,ufs,ufs-phy').split(','):
        result.append((f'arm-io/{name}', name in children, ''))
    for path in ('/defaults', '/options'):
        result.append((path, find(tree, path) is not None, ''))
    return result
