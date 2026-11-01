#!/usr/bin/env python3
"""Host test of make-noedge-medium.py on a small FAT image (no real medium, no card, no device)."""
import hashlib
from pathlib import Path
import subprocess
import sys
import tempfile

here = Path(__file__).resolve().parent
tool = here / "make-noedge-medium.py"
project = here.parents[1]
scratch = project / "tmp"
scratch.mkdir(exist_ok=True)
SIZE = 16 * 1024 * 1024

TOML = '''version = 1
[boot]
timeout = 3
default = "xnu"
[[entry]]
id = "xnu"
title = "t"
type = "xnu"
path = "Payloads/kernel.macho"
args = "odin-sd-dma=1 -v cpus=8 boot-diag-resume=0 sheng-data-persist=2"
[[entry]]
id = "reset"
title = "Restart"
type = "action"
action = "reset"
'''


def sh(*argv, **kw):
    return subprocess.run([str(a) for a in argv], check=True, capture_output=True, text=True, **kw)


def run(*argv):
    return subprocess.run([sys.executable, "-I", str(tool), *map(str, argv)], capture_output=True, text=True)


def build_object(folder, name, symbols):
    c = folder / f"{name}.c"
    c.write_text("".join(f"int {s};\n" for s in symbols))
    o = folder / f"{name}.o"
    sh("cc", "-c", c, "-o", o)
    return o


with tempfile.TemporaryDirectory(prefix="medium-", dir=scratch) as tmp:
    folder = Path(tmp)
    base = folder / "base.img"
    base.write_bytes(bytes(SIZE))
    sh("mformat", "-i", base, "::")
    for d in ("EFI", "EFI/UniXNU", "EFI/UniXNU/Payloads"):
        sh("mmd", "-i", base, "::/" + d)
    plain = build_object(folder, "plain", ["odin_touch_allocate", "sched_clutch_dispatch"])
    edge = build_object(folder, "edge", ["odin_touch_allocate", "sched_edge_dispatch"])
    foreign = build_object(folder, "foreign", ["something_else"])
    old_kernel = build_object(folder, "old", ["odin_touch_allocate", "old_marker"])
    (folder / "unixnu.toml").write_text(TOML)
    (folder / "adt").write_bytes(b"adt-bytes")
    sh("mcopy", "-i", base, folder / "unixnu.toml", "::/EFI/UniXNU/unixnu.toml")
    sh("mcopy", "-i", base, old_kernel, "::/EFI/UniXNU/Payloads/kernel.macho")
    sh("mcopy", "-i", base, folder / "adt", "::/EFI/UniXNU/Payloads/kernel.macho.adt")
    digest = hashlib.sha256(base.read_bytes()).hexdigest()
    common = ["--base", base, "--base-sha256", digest, "--image-bytes", SIZE]

    r = run(*common, "--kernel", plain, "--out", folder / "out1", "--set", "boot-diag-resume=120", "--set", "odin-userland-log=30")
    assert r.returncode == 0, r.stdout + r.stderr
    report = __import__("json").loads(r.stdout)
    assert report["changed_files"] == ["EFI/UniXNU/Payloads/kernel.macho", "EFI/UniXNU/unixnu.toml"], report
    assert not report["kernel_has_edge"] and report["hardware_tested"] is False
    args = report["kernel_args"].split()
    assert "boot-diag-resume=120" in args and "boot-diag-resume=0" not in args and args[-1] == "odin-userland-log=30"
    assert args[0] == "odin-sd-dma=1" and "sheng-data-persist=2" in args
    assert (folder / "out1" / "MANIFEST.sha256").read_text().count("\n") == 3          # toml, kernel, adt
    assert hashlib.sha256(base.read_bytes()).hexdigest() == digest                     # the base is never written
    got = sh("mtype", "-i", folder / "out1" / "odin3-boot.img", "::/EFI/UniXNU/Payloads/kernel.macho.adt").stdout
    assert got == "adt-bytes"

    # no --set: only the kernel differs, the toml is rewritten byte-identically
    r = run(*common, "--kernel", plain, "--out", folder / "out2")
    assert r.returncode == 0, r.stdout + r.stderr
    assert __import__("json").loads(r.stdout)["changed_files"] == ["EFI/UniXNU/Payloads/kernel.macho"]
    r = run(*common, "--kernel", edge, "--out", folder / "out3")
    assert r.returncode == 2 and "Edge scheduler" in r.stderr and not (folder / "out3").exists()
    r = run(*common, "--kernel", edge, "--out", folder / "out3", "--allow-edge")
    assert r.returncode == 0 and __import__("json").loads(r.stdout)["kernel_has_edge"] is True
    r = run(*common, "--kernel", foreign, "--out", folder / "out4")
    assert r.returncode == 2 and "odin_touch_allocate" in r.stderr
    r = run(*common, "--kernel", plain, "--out", folder / "out1")
    assert r.returncode == 2 and "NEW directory" in r.stderr
    r = run("--base", base, "--base-sha256", "0" * 64, "--image-bytes", SIZE, "--kernel", plain, "--out", folder / "out5")
    assert r.returncode == 2 and "SHA-256 mismatch" in r.stderr
    r = run(*common, "--kernel", plain, "--out", folder / "out6", "--set", "a=1", "--set", "a=2")
    assert r.returncode == 2 and "repeated" in r.stderr
    r = run(*common, "--kernel", plain, "--out", folder / "out7", "--set", "x=1 y=2")
    assert r.returncode == 2 and "plain characters" in r.stderr
    r = run("--base", base, "--base-sha256", digest, "--image-bytes", SIZE + 1, "--kernel", plain, "--out", folder / "out8")
    assert r.returncode == 2 and "regular file of" in r.stderr
print("PASS: only toml+kernel change, Edge kernel refused, foreign kernel refused, base untouched, SHA/size/new-dir/duplicate/odd-value refusals")
