#!/usr/bin/env python3
"""Build an ESP FILE (never a card) from a known ESP image: replace the kernel and set boot arguments, nothing else.

  make-noedge-medium.py --base firmware/out/d3-adma64k/odin3-boot.img --base-sha256 9329b5bb... \\
      --kernel firmware/out/d3-noedge/kernel.macho --out firmware/out/d3-noedge/medium \\
      --set boot-diag-resume=120 --set odin-userland-log=30

Refuses a kernel that carries the Edge scheduler (override: --allow-edge), refuses an existing --out, and after writing
re-extracts both images and proves that only EFI/UniXNU/Payloads/kernel.macho (and EFI/UniXNU/unixnu.toml when an
argument really changed) differ.
No device access and no card access: the result is an image FILE; writing p1 is a separate, authorised step
(scripts/h1/sd-inspect.sh write <image> <sha256>).
"""
import argparse
import hashlib
import importlib.util
import json
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile
import tomllib

HERE = Path(__file__).resolve().parent
TOML = "EFI/UniXNU/unixnu.toml"
KERNEL = "EFI/UniXNU/Payloads/kernel.macho"


def load(name, file):
    spec = importlib.util.spec_from_file_location(name, HERE / file)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def sha(path):
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def extract(image, folder):
    subprocess.run(["mcopy", "-s", "-n", "-i", str(image), "::/", str(folder)], check=True, capture_output=True)
    return {str(p.relative_to(folder)): sha(p) for p in sorted(folder.rglob("*")) if p.is_file()}


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--base", type=Path, required=True)
    parser.add_argument("--base-sha256", required=True)
    parser.add_argument("--kernel", type=Path, required=True)
    parser.add_argument("--out", type=Path, required=True, help="new directory; receives odin3-boot.img, MANIFEST.sha256, candidate.json")
    parser.add_argument("--set", action="append", default=[], metavar="KEY=VALUE", help="boot argument to set (replaced if present, else appended)")
    parser.add_argument("--allow-edge", action="store_true")
    parser.add_argument("--image-bytes", type=int, default=201326592)
    args = parser.parse_args()
    if not re.fullmatch(r"[0-9a-f]{64}", args.base_sha256):
        parser.error("--base-sha256 must be 64 lowercase hex digits")
    if not args.base.is_file() or args.base.stat().st_size != args.image_bytes:
        parser.error(f"base must be a regular file of {args.image_bytes} bytes")
    if sha(args.base) != args.base_sha256:
        parser.error("base SHA-256 mismatch")
    if not args.kernel.is_file():
        parser.error("kernel is not a file")
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

    profile = load("kernel_profile", "check-kernel-profile.py")
    names = profile.symbols(args.kernel)
    if "sched_edge_dispatch" in names and not args.allow_edge:
        parser.error("the kernel carries the Edge scheduler; this tool builds the no-Edge medium (--allow-edge to override)")
    if "odin_touch_allocate" not in names:
        parser.error("the kernel lacks odin_touch_allocate: not an Odin kernel of this tree")
    candidate = load("candidate", "prepare-isolated-candidate.py")

    with tempfile.TemporaryDirectory(prefix="noedge-medium-") as tmp:
        tmp = Path(tmp)
        before = extract(args.base, tmp / "base")
        original = (tmp / "base" / TOML).read_text()
        entry = next(e for e in tomllib.loads(original)["entry"] if e["type"] == "xnu")
        new_args = candidate.arguments(entry["args"], replacements)
        text, count = re.subn(r'^args = ".*"$', "args = " + json.dumps(new_args), original, flags=re.M)
        if count != 1 or next(e for e in tomllib.loads(text)["entry"] if e["type"] == "xnu")["args"] != new_args:
            raise SystemExit("could not rewrite the args line exactly once")
        expected = {KERNEL} | ({TOML} if text != original else set())
        if len(new_args) > 1000:
            raise SystemExit(f"command line would be {len(new_args)} characters; the loader keeps 1000")
        args.out.mkdir(parents=True)
        image = args.out / "odin3-boot.img"
        shutil.copyfile(args.base, image)
        toml_file = tmp / "unixnu.toml"
        toml_file.write_text(text)
        subprocess.run(["mcopy", "-o", "-i", str(image), str(toml_file), "::/" + TOML], check=True)
        subprocess.run(["mcopy", "-o", "-i", str(image), str(args.kernel), "::/" + KERNEL], check=True)
        after = extract(image, tmp / "after")

    changed = sorted(k for k in set(before) | set(after) if before.get(k) != after.get(k))
    if changed != sorted(expected):
        raise SystemExit(f"unexpected difference between base and result: {changed}")
    if after[KERNEL] != sha(args.kernel):
        raise SystemExit("kernel in the image is not the kernel given")
    manifest = [f"{digest}  {name}" for name, digest in sorted(after.items())]
    (args.out / "MANIFEST.sha256").write_text("\n".join(manifest) + "\n")
    report = {"base_sha256": args.base_sha256, "image_sha256": sha(image), "image_bytes": image.stat().st_size,
              "kernel_sha256": sha(args.kernel), "kernel_bytes": args.kernel.stat().st_size,
              "kernel_has_edge": "sched_edge_dispatch" in names, "changed_files": changed,
              "kernel_args": new_args, "hardware_tested": False}
    (args.out / "candidate.json").write_text(json.dumps(report, indent=2) + "\n")
    print(json.dumps(report, indent=2))


if __name__ == "__main__":
    sys.exit(main())
