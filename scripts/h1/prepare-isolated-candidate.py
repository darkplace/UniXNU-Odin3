#!/usr/bin/env python3
"""Prepare a pinned ESP FILE, never a card. Preserve the real base ADT identity.

touch3: qualified saved touch kernel, no Edge, identity only, 4 GiB.
mem4/mem6: the same optional pinned no-Edge kernel, only the owned window differs.
geometry: caller-pinned new diagnostic kernel, E/P tags, raw calibration, 4 GiB.
store: the same no-Edge kernel family with the SD Data store and/or the UHS stage selected by explicit arguments
(--data-persist 1|2 with --data-guid, --sd-uhs 1|2 with --sd-vqmmc). Nothing else changes; touch stays off.
Every other kind keeps Data persistence and GPU survey off. No stage4/guessed ID.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import shutil
import stat
import struct
import subprocess
import sys
import tempfile
import tomllib

BASE_SHA = "2f934846985b88755643dbd740a4d0b7af49a278fba3b6410a6e58585007b17a"
TOUCH_SHA = "af0bf7549f292b921a5ca8a842a040a02369ab935946b43c8570eb8e50bedf74"


def digest(path):
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def encode(node):
    result = bytearray(struct.pack("<II", len(node["properties"]), len(node["children"])))
    for name, value in node["properties"].items():
        assert len(name.encode()) < 32
        result += name.encode().ljust(32, b"\0") + struct.pack("<I", len(value)) + value + bytes(-len(value) % 4)
    for child in node["children"]:
        result += encode(child)
    return bytes(result)


def arguments(original, replacements):
    words = original.split()
    for key, value in replacements.items():
        assert sum(w.startswith(key + "=") for w in words) <= 1
        words = [w for w in words if not w.startswith(key + "=")]
        words.append(key + "=" + str(value))
    return " ".join(words)


GUID_PATTERN = re.compile(r"[0-9a-fA-F]{8}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{12}")


def store_arguments(persist, guid, uhs, vqmmc):
    """Boot-argument replacements for the 'store' kind; raises ValueError for any combination the driver would refuse."""
    if persist not in (0, 1, 2) or uhs not in (0, 1, 2):
        raise ValueError("--data-persist and --sd-uhs are 0, 1 or 2")
    if persist and not (guid and GUID_PATTERN.fullmatch(guid) and int(guid.replace("-", ""), 16)):
        raise ValueError("--data-persist needs the canonical --data-guid printed by sd_data_partition.py plan")
    if not persist and guid:
        raise ValueError("--data-guid without --data-persist")
    if uhs == 2:
        if vqmmc is None or not 0x40000 <= vqmmc < 0x50000 or vqmmc & 15:
            raise ValueError("--sd-uhs 2 needs --sd-vqmmc, the ldob8 address (0x4xxx0) printed by the --sd-uhs 1 boot")
    elif vqmmc is not None:
        raise ValueError("--sd-vqmmc only with --sd-uhs 2")
    result = {"sheng-data-persist": persist, "odin-sd-uhs": uhs}
    if persist:
        result["odin-data-guid"] = guid.lower()
    if uhs == 2:
        result["odin-sd-vqmmc"] = hex(vqmmc)
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--project", type=Path, required=True)
    parser.add_argument("--kind", choices=("touch3", "touch-profile", "mem4", "mem6", "geometry", "store"), required=True)
    parser.add_argument("--out", type=Path, required=True, help="New directory INSIDE project")
    parser.add_argument("--kernel", type=Path)
    parser.add_argument("--kernel-sha256")
    parser.add_argument("--data-persist", type=int, default=0)
    parser.add_argument("--data-guid")
    parser.add_argument("--sd-uhs", type=int, default=0)
    parser.add_argument("--sd-vqmmc", type=lambda text: int(text, 0))
    args = parser.parse_args()
    store = None
    if args.kind == "store":
        try:
            store = store_arguments(args.data_persist, args.data_guid, args.sd_uhs, args.sd_vqmmc)
        except ValueError as error:
            parser.error(str(error))
        if not args.kernel:
            parser.error("store needs the built --kernel and --kernel-sha256")
    elif args.data_persist or args.data_guid or args.sd_uhs or args.sd_vqmmc is not None:
        parser.error("--data-*/--sd-* belong to --kind store")
    root = args.project.resolve(strict=True)
    out = args.out.resolve()
    if not out.is_relative_to(root) or out.exists():
        parser.error("out must be a NEW directory inside project")
    if args.kind == "touch-profile":
        # Build-time snapshot, not another maintained driver fork. Keep the
        # original Clutch/SMP configuration for the first bus-identification test.
        source = Path(__file__).resolve().parents[2] / "devices/odin3-pagani"
        shutil.copytree(source, out, symlinks=True, ignore=shutil.ignore_patterns(".cache", "build", "__pycache__"))
        series = out / "patches/xnu/series"
        text = series.read_text()
        assert text.count("odin3-clusters.patch\n") == 1
        series.write_text(text.replace("odin3-clusters.patch\n", ""))
        dts = out / "dts/odin3.dts"
        text, count = re.subn(r'; unixnu,cluster-type = "[EP]"', "", dts.read_text())
        assert count == 8
        dts.write_text(text)
        (out / "HOST-PROFILE.json").write_text(json.dumps({"kind": "touch-no-edge", "source": str(source),
            "excluded_patch": "odin3-clusters.patch", "cpu_tags_removed": 8, "hardware_tested": False}, indent=2) + "\n")
        print("Prepared host-only touch profile (no AMP/Edge, no CPU tag changes)")
        return
    base_dir = root / "firmware/out/d3-handoff"
    base = base_dir / "odin3-boot.img"
    if not stat.S_ISREG(base.stat().st_mode) or digest(base) != BASE_SHA:
        parser.error("Require the pinned regular handoff ESP")
    if args.kind == "geometry" or args.kernel:
        if not args.kernel or not re.fullmatch(r"[a-f0-9]{64}", args.kernel_sha256 or ""):
            parser.error("a custom candidate requires --kernel and its exact --kernel-sha256")
        kernel = args.kernel.resolve(strict=True)
        if not kernel.is_relative_to(root) or not stat.S_ISREG(kernel.stat().st_mode) or digest(kernel) != args.kernel_sha256:
            parser.error("kernel path/type/SHA mismatch")
        symbols = subprocess.check_output(["llvm-nm", str(kernel)], text=True)
        if args.kind != "geometry" and (" _sched_edge_dispatch\n" in symbols or " _odin_touch_allocate\n" not in symbols):
            parser.error("touch/memory probes require OdinTouch and NO compiled Edge dispatch")
    else:
        if args.kernel or args.kernel_sha256:
            parser.error("--kernel-sha256 requires --kernel")
        kernel = root / "firmware/out/d3-touch/kernel.macho" if args.kind == "touch3" else None
        if kernel and (not stat.S_ISREG(kernel.stat().st_mode) or digest(kernel) != TOUCH_SHA):
            parser.error("Require the saved qualified touch kernel")
    sys.path.insert(0, str(root / "scripts/d1"))
    import adtlib

    def read(image, name):
        return subprocess.check_output(["mtype", "-i", str(image), "::/" + name])

    # Validate ALL golden inputs before making a candidate.
    manifest = []
    for line in (base_dir / "MANIFEST.sha256").read_text().splitlines():
        sha, name, size, unit = line.split()
        data = read(base, name)
        assert unit == "B" and len(data) == int(size) and hashlib.sha256(data).hexdigest() == sha
        manifest.append(name)
    adt_path = "EFI/UniXNU/Payloads/kernel.macho.adt"
    old_adt = read(base, adt_path)
    tree, end = adtlib.decode(old_adt)
    assert end == len(old_adt) and encode(tree) == old_adt
    chosen = adtlib.find(tree, "/chosen")["properties"]
    assert struct.unpack("<Q", chosen["darwinboot-memory-size"])[0] == 4 << 30
    cpus = adtlib.find(tree, "/cpus")["children"]
    assert len(cpus) == 8 and all("cluster-type" not in cpu["properties"] for cpu in cpus)
    if args.kind == "mem6":
        chosen["darwinboot-memory-size"] = struct.pack("<Q", 6 << 30)
    if args.kind == "touch3":
        arm_io = adtlib.find(tree, "/arm-io")
        assert adtlib.find(tree, "/arm-io/touch") is None
        arm_io["children"].append({"name": "touch", "properties": {
            "name": b"touch\0", "compatible": b"ayn,odin3-ft3519\0"}, "children": []})
    if args.kind == "geometry":
        for i, cpu in enumerate(cpus):
            cpu["properties"]["cluster-type"] = struct.pack("<I", ord("E" if i < 6 else "P"))
    config_path = "EFI/UniXNU/unixnu.toml"
    config = read(base, config_path).decode()
    entry = next(e for e in tomllib.loads(config)["entry"] if e["type"] == "xnu")
    replacements = {"odin-gpu-survey": 0, "sheng-data-persist": 0, "odin-touch": 3 if args.kind == "touch3" else 0}
    if args.kind == "geometry":
        replacements["odin-fb-calibrate"] = 1
    if args.kind == "touch3":
        replacements["boot-diag-resume"] = 10
    if store is not None:
        replacements.update(store)
        replacements["boot-diag-resume"] = 10      # the page must come back on a static frame to be photographed
    new_args = arguments(entry["args"], replacements)
    assert "odin-touch-id=" not in new_args and "sheng-fb-rotate=1" in new_args
    config, count = re.subn(r'^args = ".*"$', "args = " + json.dumps(new_args), config, flags=re.M)
    assert count == 1 and next(e for e in tomllib.loads(config)["entry"] if e["type"] == "xnu")["args"] == new_args
    if "boot-args" in chosen:
        chosen["boot-args"] = new_args.encode() + b"\0"
    out.mkdir(parents=True)
    image = out / "odin3-boot.img"
    shutil.copyfile(base, image)
    with tempfile.TemporaryDirectory(dir=out) as tmp:
        tmp = Path(tmp)
        for name, data in ((adt_path, encode(tree)), (config_path, config.encode())):
            file = tmp / "input"
            file.write_bytes(data)
            subprocess.run(["mcopy", "-o", "-i", str(image), str(file), "::/" + name], check=True)
        if kernel:
            subprocess.run(["mcopy", "-o", "-i", str(image), str(kernel), "::/EFI/UniXNU/Payloads/kernel.macho"], check=True)
    entries = []
    for name in manifest:
        data = read(image, name)
        if name not in (config_path, adt_path, "EFI/UniXNU/Payloads/kernel.macho"):
            assert data == read(base, name), "Unrelated golden input changed: " + name
        entries.append(f"{hashlib.sha256(data).hexdigest()}  {name}  {len(data)} B")
    (out / "MANIFEST.sha256").write_text("\n".join(entries) + "\n")
    expected_kernel = kernel.read_bytes() if kernel else read(base, "EFI/UniXNU/Payloads/kernel.macho")
    assert read(image, "EFI/UniXNU/Payloads/kernel.macho") == expected_kernel
    assert read(image, adt_path) == encode(tree) and read(image, config_path) == config.encode()
    report = {"kind": args.kind, "base_sha256": BASE_SHA, "image_bytes": image.stat().st_size,
              "image_sha256": digest(image), "kernel_bytes": len(expected_kernel),
              "kernel_sha256": hashlib.sha256(expected_kernel).hexdigest(),
              "adt_sha256": hashlib.sha256(encode(tree)).hexdigest(), "kernel_args": new_args,
              "owned_memory_bytes": struct.unpack("<Q", chosen["darwinboot-memory-size"])[0],
              "hardware_tested": False, "data_persistence_enabled": bool(store and store["sheng-data-persist"]),
              "sd_uhs_mode": store["odin-sd-uhs"] if store else 0}
    (out / "candidate.json").write_text(json.dumps(report, indent=2) + "\n")
    print(json.dumps(report, indent=2))


if __name__ == "__main__":
    main()
