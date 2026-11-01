#!/usr/bin/env python3
"""Extract input/topology facts from local OTA files; never opens a device.

--project owns firmware/ota and firmware/AYN-linux. Output is deterministic JSON;
all DTB variants are checked, not just the first one. Requires fdtget.
"""
import argparse
import hashlib
import json
from pathlib import Path
import struct
import subprocess
import tempfile


def pin(path):
    data = path.read_bytes()
    return {"bytes": len(data), "sha256": hashlib.sha256(data).hexdigest()}


def get(path, node, prop, kind="x"):
    result = subprocess.run(["fdtget", "-t", kind, str(path), node, prop],
                            capture_output=True, text=True, check=True)
    return result.stdout.strip() if kind == "s" else [int(x, 16) for x in result.stdout.split()]


def children(path, node):
    return subprocess.check_output(["fdtget", "-l", str(path), node], text=True).split()


def walk(path, node="/"):
    yield node
    for name in children(path, node):
        yield from walk(path, node.rstrip("/") + "/" + name)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--project", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    project = args.project.resolve()
    ota = project / "firmware/ota"
    out = {"inputs": {}, "variants": [], "overlays": []}
    for name in ("vendor_boot.img", "dtbo.img"):
        out["inputs"]["firmware/ota/" + name] = pin(ota / name)
    source_root = project / "firmware/AYN-linux"
    out["ayn_revision"] = subprocess.check_output(["git", "-C", str(source_root), "rev-parse", "HEAD"], text=True).strip()
    assert out["ayn_revision"] == "d0bd1239126dbd52b2bad91de1db0020de26977c"
    for name in ("cq8725s-ayn-odin3.dts", "cq8725s-ayn-common.dtsi", "sm8750.dtsi"):
        path = source_root / "arch/arm64/boot/dts/qcom" / name
        out["inputs"][str(path.relative_to(project))] = pin(path)
    out["references"] = {}
    for component in ("linux", "android_kernel_ayn_cq8725s-modules", "UniXNU-sheng", "ShengNovatekTouch", "ShengHidInput"):
        for path in sorted((project / "firmware/ref-input" / component).rglob("*")):
            if path.is_file():
                out["references"][str(path.relative_to(project))] = pin(path)
    flash = project / "firmware/ref-input/android_kernel_ayn_qcs8550-modules/efe02f64a536b04b666109a4e392dfa81e927f08/qcom/opensource/touch-drivers/focaltech_touch/focaltech_flash/focaltech_upgrade_ft3519.c"
    out["references"][str(flash.relative_to(project))] = pin(flash)
    data = (ota / "vendor_boot.img").read_bytes()
    assert data[:8] == b"VNDRBOOT"
    version, page = struct.unpack_from("<II", data, 8)
    assert version == 4
    ramdisk = struct.unpack_from("<I", data, 24)[0]
    header, size = struct.unpack_from("<II", data, 8 + 4 * 5 + 2048 + 4 + 16)
    align = lambda n: (n + page - 1) // page * page
    blob = data[align(header) + align(ramdisk):][:size]
    # All temporary files remain under the permitted project directory.
    with tempfile.TemporaryDirectory(dir=project / "firmware/out", prefix="input-dt-") as temporary:
        temp = Path(temporary)
        offset = 0
        while offset < len(blob):
            assert blob[offset:offset + 4] == b"\xd0\x0d\xfe\xed"
            n = struct.unpack_from(">I", blob, offset + 4)[0]
            assert 40 <= n <= len(blob) - offset
            f = temp / "board.dtb"
            f.write_bytes(blob[offset:offset + n])
            provider = get(f, "/__symbols__", "scmi_perf", "s")
            phandles = {get(f, provider, "phandle")[0]: provider}
            v = {"index": len(out["variants"]), "dtb": pin(f), "cpus": []}
            for node in children(f, "/cpus"):
                if not node.startswith("cpu@"):
                    continue
                node = "/cpus/" + node
                clocks = get(f, node, "clocks")
                v["cpus"].append({"node": node, "reg": get(f, node, "reg"),
                                  "capacity_dmips_mhz": get(f, node, "capacity-dmips-mhz")[0],
                                  "clocks": clocks, "clock_provider": phandles[clocks[0]]})
            engine = get(f, "/__symbols__", "qupv3_se4_i2c", "s")
            wrapper = engine.rsplit("/", 1)[0]
            v["engine"] = {"path": engine, **{p: get(f, engine, p) for p in ("reg", "interrupts", "clocks")}}
            v["wrapper"] = {"path": wrapper, "reg": get(f, wrapper, "reg"), "iommus": get(f, wrapper, "iommus")}
            out["variants"].append(v)
            offset += n
        data = (ota / "dtbo.img").read_bytes()
        magic, total, header, esize, count, start, page, version = struct.unpack_from(">8I", data)
        assert magic == 0xd7b7ab1e and esize >= 32 and total <= len(data)
        for i in range(count):
            size, offset = struct.unpack_from(">II", data, start + i * esize)
            f = temp / "overlay.dtb"
            f.write_bytes(data[offset:offset + size])
            assert len(f.read_bytes()) == size
            for fragment in children(f, "/"):
                if not fragment.startswith("fragment@"):
                    continue
                node = "/" + fragment + "/__overlay__"
                if "focaltech@38" in children(f, node):
                    node += "/focaltech@38"
                    out["overlays"].append({"index": i, "dtb": pin(f), "node": node,
                        **{p: get(f, node, p) for p in ("reg", "interrupts", "focaltech,ic-type", "focaltech,max-touch-number", "focaltech,display-coords")},
                        "compatible": get(f, node, "compatible", "s")})
    assert len(out["variants"]) == 11 and out["overlays"]
    for v in out["variants"]:
        assert [c["reg"] for c in v["cpus"]] == [[0, n] for n in (0, 0x100, 0x200, 0x300, 0x400, 0x500, 0x10000, 0x10100)]
        assert [c["clocks"][1] for c in v["cpus"]] == [0] * 6 + [1] * 2
        assert v["cpus"][6]["capacity_dmips_mhz"] > v["cpus"][0]["capacity_dmips_mhz"]
        assert v["engine"]["reg"] == [0xa90000, 0x4000]
        assert v["engine"]["interrupts"] == [0, 357, 4]
        assert v["wrapper"]["iommus"][1:] == [0xa3, 0]
    for overlay in out["overlays"]:
        assert overlay["reg"] == [0x38] and overlay["focaltech,ic-type"] == [0x35190489]
        assert overlay["focaltech,display-coords"] == [0, 0, 1080, 1920]
    args.output.write_text(json.dumps(out, indent=2) + "\n")
    print("PASS: 11 vendor DT variants, 6+2 Oryon/SCMI domains, FocalTech FT3519 I2C4 overlay", pin(args.output))


if __name__ == "__main__":
    main()
