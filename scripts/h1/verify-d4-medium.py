#!/usr/bin/env python3
"""Read back the D4 local images only; refuse block/character device inputs."""
import argparse
import hashlib
import json
from pathlib import Path
import stat
import struct
import subprocess
import sys
import tempfile
import tomllib

project = Path(__file__).resolve().parents[2]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--out", type=Path, required=True)
parser.add_argument("--payload-report", type=Path, required=True)
args = parser.parse_args()
out = args.out.resolve()
report = json.loads(args.payload_report.read_text())
esp = out / "odin3-boot.img"
disk = out / "odin3-sd.img"
for path in (esp, disk):
    if not stat.S_ISREG(path.stat().st_mode):
        raise SystemExit(f"Not a regular image file: {path}")
with disk.open("rb") as stream:
    stream.seek(512)
    assert stream.read(8) == b"EFI PART", "GPT primary header"
    stream.seek(1024 + 32)
    start_lba, end_lba = struct.unpack("<QQ", stream.read(16))
    stream.seek(start_lba * 512)
    bpb = stream.read(512)
    bytes_per_sector = struct.unpack_from("<H", bpb, 11)[0]
    fat_sectors = struct.unpack_from("<H", bpb, 19)[0] or struct.unpack_from("<I", bpb, 32)[0]
assert bytes_per_sector == 512 and bpb[510:512] == b"\x55\xaa"
assert fat_sectors * bytes_per_sector <= (end_lba - start_lba + 1) * 512, "FAT must fit inside its GPT partition"
assert start_lba == 2048, "Expected ESP start; never infer a device offset"
entries = []
for line in (out / "MANIFEST.sha256").read_text().splitlines():
    digest, path, size, unit = line.split()
    assert len(digest) == 64 and unit == "B"
    entries.append((digest, path, int(size)))
scratch = project / "tmp"
scratch.mkdir(exist_ok=True)
with tempfile.TemporaryDirectory(prefix="medium-readback-", dir=scratch) as temporary:
    folder = Path(temporary)
    verified = {}
    for image in (str(esp), f"{disk}@@{start_lba * 512}"):
        for expected, path, size in entries:
            target = folder / "readback"
            subprocess.run(["mcopy", "-o", "-i", image, f"::/{path}", str(target)], check=True)
            data = target.read_bytes()
            actual = hashlib.sha256(data).hexdigest()
            assert len(data) == size and actual == expected, (image, path, actual)
            verified[path] = actual
        config = subprocess.check_output(["mtype", "-i", image, "::/EFI/UniXNU/unixnu.toml"])
        entry = next(e for e in tomllib.loads(config.decode())["entry"] if e["type"] == "xnu")
        assert entry["args"] == report["kernel_args"]
        words = entry["args"].split()
        for value in ("odin-gop-only=1", "offline-legacy-cpu-present=1"):
            assert value in words
        # 10 = the diagnostic page returns after 10 s without a swap; 0 = the system keeps the scanout for good.
        assert "boot-diag-resume=10" in words or "boot-diag-resume=0" in words
        for prefix in ("pagani-panel=", "pagani-touch=", "pagani-buttons="):
            assert not any(w.startswith(prefix) for w in words), prefix
        listing = subprocess.run(["mdir", "-i", image, "::/EFI/BOOT/BOOTAA64.EFI"], capture_output=True)
        assert listing.returncode != 0, "No tree-less fallback loader"
        bls = subprocess.check_output(["mtype", "-i", image, "::/loader/entries/unixnu.conf"])
        assert b"linux \\UNIXNU.EFI" in bls and b"devicetree \\ODIN3.DTB" in bls
        dtb = subprocess.check_output(["mtype", "-i", image, "::/ODIN3.DTB"])
        dts = subprocess.check_output(["dtc", "-q", "-I", "dtb", "-O", "dts"], input=dtb)
        assert b"reserved-memory" in dts and b"splash_region" in dts
        adt = folder / "kernel.adt"
        subprocess.run(["mcopy", "-o", "-i", image, "::/EFI/UniXNU/Payloads/kernel.macho.adt", str(adt)], check=True)
        sys.path.insert(0, str(project / "scripts/d1"))
        import adtlib
        root, end = adtlib.decode(adt.read_bytes())
        assert end == adt.stat().st_size
        chosen = next(n for n in root["children"] if n["name"] == "chosen")
        memory = next(n for n in chosen["children"] if n["name"] == "memory-map")
        assert "PackedRAMDisk" in memory["properties"] and "RAMDisk" not in memory["properties"]
        assert chosen["properties"].get("ephemeral-storage") == struct.pack("<I", 1), "Unchanged volatile RAM Data policy"
    for field, path in (
        ("payload_kernel_sha256", "EFI/UniXNU/Payloads/kernel.macho"),
        ("adt_sha256", "EFI/UniXNU/Payloads/kernel.macho.adt"),
        ("trust_cache_sha256", "EFI/UniXNU/Payloads/kernel.macho.trustcache"),
        ("data_ramdisk_sha256", "EFI/UniXNU/Payloads/scratch-data.img"),
    ):
        assert verified[path] == report[field], field
print(f"PASS: both regular images, FAT fits GPT, {len(entries)} manifest entries each, payload hashes/args, BLS splash handoff, PackedRAMDisk and peripheral/keystore guards")
