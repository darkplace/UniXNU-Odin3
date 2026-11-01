#!/usr/bin/env python3
"""Build p2 (OS) and p3 (SystemOS) raw volumes from the target IPSW, without hdiutil.

  scripts/h1/make-target-volumes.py [--out DIR] [--ipsw FILE] [--tree DIR] [--repo DIR] [--force]

The two partitions of the card are the OS and SystemOS disks the IPSW carries:

  044-76040-038.dmg  component OS                 UDIF with a koly trailer, 5 157 406 069 B
  044-76034-038.dmg  component Cryptex1,SystemOS  a raw UDRW image, no koly, 3 303 014 400 B

Both are decoded to their raw disk with the UniXNU tool's own udif reader (unixnu/udif.py), which
does what `hdiutil attach` exposes as /dev/rdiskN and needs no Apple tool: raw chunks are copied,
zero-fill chunks read as zeros and zlib, bzip2 and LZFSE chunks are decompressed. A file without a
koly trailer is its own raw disk, so SystemOS is copied unchanged.

Nothing here touches a device or a card: it reads the IPSW and writes files.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys
import tomllib

# Measured on iPad_Pro_HFR_17.7.11_21H461_Restore.ipsw (2026-10-28). A different IPSW is a different
# card, so the pins can be relaxed with --no-pin but never silently.
PINS = {
    'OS': ('044-76040-038.raw', 6_077_580_800,
           'e5634d04a8ac35f024845f583394e0a35deb26a04cb0c00286a8674e25a9a4f2'),
    'Cryptex1,SystemOS': ('044-76034-038.raw', 3_303_014_400,
                          'b4908cde41d23e6bc05e6af51a2c9eeb00cf0f863a6a18b1600c96a3fd70414f'),
}
COMPONENTS = tuple(PINS)
MEMBERS = {'OS': '044-76040-038.dmg', 'Cryptex1,SystemOS': '044-76034-038.dmg'}
REPO = Path(__file__).resolve().parents[2]


def device_repo(path):
    """The directory that holds board.toml: this repository or devices/odin3-pagani inside it."""
    path = Path(path)
    for candidate in (path, path / 'devices' / 'odin3-pagani'):
        if (candidate / 'board.toml').is_file():
            return candidate
    raise SystemExit(f'no board.toml in {path} nor in {path / "devices" / "odin3-pagani"}')


def sha256_file(path):
    digest = hashlib.sha256()
    with path.open('rb') as handle:
        while block := handle.read(8 << 20):
            digest.update(block)
    return digest.hexdigest()


def write_raw(source, destination, tree):
    """Decode `source` (UDIF or raw) into `destination`; returns (sha256, size)."""
    sys.path.insert(0, str(Path(tree) / 'scripts'))
    from unixnu import udif

    image = udif.Image(str(source))
    digest = hashlib.sha256()
    written = 0
    partial = Path(destination).with_suffix(Path(destination).suffix + '.partial')
    with partial.open('wb') as target:
        for block in image.blocks():
            target.write(block)
            digest.update(block)
            written += len(block)
    if written != image.size:
        partial.unlink()
        raise SystemExit(f'{source}: wrote {written} bytes, the image declares {image.size}')
    partial.rename(destination)
    return digest.hexdigest(), written


def extract(tree, repo, ipsw):
    """Ask the tool for the two members; it checks the board identity against the IPSW manifest."""
    board = tomllib.loads((Path(repo) / 'board.toml').read_text())
    command = [sys.executable, str(Path(tree) / 'unixnu'), 'ipsw', 'fetch', *COMPONENTS,
               '-D', str(repo), '--ipsw', str(ipsw)]
    result = subprocess.run(command, capture_output=True, text=True)
    if result.returncode:
        raise SystemExit('unixnu ipsw fetch failed:\n' + (result.stdout + result.stderr).strip())
    cache = Path(tree) / 'out' / 'ipsw' / board['ipsw']['build']
    found = {}
    for component in COMPONENTS:
        located = None
        for line in result.stdout.splitlines():
            candidate = Path(line.strip())
            if candidate.suffix == '.dmg' and candidate.name == MEMBERS[component]:
                located = candidate if candidate.is_absolute() else cache / candidate.name
        located = located or cache / MEMBERS[component]
        if not located.is_file():
            raise SystemExit(f'{component}: {MEMBERS[component]} is not on disk after the extraction')
        found[component] = located
    return found


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--out', type=Path, help='output directory (default: <repo>/out/target-volumes)')
    parser.add_argument('--ipsw', type=Path, help='restore image (default: UNIXNU_IPSW)')
    parser.add_argument('--tree', type=Path, help='UniXNU checkout (default: UNIXNU_TREE, else <repo>/../UniXNU)')
    parser.add_argument('--repo', type=Path, default=REPO, help='device repository (default: this one)')

    parser.add_argument('--force', action='store_true', help='rewrite volumes that already exist')
    parser.add_argument('--no-pin', action='store_true', help='do not compare against the measured SHA-256')
    args = parser.parse_args()

    args.repo = device_repo(args.repo)
    out = (args.out or args.repo / 'out/target-volumes').resolve()
    tree = args.tree or Path(os.environ.get('UNIXNU_TREE') or args.repo.parent / 'UniXNU')
    ipsw = args.ipsw or Path(os.environ.get('UNIXNU_IPSW') or '')
    if not str(ipsw) or not ipsw.is_file():
        board = tomllib.loads((args.repo / 'board.toml').read_text())
        sys.exit(f'no IPSW: pass --ipsw or set UNIXNU_IPSW (board url: {board["ipsw"]["url"]})')
    if not (tree / 'unixnu').is_file():
        sys.exit(f'not a UniXNU checkout: {tree} (pass --tree or set UNIXNU_TREE)')

    out.mkdir(parents=True, exist_ok=True)
    sources = extract(tree, args.repo, ipsw)
    rows, failed = [], False
    for component, source in sources.items():
        name, size, pinned = PINS[component]
        destination = out / name
        if destination.is_file() and not args.force:
            digest, written = sha256_file(destination), destination.stat().st_size
        else:
            print(f'== {component}: {source.name} -> {name}')
            digest, written = write_raw(source, destination, tree)
        record = {'component': component, 'member': source.name, 'raw': name,
                  'size': written, 'sha256': digest, 'ipsw': ipsw.name}
        destination.with_suffix('.json').write_text(json.dumps(record, indent=2) + '\n')
        ok = args.no_pin or (digest == pinned and written == size)
        failed = failed or not ok
        rows.append((component, name, written, digest, 'ok' if ok else 'MISMATCH'))

    print()
    for component, name, written, digest, state in rows:
        print(f'{component:20} {name:22} {written:>13} B  {digest}  {state}')
    if failed:
        sys.exit('the decoded volumes do not match the measured pins (--no-pin to accept another IPSW)')


if __name__ == '__main__':
    main()
