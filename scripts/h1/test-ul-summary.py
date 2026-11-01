#!/usr/bin/env python3
"""Host test of ul-summary.py on a fixture of UL lines (copied from firmware/out/sdlog-noedge-20261030.txt, t=150..240 and a variant)."""
from pathlib import Path
import re
import subprocess
import sys
import tempfile

tool = Path(__file__).resolve().with_name("ul-summary.py")
LINE = ("UL t={t} main={main} procs={procs} jobs=5f/5f svc1=1 own={own} logo=0 frames={frames} sb=0(0:0:0:0) bb=0(0:0:0:0) "
        "last={last} exits={exits} ab=0 trap=0 aks=400/7 disp=key operation unavailable:a free=1 avail=2 rd={rd}MiB wr=1KiB")
CONTROL = [
    dict(t=150, main=1, procs=3, own=0, frames=0, last="-:0:0", exits=0, rd=2872),
    dict(t=180, main=1, procs=64, own=0, frames=0, last="fairplayd.A2:87:0", exits=16, rd=3029),
    dict(t=210, main=1, procs=68, own=0, frames=0, last="fairplayd.A2:96:0", exits=18, rd=3186),
    dict(t=240, main=1, procs=87, own=0, frames=0, last="com.apple.datami:111:0", exits=22, rd=3353),
    dict(t=270, main=1, procs=112, own=1, frames=3, last="fairplayd.A2:154:0", exits=26, rd=3501),
]
TREATMENT = [
    dict(t=150, main=1, procs=3, own=0, frames=0, last="-:0:0", exits=0, rd=2872),
    dict(t=180, main=1, procs=60, own=0, frames=0, last="com.apple.datami:80:0", exits=4, rd=3000),
    dict(t=210, main=1, procs=70, own=1, frames=2, last="UARPUpdaterServi:90:f", exits=6, rd=3100),
]


def log(rows, noise=True):
    lines = ["== /dev/sda: 500006912 sectores", "PROBE: sdhci provider accepted"] if noise else []
    lines += [LINE.format(**row) for row in rows]
    lines.append("STORE: 2048 writes ok, 49384 KiB, 16814 ms inside the transport")
    return "\n".join(lines) + "\n"


def run(*argv):
    return subprocess.run([sys.executable, "-I", str(tool), *map(str, argv)], capture_output=True, text=True)


with tempfile.TemporaryDirectory() as tmp:
    a, b = Path(tmp) / "control.txt", Path(tmp) / "treatment.txt"
    a.write_text(log(CONTROL))
    b.write_text(log(TREATMENT))
    r = run(a)
    assert r.returncode == 0, r.stderr
    assert "'samples': 4" in r.stdout and "'job_last': 3" in r.stdout and "'job_pids': [87, 96, 154]" in r.stdout, r.stdout
    assert "'frames_t': 270" in r.stdout and "'main_t': 150" in r.stdout and "'exits': 26" in r.stdout, r.stdout
    assert re.search(r"^\s*180\s+64\s+16\s+fairplayd\.A2:87\s", r.stdout, re.M), r.stdout   # window Δexits is exits(t) - exits(t-30)
    assert re.search(r"^\s*210\s+68\s+2\s+fairplayd\.A2:96\s", r.stdout, re.M), r.stdout
    r = run(a, "--vs", b)
    assert r.returncode == 0, r.stderr
    assert "last to exit in 3/4 samples (control.txt) vs 0/2 (treatment.txt)" in r.stdout, r.stdout
    assert "first system frame at UL t: 270 vs 210" in r.stdout, r.stdout
    empty = Path(tmp) / "empty.txt"
    empty.write_text("PROBE: nothing\n")
    r = run(empty)
    assert r.returncode != 0 and "no UL lines" in r.stderr
    r = run(a, "--job", "com.apple.datamigrator")                                       # UL keeps 16 characters of the name
    assert "'job_last': 1" in r.stdout and "'job_pids': [111]" in r.stdout, r.stdout
print("PASS: UL fields parsed, Δexits per window, last-to-exit count and pids, first system frame, two-log comparison, empty log refused")
