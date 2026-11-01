#!/usr/bin/env python3
"""Pure candidate transformation/refusal tests. No device or image writes."""
import importlib.util
from pathlib import Path
import subprocess
import sys

here = Path(__file__).resolve().parent
spec = importlib.util.spec_from_file_location("candidate", here / "prepare-isolated-candidate.py")
candidate = importlib.util.module_from_spec(spec)
spec.loader.exec_module(candidate)
sys.path.insert(0, str(here.parent / "d1"))
import adtlib

tree = {"name": "root", "properties": {"name": b"root\0", "odd": b"abc"}, "children": [
    {"name": "chosen", "properties": {"name": b"chosen\0", "test": bytes(8)}, "children": []}]}
raw = candidate.encode(tree)
decoded, end = adtlib.decode(raw)
assert end == len(raw) and decoded == tree and candidate.encode(decoded) == raw
assert candidate.arguments("-v odin-touch=0 odin-gpu-survey=1", {"odin-touch": 3, "odin-gpu-survey": 0}) == "-v odin-touch=3 odin-gpu-survey=0"
try:
    candidate.arguments("odin-touch=0 odin-touch=4", {"odin-touch": 3})
    raise AssertionError("duplicate argument accepted")
except AssertionError as e:
    if str(e) == "duplicate argument accepted":
        raise
good = "7f3d2c1a-55aa-4e0b-9c21-0d8d6a4b1e99"
assert candidate.store_arguments(0, None, 0, None) == {"sheng-data-persist": 0, "odin-sd-uhs": 0}
assert candidate.store_arguments(1, good.upper(), 1, None) == {"sheng-data-persist": 1, "odin-sd-uhs": 1, "odin-data-guid": good}
assert candidate.store_arguments(2, good, 2, 0x40c00)["odin-sd-vqmmc"] == "0x40c00"
for bad in ((3, good, 0, None), (1, None, 0, None), (1, "not-a-guid", 0, None), (1, "00000000-0000-0000-0000-000000000000", 0, None),
            (0, good, 0, None), (0, None, 2, None), (0, None, 2, 0x1234), (0, None, 2, 0x40c01), (0, None, 1, 0x40c00), (0, None, 4, None)):
    try:
        candidate.store_arguments(*bad)
        raise AssertionError(f"accepted {bad}")
    except ValueError:
        pass
project = here.parents[1]
for output in (str(project), "/dev/sda", "/tmp/not-allowed-candidate"):
    command = [sys.executable, str(here / "prepare-isolated-candidate.py"), "--project", str(project),
               "--kind", "mem6", "--out", output]
    r = subprocess.run(command, capture_output=True, text=True)
    assert r.returncode == 2 and "NEW directory inside project" in r.stderr
r = subprocess.run([sys.executable, str(here / "prepare-isolated-candidate.py"), "--project", str(project), "--kind", "mem6", "--data-persist", "2", "--out", str(project / "tmp/never")],
                   capture_output=True, text=True)
assert r.returncode == 2 and "belong to --kind store" in r.stderr
print("PASS: store-argument validation, ADT roundtrip/padding, single-argument replacement, duplicate rejection, existing/outside/device output refusal")
