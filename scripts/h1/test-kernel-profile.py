#!/usr/bin/env python3
"""Host test of check-kernel-profile.py on tiny objects (ELF, so symbols carry no leading underscore)."""
from pathlib import Path
import subprocess
import sys
import tempfile

here = Path(__file__).resolve().parent
checker = here / "check-kernel-profile.py"
project = here.parents[1]
scratch = project / "tmp"
scratch.mkdir(exist_ok=True)


def build(folder, name, source):
    c = folder / f"{name}.c"
    c.write_text(source)
    o = folder / f"{name}.o"
    subprocess.run(["cc", "-c", str(c), "-o", str(o)], check=True)
    return o


def run(*argv):
    return subprocess.run([sys.executable, "-I", str(checker), *map(str, argv)], capture_output=True, text=True)


with tempfile.TemporaryDirectory(prefix="kprofile-", dir=scratch) as tmp:
    folder = Path(tmp)
    plain = build(folder, "plain", "int odin_touch_allocate(void){return 0;}\nint sched_clutch_dispatch;\n")
    edge = build(folder, "edge", "int odin_touch_allocate(void){return 0;}\nint sched_edge_dispatch;\n"
                                 "int sched_edge_choose_thread;\nint sched_amp_init;\n")
    r = run(plain, "--expect-no-edge")
    assert r.returncode == 0 and "no Edge" in r.stdout and "edge_dispatch  False" in r.stdout, r.stdout + r.stderr
    r = run(plain, "--expect-edge")
    assert r.returncode == 1 and "FAIL" in r.stderr
    r = run(edge, "--expect-edge")
    assert r.returncode == 0 and "edge_symbols   2" in r.stdout and "amp_symbols    1" in r.stdout, r.stdout
    r = run(edge, "--expect-no-edge")
    assert r.returncode == 1 and "Edge scheduler present" in r.stderr
    r = run(folder / "missing.o")
    assert r.returncode == 2
    r = run(edge)
    assert r.returncode == 0 and "Edge/AMP" in r.stdout
print("PASS: Edge detection on objects with and without the Edge dispatch table, both expectations, missing file")
