#!/usr/bin/env python3
"""Host test of make-seed-medium.py on a small FAT image (no real medium, no card, no device)."""
import hashlib
import importlib.util
import json
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import uuid

here = Path(__file__).resolve().parent
tool = here / "make-seed-medium.py"
project = here.parents[1]
scratch = project / "tmp"
scratch.mkdir(exist_ok=True)
SIZE = 16 * 1024 * 1024
UUID = "5D8FE226-65AB-46F4-92F2-C801EE86F627"
OTHER = "FD326389-0000-4000-8000-000000000001"

TOML = '''version = 1
[boot]
timeout = 3
default = "xnu"
[[entry]]
id = "xnu"
title = "t"
type = "xnu"
path = "Payloads/kernel.macho"
initrd = "Payloads/scratch-data.img"
args = "odin-sd-dma=1 -v cpus=8 sheng-data-persist=2 odin-userland-log=30"
[[entry]]
id = "reset"
title = "Restart"
type = "action"
action = "reset"
'''

spec = importlib.util.spec_from_file_location("packed", project / "devices/odin3-pagani/modules/Pagani/drivers/PaganiDarwinBoot/Test/packed_ramdisk.py")
packed = importlib.util.module_from_spec(spec)
spec.loader.exec_module(packed)


def sh(*argv, **kw):
    return subprocess.run([str(a) for a in argv], check=True, capture_output=True, text=True, **kw)


def run(*argv):
    return subprocess.run([sys.executable, "-I", str(tool), *map(str, argv)], capture_output=True, text=True)


def seed(path, volumes, extra=b""):
    """A small DBRDISK1 file: `volumes` is a list of (xid, uuid) APSB pages."""
    raw = bytearray(8 * 1024 * 1024)
    for index, (xid, ident) in enumerate(volumes):
        page = 4096 * (index + 1)
        raw[page + 32:page + 36] = b"APSB"
        struct.pack_into("<Q", raw, page + 16, xid)
        raw[page + 240:page + 256] = uuid.UUID(ident).bytes
    raw[4096 * 7:4096 * 7 + len(extra)] = extra
    source = path.with_suffix(".raw")
    source.write_bytes(raw)
    packed.pack(source, path)
    return path


