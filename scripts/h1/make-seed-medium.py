#!/usr/bin/env python3
"""Build an ESP FILE (never a card) from a known ESP image: replace the Data seed and/or set boot arguments, nothing else.

  make-seed-medium.py --base firmware/out/d3-noedge2/medium/odin3-boot.img --base-sha256 42d6152b... \\
      --out firmware/out/d3-fairplay/medium-T --seed firmware/out/d3-fairplay/data.dbrdisk \\
      --volume-uuid 5D8FE226-65AB-46F4-92F2-C801EE86F627
  make-seed-medium.py --base ... --base-sha256 ... --out firmware/out/d3-fairplay/medium-C --set sheng-data-reset=1

The kernel, the ADT, the trust cache, DarwinBoot and every other file of the base stay byte-identical: after writing, both images
are extracted again and the set of files that differ must be exactly {the seed} and/or {unixnu.toml, only if an argument really
changed}. A seed must be a DBRDISK1 file whose newest APFS volume superblock carries --volume-uuid, because the kernel compiles
that UUID in (CONFIG_XNU_OWNER_DATA_VOLUME_UUID) and finds its Data volume by it.
Refuses an existing --out. No device access and no card access: writing p1 is a separate, authorised step.
"""
import argparse
import hashlib
import importlib.util
import json
from pathlib import Path
import re
import shutil
import struct
import subprocess
import sys
import tempfile
import tomllib
import uuid

HERE = Path(__file__).resolve().parent
PROJECT = HERE.parents[1]
PACKED = PROJECT / "devices/odin3-pagani/modules/Pagani/drivers/PaganiDarwinBoot/Test/packed_ramdisk.py"
TOML = "EFI/UniXNU/unixnu.toml"
SEED = "EFI/UniXNU/Payloads/scratch-data.img"
KERNEL = "EFI/UniXNU/Payloads/kernel.macho"


def load(name, file):
    spec = importlib.util.spec_from_file_location(name, file)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def sha(path):
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def extract(image, folder):
    subprocess.run(["mcopy", "-s", "-n", "-i", str(image), "::/", str(folder)], check=True, capture_output=True)
    return {str(p.relative_to(folder)): sha(p) for p in sorted(folder.rglob("*")) if p.is_file()}


