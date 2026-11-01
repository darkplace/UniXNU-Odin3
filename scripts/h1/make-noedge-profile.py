#!/usr/bin/env python3
"""Snapshot the current Odin device tree WITHOUT the Edge scheduler, ready for scripts/h1/build-pagani-kernel.sh.

Why: the kernel that reached the lock screen (ea877510) was built from such a snapshot; the default device tree
carries odin3-clusters.patch (defines __ARM_AMP__, so XNU compiles the Edge scheduler) and every kernel built from it
since has failed to reach the lock screen (change ui-handoff-rca). This is the one difference to test.

  make-noedge-profile.py NAME [--project DIR]

Writes DIR/firmware/out/NAME/profile (must not exist). Nothing else changes: same drivers, same series minus the
clusters patch, same DTS minus the eight cluster-type tags (the ADT already shipped has none). Prints the build line.
No device access, no card access.
"""
import argparse
import os
from pathlib import Path
import subprocess
import sys


def main():
    here = Path(__file__).resolve().parent
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("name")
    parser.add_argument("--project", type=Path, default=here.parents[1])
    parser.add_argument("--out", type=Path, help="override DIR/firmware/out/NAME/profile (must be inside the project)")
    args = parser.parse_args()
    project = args.project.resolve(strict=True)
    out = (args.out or project / "firmware/out" / args.name / "profile").resolve()
    done = subprocess.run([sys.executable, "-I", str(here / "prepare-isolated-candidate.py"), "--project", str(project),
                           "--kind", "touch-profile", "--out", str(out)])
    if done.returncode:
        raise SystemExit(done.returncode)
    series = (out / "patches/xnu/series").read_text().split()
    assert "odin3-clusters.patch" not in series, "clusters patch still in the series"
    assert "unixnu,cluster-type =" not in (out / "dts/odin3.dts").read_text(), "cluster-type tags still in the DTS"
    # The snapshot sits three levels below firmware/, the device tree two levels below the project: fix the SoC path.
    soc = project / "firmware/UniXNU-sm8750-common"
    relative = os.path.relpath(soc, out)
    (out / "local.toml").write_text(
        "# Local overrides for the no-Edge snapshot: same SoC pin, Pagani drivers under modules/, no network.\n"
        '[[source]]\nname = "sm8750"\nkind = "soc"\n'
        f'local = "{relative}"\n\n[local]\ndrivers = "modules/Pagani/drivers"\n')
    if not soc.is_dir():
        print(f"warning: {soc} does not exist here; build from the main checkout", file=sys.stderr)
    print(f"profile  {out}")
    print(f"series   {len(series)} patches, no odin3-clusters.patch")
    print(f"build    DEVICE={out} bash scripts/h1/build-pagani-kernel.sh build kernel")
    print("check    python3 scripts/h1/check-kernel-profile.py <kernel.macho> --expect-no-edge")


if __name__ == "__main__":
    main()
