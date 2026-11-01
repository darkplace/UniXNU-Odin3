#!/usr/bin/env python3
"""Satisfy the pinned Pagani display recipe's compile-only kit path.

The upstream ABI-qualified recipe also builds its embedded peripherals. Its
report and compiler use kits/oneplus13t/PaganiPeripherals.h. Keep that input
explicit rather than copying unknown addresses into Odin's DTS. None of those
peripherals starts with odin-gop-only=1 (Stage7Probe's hard early return).
"""
from pathlib import Path

root = Path(__file__).resolve().parents[1]
source = root / "kits/odin3"
alias = root / "kits/oneplus13t"
if not (source / "PaganiPeripherals.h").is_file():
    raise SystemExit("Missing retained compile-only Pagani ABI scaffold")
if alias.is_symlink():
    if alias.resolve() != source.resolve():
        raise SystemExit("Refusing to replace an unrelated oneplus13t kit")
elif alias.exists():
    raise SystemExit("Refusing to replace an existing oneplus13t kit")
else:
    alias.symlink_to("odin3", target_is_directory=True)
print("Odin GOP: retained compile-only kit alias; no device access")
