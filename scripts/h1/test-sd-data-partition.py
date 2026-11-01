#!/usr/bin/env python3
"""Host test of scripts/h1/sd_data_partition.py on a sparse image with the card's real geometry.

Independent checks: the image GPT is built here (not by the tool), the result is read back by util-linux `sfdisk`,
by GNU `parted`, and by the driver's own locator (SdStore.h) compiled from drivers/OdinSd. No device is touched.
"""
import importlib.util
import json
import os
import re
import shutil
import struct
import subprocess
import sys
import tempfile
import uuid
import zlib
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location("sdp", ROOT / "scripts/h1/sd_data_partition.py")
sdp = importlib.util.module_from_spec(spec)
spec.loader.exec_module(sdp)

TOTAL = 500006912
PARTS = [("UNIXNU", 2048, 819200, "C12A7328-F81F-11D2-BA4B-00A0C93EC93B"),
         ("OS", 821248, 11870275, "EBD0A0A2-B9E5-4433-87C0-68B6B72699C7"),
         ("SystemOS", 12693504, 6451200, "EBD0A0A2-B9E5-4433-87C0-68B6B72699C7")]
DISK_GUID = uuid.UUID("11111111-2222-3333-4444-555555555555")
failures = 0


def check(condition, text):
    global failures
    print(("  ok    " if condition else "  FAIL  ") + text)
    if not condition:
        failures += 1


def crc(data):
    return zlib.crc32(data) & 0xFFFFFFFF


