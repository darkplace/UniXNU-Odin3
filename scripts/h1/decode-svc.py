#!/usr/bin/env python3
"""Decode the observer's signed DECIMAL SVC number using the built XNU source."""
import argparse
import os
from pathlib import Path
import re

project = Path(os.environ.get("IDRAGON_PROJECT", Path.home() / "iDragon"))
default = project / "firmware/UniXNU-enc3l/out/odin3/ipados-17.7.11-21H461/odin3/tree/.cache/xnu-work"
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("call", type=int)
parser.add_argument("--xnu", type=Path, default=default)
args = parser.parse_args()
relative = "bsd/kern/syscalls.master" if args.call >= 0 else "osfmk/kern/syscall_sw.c"
path = args.xnu / relative
pattern = (rf"^\s*{args.call}\s" if args.call >= 0 else rf"/\*\s*{-args.call}\s*\*/")
matches = 0
for line, text in enumerate(path.read_text().splitlines(), 1):
    if re.search(pattern, text):
        print(f"{path}:{line}: {text.strip()}")
        matches += 1
if args.call < 0:
    # ARM time fast paths precede mach_trap_table dispatch.
    for relative in ("osfmk/mach/arm/syscall_sw.h", "osfmk/mach/arm/traps.h"):
        path = args.xnu / relative
        if not path.is_file():
            continue
        for line, text in enumerate(path.read_text().splitlines(), 1):
            if "MACH_ARM_TRAP_" in text and re.search(rf"(?<!\d){args.call}(?!\d)", text):
                print(f"{path}:{line}: ARM fast path: {text.strip()}")
if not matches:
    raise SystemExit("No table match; preserve the number and inspect handle_svc in this exact source.")
print("IN is an outstanding observation, not proof of a deadlock; Mach receive may be normal.")