def seed_volume_uuid(packed, path):
    """UUID of the newest APFS volume superblock (object type APSB, highest transaction id) inside a DBRDISK1 file."""
    _, extents = packed.regions(path.read_bytes())
    newest = None
    for offset, data in extents:
        for at in range(0, len(data), 4096):
            page = bytes(data[at:at + 4096])
            if page[32:36] == b"APSB":
                xid = struct.unpack_from("<Q", page, 16)[0]
                if newest is None or xid > newest[0]:
                    newest = (xid, str(uuid.UUID(bytes=page[240:256])).upper())
    return newest


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--base", type=Path, required=True)
    parser.add_argument("--base-sha256", required=True)
    parser.add_argument("--out", type=Path, required=True, help="new directory; receives odin3-boot.img, MANIFEST.sha256, candidate.json")
    parser.add_argument("--seed", type=Path, help="DBRDISK1 file that replaces scratch-data.img")
    parser.add_argument("--volume-uuid", help="required with --seed: the UUID the kernel was built with")
    parser.add_argument("--set", action="append", default=[], metavar="KEY=VALUE", help="boot argument to set (replaced if present, else appended)")
    parser.add_argument("--image-bytes", type=int, default=201326592)
    args = parser.parse_args()
    if not re.fullmatch(r"[0-9a-f]{64}", args.base_sha256):
        parser.error("--base-sha256 must be 64 lowercase hex digits")
    if not args.base.is_file() or args.base.stat().st_size != args.image_bytes:
        parser.error(f"base must be a regular file of {args.image_bytes} bytes")
    if sha(args.base) != args.base_sha256:
        parser.error("base SHA-256 mismatch")
    if not args.seed and not args.set:
        parser.error("nothing to change: give --seed and/or --set")
    if args.out.exists():
        parser.error("out must be a NEW directory")
    replacements = {}
    for item in args.set:
        key, sep, value = item.partition("=")
        if not sep or not key or not re.fullmatch(r"[A-Za-z0-9_.:-]+", key) or not re.fullmatch(r"[A-Za-z0-9_.:,+@/-]*", value):
            parser.error(f"--set needs KEY=VALUE with plain characters: {item!r}")
        if key in replacements:
            parser.error(f"--set repeated: {key}")
        replacements[key] = value

    packed = load("packed_ramdisk", PACKED)
    seed_info = None
    if args.seed:
        if not args.seed.is_file():
            parser.error("--seed is not a file")
        if not args.volume_uuid or not re.fullmatch(r"[0-9A-Fa-f]{8}(-[0-9A-Fa-f]{4}){3}-[0-9A-Fa-f]{12}", args.volume_uuid):
            parser.error("--seed needs --volume-uuid (canonical form)")
        try:
            info, _ = packed.inspect(args.seed)
        except ValueError as error:
            parser.error(f"--seed is not a valid DBRDISK1 file: {error}")
        if not info["packed"]:
            parser.error("--seed must be a DBRDISK1 file, not a raw image")
        newest = seed_volume_uuid(packed, args.seed)
        if newest is None:
            parser.error("--seed holds no APFS volume superblock")
        if newest[1] != args.volume_uuid.upper():
            parser.error(f"--seed volume UUID {newest[1]} is not the kernel's {args.volume_uuid.upper()}")
        seed_info = dict(info, volume_uuid=newest[1], volume_superblock_xid=newest[0])

    candidate = load("candidate", HERE / "prepare-isolated-candidate.py")
    with tempfile.TemporaryDirectory(prefix="seed-medium-") as tmp:
        tmp = Path(tmp)
        before = extract(args.base, tmp / "base")
        original = (tmp / "base" / TOML).read_text()
        entry = next(e for e in tomllib.loads(original)["entry"] if e["type"] == "xnu")
        new_args = candidate.arguments(entry["args"], replacements)
        text, count = re.subn(r'^args = ".*"$', "args = " + json.dumps(new_args), original, flags=re.M)
        if count != 1 or next(e for e in tomllib.loads(text)["entry"] if e["type"] == "xnu")["args"] != new_args:
            raise SystemExit("could not rewrite the args line exactly once")
        if entry.get("initrd") != "Payloads/scratch-data.img":
            raise SystemExit(f"the base entry does not boot Payloads/scratch-data.img: {entry.get('initrd')!r}")
        expected = ({SEED} if args.seed and sha(args.seed) != before[SEED] else set()) | ({TOML} if text != original else set())
        if not expected:
            raise SystemExit("the change is a no-op: the seed and the arguments are what the base already has")
        if len(new_args) > 1000:
            raise SystemExit(f"command line would be {len(new_args)} characters; the loader keeps 1000")
        args.out.mkdir(parents=True)
        image = args.out / "odin3-boot.img"
        shutil.copyfile(args.base, image)
        if TOML in expected:
            toml_file = tmp / "unixnu.toml"
            toml_file.write_text(text)
            subprocess.run(["mcopy", "-o", "-i", str(image), str(toml_file), "::/" + TOML], check=True)
        if SEED in expected:
            subprocess.run(["mcopy", "-o", "-i", str(image), str(args.seed), "::/" + SEED], check=True)
        after = extract(image, tmp / "after")

    changed = sorted(k for k in set(before) | set(after) if before.get(k) != after.get(k))
    if changed != sorted(expected):
        raise SystemExit(f"unexpected difference between base and result: {changed}")
    if args.seed and after[SEED] != sha(args.seed):
        raise SystemExit("seed in the image is not the seed given")
    if after[KERNEL] != before[KERNEL]:
        raise SystemExit("the kernel changed")
    manifest = [f"{digest}  {name}" for name, digest in sorted(after.items())]
    (args.out / "MANIFEST.sha256").write_text("\n".join(manifest) + "\n")
    report = {"base_sha256": args.base_sha256, "image_sha256": sha(image), "image_bytes": image.stat().st_size,
              "kernel_sha256": after[KERNEL], "changed_files": changed, "seed": seed_info,
              "seed_file_sha256": after[SEED], "kernel_args": new_args, "hardware_tested": False}
    (args.out / "candidate.json").write_text(json.dumps(report, indent=2) + "\n")
    print(json.dumps(report, indent=2))


if __name__ == "__main__":
    sys.exit(main())
