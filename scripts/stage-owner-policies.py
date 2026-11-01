#!/usr/bin/env python3
"""Stage the owner-signed verified-root policies and the original volume images into the build tree.

The policies (RSA-3072, per 4 KiB block hashes) are produced on macOS by build-owner-os-policy.py /
build-owner-systemos-policy.py (docs/builds/q2-userland-20261028.md: image_sha256 e5634d04… for OS, b4908cde… for
SystemOS) and live in the QEMU build tree of this project. build-pagani-verified-root-probe.py reads them from
<tree>/build/verified-root-{os,systemos} and the SystemOS image from <tree>/.cache/firmware/target-volumes.
Nothing is copied or modified: the inputs are linked read-only.
"""
import os
import sys
from pathlib import Path

root = Path(__file__).resolve().parents[1]
# The device repository is devices/odin3-pagani of the project, reachable through the kit link (tree/kits/odin3 -> <device>/package).
project = Path(os.environ.get('IDRAGON_PROJECT', (root / 'kits/odin3/..').resolve().parents[1])).resolve()
policies = project / 'firmware/UniXNU/out/qemu/tree/build'
volumes = project / 'firmware/UniXNU-qemu/.cache/firmware/target-volumes'
missing = [p for p in (policies / 'verified-root-os/manifest.bin', policies / 'verified-root-systemos/manifest.bin',
                       volumes / '044-76034-038.dmg') if not p.is_file()]
if missing:
    raise SystemExit('missing owner policy inputs: ' + ', '.join(map(str, missing)))
(root / 'build').mkdir(exist_ok=True)
(root / '.cache/firmware').mkdir(parents=True, exist_ok=True)
for name, target in (('build/verified-root-os', policies / 'verified-root-os'),
                     ('build/verified-root-systemos', policies / 'verified-root-systemos'),
                     ('build/verified-root-host', policies / 'verified-root-host'),
                     ('.cache/firmware/target-volumes', volumes)):
    link = root / name
    if link.is_symlink() or link.exists():
        if link.is_symlink() and link.resolve() == target.resolve():
            continue
        link.unlink()
    link.symlink_to(target)
    print(f'{name} -> {target}')
print('PASS: owner policy inputs staged')
