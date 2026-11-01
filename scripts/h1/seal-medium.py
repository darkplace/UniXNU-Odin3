#!/usr/bin/env python3
"""Seal a built medium: write SEAL.json with every input that produced it.

  scripts/h1/seal-medium.py [--out DIR] [--repo DIR] [--tree DIR] [--ipsw FILE] [--image NAME]

A medium is reproducible when each of its inputs can be named. This records them next to the images:

  device      the device repository: commit, and whether its working tree was clean
  tree        the UniXNU checkout: commit, and the revision recorded in upstream.rev
  patches     the SHA-256 of every patch in patches/unixnu/, in order
  inputs      the SHA-256 of unixnu.lock, board.toml and the defconfig
  toolchain   the docker image id of the toolchain container
  ipsw        the SHA-256 and size of the restore image
  medium      the table of MANIFEST.sha256, as read back from the image

No absolute paths are written. To rebuild a sealed medium: check out the recorded commits, apply the
recorded patches, and follow docs/build.md. What the seal does not promise is a byte-identical kernel:
see the "Hashes to compare" section of docs/build.md.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys
import tomllib


def sha256_file(path):
    digest = hashlib.sha256()
    with open(path, 'rb') as handle:
        while block := handle.read(4 << 20):
            digest.update(block)
    return digest.hexdigest()


def git(path, *args):
    result = subprocess.run(['git', '-C', str(path), *args], capture_output=True, text=True)
    if result.returncode:
        raise SystemExit(f'git {args[0]} failed in {path}: {result.stderr.strip()}')
    return result.stdout.strip()


def device_repo(path):
    path = Path(path)
    for candidate in (path, path / 'devices' / 'odin3-pagani'):
        if (candidate / 'board.toml').is_file():
            return candidate
    raise SystemExit(f'no board.toml in {path} nor in {path / "devices" / "odin3-pagani"}')


def manifest(path):
    table = {}
    for line in path.read_text().splitlines():
        parts = line.split()
        if len(parts) >= 2 and len(parts[0]) == 64:
            table[parts[1]] = parts[0]
    return table


def main():
    repo_root = Path(__file__).resolve().parents[2]
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--out', type=Path, help='medium directory (default: <repo>/out)')
    parser.add_argument('--repo', type=Path, default=repo_root, help='repository (default: this one)')
    parser.add_argument('--tree', type=Path, help='UniXNU checkout (default: UNIXNU_TREE, else <repo>/../UniXNU)')
    parser.add_argument('--ipsw', type=Path, help='restore image (default: UNIXNU_IPSW)')
    parser.add_argument('--image', default=os.environ.get('UNIXNU_TOOLCHAIN_IMAGE', 'unixnu-toolchain:v1'),
                        help='toolchain image (default: unixnu-toolchain:v1)')
    args = parser.parse_args()

    repo = device_repo(args.repo)
    tree = args.tree or Path(os.environ.get('UNIXNU_TREE') or args.repo.parent / 'UniXNU')
    out = (args.out or repo / 'out').resolve()
    board = tomllib.loads((repo / 'board.toml').read_text())
    defconfig = Path('configs') / 'odin3_defconfig'

    seal = {
        'schema_version': 1,
        'device': {
            'repository': repo.name,
            'commit': git(repo, 'rev-parse', 'HEAD'),
            'clean': not git(repo, 'status', '--porcelain'),
            'dirty_files': [line[3:] for line in git(repo, 'status', '--porcelain').splitlines()][:64],
        },
        'tree': {
            'commit': git(tree, 'rev-parse', 'HEAD'),
            'upstream_rev': (repo / 'upstream.rev').read_text().strip(),
            'clean': not git(tree, 'status', '--porcelain'),
            'dirty_files': [line[3:] for line in git(tree, 'status', '--porcelain').splitlines()][:64],
        },
        'patches': {path.name: sha256_file(path) for path in sorted((repo / 'patches/unixnu').glob('*.patch'))},
        'inputs': {
            'unixnu.lock': sha256_file(repo / 'unixnu.lock'),
            'board.toml': sha256_file(repo / 'board.toml'),
            defconfig.as_posix(): sha256_file(repo / defconfig),
        },
        'toolchain': {'image': args.image},
        'ipsw': {},
        'medium': {},
    }

    if (repo / 'tools.toml').is_file():
        seal['inputs']['tools.toml'] = sha256_file(repo / 'tools.toml')

    result = subprocess.run(['docker', 'image', 'inspect', '--format', '{{.Id}}', args.image],
                            capture_output=True, text=True)
    seal['toolchain']['id'] = result.stdout.strip() if result.returncode == 0 else None

    ipsw = args.ipsw or Path(os.environ.get('UNIXNU_IPSW') or '')
    if str(ipsw) and ipsw.is_file():
        seal['ipsw'] = {'name': ipsw.name, 'size': ipsw.stat().st_size, 'sha256': sha256_file(ipsw)}
    else:
        seal['ipsw'] = {'name': board['ipsw'].get('url', '').rsplit('/', 1)[-1], 'size': None, 'sha256': None}
        print('  warning: no IPSW on disk (pass --ipsw or set UNIXNU_IPSW); the seal has no image hash')

    table = manifest(out / 'MANIFEST.sha256') if (out / 'MANIFEST.sha256').is_file() else {}
    seal['medium'] = table

    target = out / 'SEAL.json'
    target.write_text(json.dumps(seal, indent=2) + '\n')
    print(f'seal: {target}')
    print(f'  device {seal["device"]["commit"][:12]}{"" if seal["device"]["clean"] else " (dirty)"}'
          f'  tree {seal["tree"]["commit"][:12]}{"" if seal["tree"]["clean"] else " (dirty)"}'
          f'  patches {len(seal["patches"])}  medium files {len(seal["medium"])}')
    if not seal['device']['clean'] or not seal['tree']['clean']:
        print('  note: the trees are dirty; SEAL.json lists the modified paths. Applying the recorded'
              ' patches under patches/unixnu/ leaves the build tree dirty by design')
    if not seal['medium']:
        print('  warning: no MANIFEST.sha256 in the medium directory; the seal has no result table')
    return 0


if __name__ == '__main__':
    sys.exit(main())