def build_card(path, TOTAL=TOTAL):
    """A card laid out like the real one, written without the tool under test."""
    with open(path, "wb") as f:
        f.truncate(TOTAL * 512)
    blob = bytearray(16384)
    for i, (name, first, count, kind) in enumerate(PARTS):
        raw = uuid.UUID(kind).bytes_le + uuid.UUID(int=0xA000 + i).bytes_le + struct.pack("<QQQ", first, first + count - 1, 0)
        raw += name.encode("utf-16-le").ljust(72, b"\0")
        blob[i * 128:(i + 1) * 128] = raw
    blob = bytes(blob)

    def header(me, alt, array):
        h = bytearray(struct.pack("<8sIIIIQQQQ16sQIII", b"EFI PART", 0x10000, 92, 0, 0, me, alt, 34, TOTAL - 34,
                                  DISK_GUID.bytes_le, array, 128, 128, crc(blob)))
        h[16:20] = struct.pack("<I", crc(bytes(h)))
        return bytes(h).ljust(512, b"\0")

    mbr = bytearray(512)
    mbr[446 + 4] = 0xEE
    mbr[446 + 8:446 + 16] = struct.pack("<II", 1, 0xFFFFFFFF)
    mbr[510:512] = b"\x55\xaa"
    fd = os.open(path, os.O_RDWR)
    for lba, data in ((0, bytes(mbr)), (1, header(1, TOTAL - 1, 2)), (2, blob), (TOTAL - 33, blob), (TOTAL - 1, header(TOTAL - 1, 1, TOTAL - 33))):
        os.pwrite(fd, data, lba * 512)
    sentinels = {}
    for name, first, count, _ in PARTS:
        for lba in (first, first + 1, first + count // 2, first + count - 1):
            sentinels[lba] = os.urandom(512)
    for lba in (TOTAL - 8192, TOTAL - 8192 + 63, 19144704, 19144704 + 7, 19144704 + 8, 19144704 + 2048, 19144704 + 100000, 40000000):
        sentinels[lba] = os.urandom(512)       # log, and stale garbage where p4 will go (must be zeroed only at its first 4 KiB)
    for lba, data in sentinels.items():
        os.pwrite(fd, data, lba * 512)
    os.close(fd)
    return sentinels


def read_sector(path, lba, count=1):
    with open(path, "rb") as f:
        f.seek(lba * 512)
        return f.read(count * 512)


def fingerprint(path, sentinels):
    """Hash of the sectors a bug could plausibly change (a sparse 238 GB image is too big to read whole)."""
    import hashlib
    h = hashlib.sha256()
    for lba in list(range(0, 40)) + list(range(TOTAL - 40, TOTAL)) + sorted(sentinels) + [p[1] for p in PARTS] + [TOTAL // 2]:
        h.update(read_sector(path, lba))
    return h.hexdigest()


def run(*args, ok=True):
    result = subprocess.run(args, capture_output=True, text=True)
    if ok and result.returncode:
        print(result.stdout, result.stderr)
    return result


def tool(*args):
    return run(sys.executable, "-I", str(ROOT / "scripts/h1/sd_data_partition.py"), *args)


def main():
    work_root = ROOT / "tmp"
    work_root.mkdir(exist_ok=True)
    with tempfile.TemporaryDirectory(dir=work_root) as tmp:
        tmp = Path(tmp)
        image = tmp / "card.img"
        sentinels = build_card(image)
        guid = "7f3d2c1a-55aa-4e0b-9c21-0d8d6a4b1e99"

        print("== the image itself is a valid GPT for the independent tools")
        check(run("sfdisk", "-V", str(image)).returncode == 0, "sfdisk -V accepts the starting card")
        before_dump = run("sfdisk", "-d", str(image)).stdout
        check(before_dump.count("start=") == 3, "sfdisk sees 3 partitions before")

        print("== inspect and plan are read-only")
        digest = fingerprint(image, sentinels)
        r = tool("inspect", str(image))
        check(r.returncode == 0 and "SystemOS" in r.stdout, "inspect lists the partitions")
        check(tool("plan", str(image), "--guid", guid, "--out", str(tmp / "strict")).returncode == 0, "plan passes the card-identity check on the real geometry")
        other = tmp / "other.img"
        build_card(other, TOTAL=TOTAL - 4096)          # a card of another capacity must be refused without --any-card
        check(tool("plan", str(other), "--guid", guid, "--out", str(tmp / "other-plan")).returncode != 0, "another capacity is refused (identity check)")
        check(tool("plan", str(other), "--any-card", "--guid", guid, "--out", str(tmp / "other-ok")).returncode == 0, "--any-card is the explicit override")
        out = tmp / "plan"
        r = tool("plan", str(image), "--any-card", "--guid", guid, "--size-gib", "8", "--out", str(out))
        check(r.returncode == 0, "plan succeeds (--any-card: the test image has the real geometry but not the real serial)")
        plan = json.loads((out / "plan.json").read_text())
        part = plan["partition"]
        check(part["first_lba"] == 19144704 and part["first_lba"] % 2048 == 0, f"p4 starts at LBA {part['first_lba']}, 1 MiB aligned, right after p3")
        check(part["sectors"] == 8 * 2**30 // 512 and part["last_lba"] < TOTAL - 8192, "8 GiB and below the OdinSd boot log")
        check(part["bytes"] >= sdp.OVERLAY_MIN_BYTES, "large enough for the 1 GiB overlay (1073778688 B)")
        check([w["label"] for w in plan["writes"]] == ["backup array", "backup header", "primary array", "primary header", "data header block"],
              "write order: backup, then primary; header last")
        sums = subprocess.run(["sha256sum", "-c", "SHA256SUMS"], cwd=out, capture_output=True, text=True)
        check(sums.returncode == 0, "backup files match their SHA256SUMS")
        check(fingerprint(image, sentinels) == digest, "image bytes unchanged after plan")

        print("== apply refuses without the right id, or on a changed card")
        check(tool("apply", str(image), "--any-card", "--plan", str(out / "plan.json"), "--confirm", "0000000000000000").returncode != 0, "wrong plan id refused")
        check(fingerprint(image, sentinels) == digest, "refused apply wrote nothing")
        tampered = tmp / "tampered.img"
        shutil.copy(image, tampered)
        with open(tampered, "r+b") as f:
            f.seek(2 * 512 + 16)
            f.write(b"\x99")           # alter p1's unique GUID in the primary array only
        check(tool("apply", str(tampered), "--any-card", "--plan", str(out / "plan.json"), "--confirm", plan["id"]).returncode != 0, "diverged primary/backup arrays refused")

        print("== apply, recording every byte range written")
        writes = []
        original_write = sdp.Disk.write

        def spy(self, lba, data):
            writes.append((lba, len(data) // 512))
            original_write(self, lba, data)
        sdp.Disk.write = spy
        disk = sdp.Disk(str(image), write=True)
        try:
            sdp.apply(disk, plan, plan["id"], False)
        finally:
            disk.close()
            sdp.Disk.write = original_write
        expected_writes = [(w["lba"], w["sectors"]) for w in plan["writes"]]
        check(writes == expected_writes, f"exactly the planned ranges were written: {writes}")
        protected = [(PARTS[i][1], PARTS[i][1] + PARTS[i][2] - 1) for i in range(3)]
        check(all(not (lba + n - 1 >= a and lba <= b) for lba, n in writes for a, b in protected), "no write touched p1-p3")
        check(all(lba >= 1 for lba, _ in writes), "LBA 0 (protective MBR) never written")
        for lba, data in sentinels.items():
            now = read_sector(image, lba)
            if 19144704 <= lba < 19144704 + 8:
                check(now == b"\0" * 512, f"stale sector {lba} in the new Data header block is zeroed")
            else:
                check(now == data, f"sector {lba} untouched")
        check(read_sector(image, 19144704 + 8) != b"\0" * 512, "sector 8 of p4 (outside the header block) is not rewritten")

        print("== independent readers agree")
        check(run("sfdisk", "-V", str(image)).returncode == 0, "sfdisk -V accepts the result")
        dump = run("sfdisk", "-d", str(image)).stdout
        check(dump.count("start=") == 4, "sfdisk sees 4 partitions")
        m = re.search(r"start=\s*(\d+), size=\s*(\d+), type=([0-9A-F-]+), uuid=([0-9A-F-]+), name=\"UniXNUData\"", dump)
        check(bool(m) and int(m.group(1)) == part["first_lba"] and int(m.group(2)) == part["sectors"]
              and m.group(3) == "EBD0A0A2-B9E5-4433-87C0-68B6B72699C7" and m.group(4).lower() == guid,
              "sfdisk: UniXNUData start/size/type/GUID as planned")
        for name, first, count, _ in PARTS:
            check(re.search(rf"start=\s*{first}, size=\s*{count},.*name=\"{name}\"", dump) is not None, f"sfdisk: {name} unchanged at {first}")
        parted = run("parted", "-s", str(image), "unit", "s", "print")
        check("UniXNUData" in parted.stdout and "Error" not in parted.stderr, "parted lists UniXNUData without GPT errors")
        build = tmp / "locate"
        cc = run("c++", "-std=c++17", "-Wall", "-Wextra", "-Werror", "-fsanitize=address,undefined", "-I", str(ROOT / "devices/odin3-pagani/drivers/OdinSd"),
                 str(ROOT / "devices/odin3-pagani/drivers/OdinSd/test/locate_image.cpp"), "-o", str(build))
        check(cc.returncode == 0, "driver locator compiles")
        r = run(str(build), str(image), guid)
        check(r.returncode == 0 and r.stdout.strip() == f"OK first={part['first_lba']} sectors={part['sectors']}",
              f"driver locator (SdStore.h) accepts it: {r.stdout.strip()}")
        check(run(str(build), str(image), "7f3d2c1a-55aa-4e0b-9c21-0d8d6a4b1e98").returncode == 1, "driver locator rejects a different GUID")
        broken = tmp / "broken.img"
        shutil.copy(image, broken)
        with open(broken, "r+b") as f:
            f.seek((TOTAL - 33) * 512 + 3 * 128 + 40)
            f.write(b"\x01")
        check(run(str(build), str(broken), guid).returncode == 1, "driver locator rejects a torn primary/backup pair")

        print("== second apply, and size limits")
        check(tool("plan", str(image), "--any-card", "--out", str(tmp / "again")).returncode != 0, "a second plan refuses: UniXNUData exists")
        fresh = tmp / "fresh.img"
        build_card(fresh)
        check(tool("plan", str(fresh), "--any-card", "--size-gib", "0.5", "--out", str(tmp / "small")).returncode != 0, "0.5 GiB cannot hold the overlay")
        check(tool("plan", str(fresh), "--any-card", "--size-gib", "300", "--out", str(tmp / "huge")).returncode != 0, "300 GiB does not fit before the log")

        print("== restore.sh puts the original sectors back")
        script = (out / "restore.sh").read_text().replace('[ "$(blockdev --getss "$dev")" = 512 ] && ', "true && ").replace('[ "$(blockdev --getsz "$dev")" = %d ] || { echo "not the planned card" >&2; exit 1; }' % TOTAL, "true")
        (tmp / "restore-test.sh").write_text(script)
        r = run("bash", str(tmp / "restore-test.sh"), str(image), ok=False)
        # the script cds to its own directory: copy it next to the saved sectors
        shutil.copy(tmp / "restore-test.sh", out / "restore-test.sh")
        r = run("bash", str(out / "restore-test.sh"), str(image))
        check(r.returncode == 0, "restore ran on the image")
        check(run("sfdisk", "-d", str(image)).stdout == before_dump, "sfdisk dump after restore equals the dump before")

    print(f"\n{'FAIL' if failures else 'PASS'}: {failures} failure(s)")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