with tempfile.TemporaryDirectory(prefix="medium-", dir=scratch) as tmp:
    folder = Path(tmp)
    base = folder / "base.img"
    base.write_bytes(bytes(SIZE))
    sh("mformat", "-i", base, "::")
    for d in ("EFI", "EFI/UniXNU", "EFI/UniXNU/Payloads"):
        sh("mmd", "-i", base, "::/" + d)
    (folder / "unixnu.toml").write_text(TOML)
    (folder / "kernel").write_bytes(b"kernel-bytes" * 100)
    (folder / "adt").write_bytes(b"adt-bytes")
    old_seed = seed(folder / "old.dbr", [(2, UUID), (4, UUID)])
    sh("mcopy", "-i", base, folder / "unixnu.toml", "::/EFI/UniXNU/unixnu.toml")
    sh("mcopy", "-i", base, folder / "kernel", "::/EFI/UniXNU/Payloads/kernel.macho")
    sh("mcopy", "-i", base, folder / "adt", "::/EFI/UniXNU/Payloads/kernel.macho.adt")
    sh("mcopy", "-i", base, old_seed, "::/EFI/UniXNU/Payloads/scratch-data.img")
    digest = hashlib.sha256(base.read_bytes()).hexdigest()
    common = ["--base", base, "--base-sha256", digest, "--image-bytes", SIZE]
    new_seed = seed(folder / "new.dbr", [(2, UUID), (4, UUID), (7, UUID)], b"policy")
    SEED = "EFI/UniXNU/Payloads/scratch-data.img"

    # T: new seed only -> exactly one file differs; the toml is not rewritten
    r = run(*common, "--out", folder / "t", "--seed", new_seed, "--volume-uuid", UUID.lower())
    assert r.returncode == 0, r.stdout + r.stderr
    report = json.loads(r.stdout)
    assert report["changed_files"] == [SEED], report
    assert report["seed"]["volume_uuid"] == UUID and report["seed"]["volume_superblock_xid"] == 7 and report["seed"]["packed"] is True
    assert report["seed_file_sha256"] == hashlib.sha256(new_seed.read_bytes()).hexdigest()
    assert report["kernel_args"] == "odin-sd-dma=1 -v cpus=8 sheng-data-persist=2 odin-userland-log=30"
    assert (folder / "t" / "MANIFEST.sha256").read_text().count("\n") == 4          # toml, kernel, adt, seed
    assert hashlib.sha256(base.read_bytes()).hexdigest() == digest                  # the base is never written
    assert sh("mtype", "-i", folder / "t" / "odin3-boot.img", "::/EFI/UniXNU/Payloads/kernel.macho.adt").stdout == "adt-bytes"

    # C: an argument only -> only the toml differs; the seed stays
    r = run(*common, "--out", folder / "c", "--set", "sheng-data-reset=1")
    assert r.returncode == 0, r.stdout + r.stderr
    report = json.loads(r.stdout)
    assert report["changed_files"] == ["EFI/UniXNU/unixnu.toml"] and report["seed"] is None, report
    assert report["kernel_args"].split()[-1] == "sheng-data-reset=1"

    # both
    r = run(*common, "--out", folder / "tc", "--seed", new_seed, "--volume-uuid", UUID, "--set", "sheng-data-reset=1")
    assert r.returncode == 0 and json.loads(r.stdout)["changed_files"] == [SEED, "EFI/UniXNU/unixnu.toml"]

    # refusals
    r = run(*common, "--out", folder / "x1", "--seed", new_seed, "--volume-uuid", OTHER)
    assert r.returncode == 2 and "is not the kernel's" in r.stderr and not (folder / "x1").exists()
    r = run(*common, "--out", folder / "x2", "--seed", new_seed)
    assert r.returncode == 2 and "--volume-uuid" in r.stderr
    r = run(*common, "--out", folder / "x3", "--seed", old_seed, "--volume-uuid", UUID)
    assert r.returncode == 1 and "no-op" in r.stderr
    r = run(*common, "--out", folder / "x4")
    assert r.returncode == 2 and "nothing to change" in r.stderr
    raw = folder / "raw.img"
    raw.write_bytes(bytes(8192))
    r = run(*common, "--out", folder / "x5", "--seed", raw, "--volume-uuid", UUID)
    assert r.returncode == 2 and "DBRDISK1" in r.stderr
    nouuid = seed(folder / "none.dbr", [])
    r = run(*common, "--out", folder / "x6", "--seed", nouuid, "--volume-uuid", UUID)
    assert r.returncode == 2 and "no APFS volume superblock" in r.stderr
    newest_other = seed(folder / "other.dbr", [(2, UUID), (9, OTHER)])
    r = run(*common, "--out", folder / "x7", "--seed", newest_other, "--volume-uuid", UUID)
    assert r.returncode == 2 and OTHER in r.stderr                                    # the NEWEST superblock decides
    r = run(*common, "--out", folder / "t", "--seed", new_seed, "--volume-uuid", UUID)
    assert r.returncode == 2 and "NEW directory" in r.stderr
    r = run("--base", base, "--base-sha256", "0" * 64, "--image-bytes", SIZE, "--out", folder / "x8", "--set", "a=1")
    assert r.returncode == 2 and "SHA-256 mismatch" in r.stderr
    r = run(*common, "--out", folder / "x9", "--set", "a=1", "--set", "a=2")
    assert r.returncode == 2 and "repeated" in r.stderr
    r = run(*common, "--out", folder / "x10", "--set", "x=1 y=2")
    assert r.returncode == 2 and "plain characters" in r.stderr
print("PASS: seed-only and arg-only media differ by exactly the intended file, kernel/ADT untouched, base untouched, "
      "UUID taken from the NEWEST volume superblock, no-op/raw/uuid-less/foreign-uuid/duplicate/odd-value refusals")
