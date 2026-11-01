#!/usr/bin/env python3
"""Decode an Apple device tree (ADT) sidecar and check the nodes the platform expert requires.

  python3 scripts/d1/adt-dump.py kernel.macho.adt [--json report.json]
"""
import argparse
import json
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import adtlib  # noqa: E402

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('adt')
parser.add_argument('--json', help='write the checks as JSON')
args = parser.parse_args()
data = open(args.adt, 'rb').read()
root, end = adtlib.decode(data)
if end != len(data):
    raise SystemExit(f'{args.adt}: decoded {end} bytes of {len(data)}')
results = adtlib.checks(root, data)
print(f'{args.adt}: {len(data)} bytes, {len(results)} checks')
for name, ok, detail in results:
    print(f'  {"OK  " if ok else "FAIL"} {name:<36}{detail}')
if args.json:
    with open(args.json, 'w') as handle:
        json.dump({'adt': args.adt, 'bytes': len(data),
                   'checks': [{'name': n, 'ok': o, 'detail': d} for n, o, d in results]}, handle, indent=2)
sys.exit(0 if all(ok for _, ok, _ in results) else 1)
