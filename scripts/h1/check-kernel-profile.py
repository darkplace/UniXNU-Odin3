#!/usr/bin/env python3
"""Report which scheduler a built Odin kernel carries, from its symbol table (llvm-nm). No device access.

The kernels that reached the lock screen (ea877510, 51a1a577) have no Edge scheduler; every kernel built from the
default device tree since odin3-clusters.patch entered the series has one (change ui-handoff-rca, report
docs/builds/odin3-ui-handoff-rca-20261030.md). A medium report must say which of the two it is.

  check-kernel-profile.py KERNEL [--expect-no-edge | --expect-edge]

Exit 0 when the expectation holds (or none was given), 1 when it does not, 2 on a usage or tool error.
"""
import argparse
import hashlib
from pathlib import Path
import shutil
import subprocess
import sys


def symbols(kernel):
    tool = shutil.which("llvm-nm")
    if not tool:
        raise SystemExit("llvm-nm not found")
    # Mach-O prefixes C symbols with "_", ELF (host test objects) does not; accept both.
    text = subprocess.run([tool, "--defined-only", str(kernel)], capture_output=True, text=True, check=True).stdout
    names = set()
    for line in text.splitlines():
        parts = line.split()
        if len(parts) >= 3:
            names.add(parts[-1].lstrip("_"))
    return names


def profile(names):
    return {
        "edge_symbols": sum(n.startswith("sched_edge_") for n in names),
        "amp_symbols": sum(n.startswith("sched_amp_") for n in names),
        "edge_dispatch": "sched_edge_dispatch" in names,
        "odin_touch": "odin_touch_allocate" in names,
        "odin_fan": "odin_fan_allocate" in names,
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("kernel", type=Path)
    group = parser.add_mutually_exclusive_group()
    group.add_argument("--expect-no-edge", action="store_true")
    group.add_argument("--expect-edge", action="store_true")
    args = parser.parse_args()
    if not args.kernel.is_file():
        print(f"not a file: {args.kernel}", file=sys.stderr)
        return 2
    data = args.kernel.read_bytes()
    result = profile(symbols(args.kernel))
    print(f"kernel   {args.kernel}")
    print(f"sha256   {hashlib.sha256(data).hexdigest()}")
    print(f"bytes    {len(data)}")
    for key, value in result.items():
        print(f"{key:<14} {value}")
    has_edge = result["edge_dispatch"]
    print("scheduler  Edge/AMP" if has_edge else "scheduler  Clutch, single cluster (no Edge)")
    if args.expect_no_edge and has_edge:
        print("FAIL: Edge scheduler present, expected none", file=sys.stderr)
        return 1
    if args.expect_edge and not has_edge:
        print("FAIL: no Edge scheduler, expected one", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
