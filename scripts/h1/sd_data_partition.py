#!/usr/bin/env python3
"""Plan, back up and (only when told to) create the Data partition `UniXNUData` (p4) on the Odin 3 microSD.

The card carries p1 UNIXNU (ESP), p2 OS and p3 SystemOS, byte-identical to signed policies. This tool never reads
or writes inside p1-p3. It only ever touches:
  * LBA 1..33 (primary GPT header + 128-entry array) and the last 33 sectors (backup array + header),
  * the first 8 sectors (4 KiB) of the new partition, zeroed so the SHNGDATA overlay starts blank.
LBA 0 (protective MBR) and entries 0..2 are verified byte-identical after the write.

  inspect DEVICE_OR_IMAGE                       read-only report
  plan    DEVICE_OR_IMAGE --guid UUID --out DIR  read-only; writes DIR/plan.json and the backup of every sector that
                                                 will change (+ SHA256SUMS, restore.sh). Prints the plan id.
  apply   DEVICE_OR_IMAGE --plan DIR/plan.json --confirm PLAN_ID
                                                 the only command that writes. Needs the id printed by `plan`.

Run on a block device as root (pkexec/sudo); on an image file for the host tests. SD cards through a reader report
512-byte logical sectors (the 4Kn rule applies to the internal UFS, not to this card): the tool requires 512 and
requires the backup header at the last LBA, so a mismatch stops it.
"""
import argparse
import binascii
import fcntl
import hashlib
import json
import os
import struct
import sys
import uuid
import zlib
from pathlib import Path

SECTOR = 512
BLKSSZGET = 0x1268
ENTRY_BYTES = 128
ENTRY_COUNT = 128
ARRAY_SECTORS = ENTRY_COUNT * ENTRY_BYTES // SECTOR          # 32
LOG_RESERVED = 8192                                         # OdinSd keeps its 32 KiB boot log at capacity-8192
BASIC_DATA = uuid.UUID("EBD0A0A2-B9E5-4433-87C0-68B6B72699C7")
PART_NAME = "UniXNUData"
ALIGN = 2048                                                # 1 MiB
HEADER_ZERO_SECTORS = 8                                     # first 4 KiB of the partition (SHNGDATA header block)
OVERLAY_MIN_BYTES = (262144 + 8 + 1) * 4096                 # 1 GiB seed: 262144 pages + header + 8 bitmap blocks
# Facts about this card, measured earlier (docs/builds/odin3-persistence-20261029.md, odin3-root-sd-*.md).
EXPECTED = {
    "sectors": 500006912,
    "names": ["UNIXNU", "OS", "SystemOS"],
    "os": (821248, 11870275),
    "systemos": (12693504, 6451200),
}


class Stop(SystemExit):
    pass


def stop(message):
    raise Stop(f"STOP: {message}")


def crc(data):
    return zlib.crc32(data) & 0xFFFFFFFF


class Disk:
    """Random access to a block device or an image file."""

    def __init__(self, path, write=False):
        self.path = path
        flags = os.O_RDWR if write else os.O_RDONLY
        self.fd = os.open(path, flags | getattr(os, "O_CLOEXEC", 0))
        self.is_block = (os.stat(path).st_mode & 0o170000) == 0o060000
        self.size = os.lseek(self.fd, 0, os.SEEK_END)
        if self.is_block:
            buffer = bytearray(4)
            fcntl.ioctl(self.fd, BLKSSZGET, buffer)
            self.logical = struct.unpack("I", buffer)[0]
        else:
            self.logical = SECTOR
        self.sectors = self.size // SECTOR

    def read(self, lba, count=1):
        data = os.pread(self.fd, count * SECTOR, lba * SECTOR)
        if len(data) != count * SECTOR:
            stop(f"short read at LBA {lba} (+{count})")
        return data

    def write(self, lba, data):
        if len(data) % SECTOR:
            stop("unaligned write")
        if os.pwrite(self.fd, data, lba * SECTOR) != len(data):
            stop(f"short write at LBA {lba}")

    def sync(self):
        os.fsync(self.fd)

    def close(self):
        os.close(self.fd)


def parse_header(sector, self_lba, alt_lba, total):
    if sector[:8] != b"EFI PART":
        stop(f"no GPT signature at LBA {self_lba}")
    (revision, size, header_crc, reserved, my_lba, alt, first_usable, last_usable, disk_guid, entries_lba,
     count, entry_size, entries_crc) = struct.unpack_from("<IIIIQQQQ16sQIII", sector, 8)
    if revision != 0x10000 or size < 92 or size > SECTOR or reserved:
        stop(f"GPT header at LBA {self_lba} is malformed (revision {revision:#x}, size {size})")
    check = bytearray(sector[:size])
    check[16:20] = b"\0\0\0\0"
    if crc(bytes(check)) != header_crc:
        stop(f"GPT header CRC mismatch at LBA {self_lba}")
    if my_lba != self_lba or alt != alt_lba:
        stop(f"GPT header at LBA {self_lba} points to {my_lba}/{alt}, expected {self_lba}/{alt_lba}")
    if count != ENTRY_COUNT or entry_size != ENTRY_BYTES:
        stop(f"GPT array is {count}x{entry_size}, this tool handles {ENTRY_COUNT}x{ENTRY_BYTES} only")
    if not (34 <= first_usable <= last_usable < total - 33):
        stop("GPT usable range is inconsistent with the capacity")
    return {"size": size, "my": my_lba, "alt": alt, "first_usable": first_usable, "last_usable": last_usable,
            "disk_guid": uuid.UUID(bytes_le=disk_guid), "entries_lba": entries_lba, "entries_crc": entries_crc}


def parse_entries(blob):
    entries = []
    for index in range(ENTRY_COUNT):
        raw = blob[index * ENTRY_BYTES:(index + 1) * ENTRY_BYTES]
        if raw[:16] == b"\0" * 16:
            continue
        first, last, attributes = struct.unpack_from("<QQQ", raw, 32)
        name = raw[56:128].decode("utf-16-le").split("\0")[0]
        entries.append({"index": index, "type": uuid.UUID(bytes_le=raw[:16]), "guid": uuid.UUID(bytes_le=raw[16:32]),
                        "first": first, "last": last, "attributes": attributes, "name": name, "raw": raw})
    return entries


def inspect(disk):
    if disk.logical != SECTOR:
        stop(f"logical sector size is {disk.logical}, this card is addressed in {SECTOR}")
    total = disk.sectors
    primary = parse_header(disk.read(1), 1, total - 1, total)
    backup = parse_header(disk.read(total - 1), total - 1, 1, total)
    if primary["entries_lba"] != 2 or backup["entries_lba"] != total - 33:
        stop(f"GPT arrays are at {primary['entries_lba']}/{backup['entries_lba']}, expected 2/{total - 33}")
    primary_blob = disk.read(2, ARRAY_SECTORS)
    backup_blob = disk.read(total - 33, ARRAY_SECTORS)
    if primary_blob != backup_blob:
        stop("primary and backup entry arrays differ; repair the card's GPT first, this tool will not guess")
    for header, blob, label in ((primary, primary_blob, "primary"), (backup, backup_blob, "backup")):
        if crc(blob) != header["entries_crc"]:
            stop(f"{label} entry array CRC mismatch")
    for key in ("disk_guid", "first_usable", "last_usable"):
        if primary[key] != backup[key]:
            stop(f"primary/backup headers disagree on {key}")
    return {"total": total, "primary": primary, "backup": backup, "blob": primary_blob, "entries": parse_entries(primary_blob),
            "lba0": disk.read(0)}


def free_ranges(report):
    used = sorted((e["first"], e["last"]) for e in report["entries"])
    cursor = report["primary"]["first_usable"]
    log_first = report["total"] - LOG_RESERVED
    limit = min(report["primary"]["last_usable"], log_first - 1)
    gaps = []
    for first, last in used:
        if first > cursor:
            gaps.append((cursor, first - 1))
        cursor = max(cursor, last + 1)
    if cursor <= limit:
        gaps.append((cursor, limit))
    return gaps


def describe(report):
    lines = [f"capacity {report['total']} sectors ({report['total'] * SECTOR / 2**30:.2f} GiB), disk GUID {report['primary']['disk_guid']}",
             f"usable {report['primary']['first_usable']}..{report['primary']['last_usable']}; OdinSd boot log at {report['total'] - LOG_RESERVED}"]
    for entry in report["entries"]:
        size = (entry["last"] - entry["first"] + 1) * SECTOR
        lines.append(f"  p{entry['index'] + 1}: {entry['name']!r:12} {entry['first']:>10}..{entry['last']:<10} {size / 2**20:10.1f} MiB  "
                     f"type {entry['type']} guid {entry['guid']}")
    for first, last in free_ranges(report):
        lines.append(f"  free: {first}..{last} ({(last - first + 1) * SECTOR / 2**30:.2f} GiB)")
    return "\n".join(lines)


def check_card(report, strict):
    """Refuse anything that is not the card this port was built for."""
    names = [e["name"] for e in report["entries"]]
    if any(e["name"] == PART_NAME for e in report["entries"]):
        stop(f"a partition named {PART_NAME} already exists; nothing to create")
    if strict:
        if report["total"] != EXPECTED["sectors"]:
            stop(f"capacity {report['total']} is not the card's {EXPECTED['sectors']} sectors (use --any-card only for tests)")
        if names != EXPECTED["names"]:
            stop(f"partitions {names} are not {EXPECTED['names']}")
        by_name = {e["name"]: (e["first"], e["last"] - e["first"] + 1) for e in report["entries"]}
        if by_name["OS"] != EXPECTED["os"] or by_name["SystemOS"] != EXPECTED["systemos"]:
            stop("OS/SystemOS do not sit where the signed policies expect them")
    if len(names) >= ENTRY_COUNT:
        stop("no free GPT entry")


def layout(report, size_bytes, align=ALIGN):
    gaps = free_ranges(report)
    if not gaps:
        stop("no free space")
    # First fit after the last partition keeps p4 adjacent to p3 and leaves one big tail for later.
    last_end = max(e["last"] for e in report["entries"])
    sectors = size_bytes // SECTOR
    if sectors * SECTOR != size_bytes or sectors % 8:
        stop("size must be a multiple of 4 KiB")
    first = -(-(last_end + 1) // align) * align
    last = first + sectors - 1
    for gap_first, gap_last in gaps:
        if gap_first <= first and last <= gap_last:
            return first, last
    stop(f"{sectors} sectors at LBA {first} do not fit in the free ranges {gaps}")


def build_gpt(report, guid, first, last):
    """Pure function: the new primary header, array and backup header for the old tables plus UniXNUData."""
    blob = bytearray(report["blob"])
    slot = next(i for i in range(ENTRY_COUNT) if blob[i * ENTRY_BYTES:i * ENTRY_BYTES + 16] == b"\0" * 16)
    name = PART_NAME.encode("utf-16-le").ljust(72, b"\0")
    entry = BASIC_DATA.bytes_le + guid.bytes_le + struct.pack("<QQQ", first, last, 0) + name
    assert len(entry) == ENTRY_BYTES
    blob[slot * ENTRY_BYTES:(slot + 1) * ENTRY_BYTES] = entry
    blob = bytes(blob)
    total = report["total"]
    headers = {}
    for label, header, my, alt, array in (("primary", report["primary"], 1, total - 1, 2),
                                          ("backup", report["backup"], total - 1, 1, total - 33)):
        body = bytearray(struct.pack("<8sIIIIQQQQ16sQIII", b"EFI PART", 0x10000, header["size"], 0, 0, my, alt,
                                     header["first_usable"], header["last_usable"], header["disk_guid"].bytes_le,
                                     array, ENTRY_COUNT, ENTRY_BYTES, crc(blob)))
        body[16:20] = struct.pack("<I", crc(bytes(body[:header["size"]])))
        headers[label] = bytes(body).ljust(SECTOR, b"\0")
    return slot, blob, headers


def plan_ranges(report, first):
    """Every sector range the apply step writes, in write order, as (label, lba, sector_count)."""
    total = report["total"]
    return [("backup array", total - 33, ARRAY_SECTORS), ("backup header", total - 1, 1),
            ("primary array", 2, ARRAY_SECTORS), ("primary header", 1, 1),
            ("data header block", first, HEADER_ZERO_SECTORS)]


def sha(data):
    return hashlib.sha256(data).hexdigest()


def make_plan(disk, guid, size_bytes, strict):
    report = inspect(disk)
    check_card(report, strict)
    if size_bytes < OVERLAY_MIN_BYTES:
        stop(f"{size_bytes} B cannot hold the 1 GiB Data overlay ({OVERLAY_MIN_BYTES} B)")
    first, last = layout(report, size_bytes)
    slot, blob, headers = build_gpt(report, guid, first, last)
    images = {"backup array": blob, "backup header": headers["backup"], "primary array": blob,
              "primary header": headers["primary"], "data header block": b"\0" * (HEADER_ZERO_SECTORS * SECTOR)}
    ranges = plan_ranges(report, first)
    before = {}
    for label, lba, count in ranges:
        before[label] = disk.read(lba, count)
    plan = {
        "version": 1,
        "device_size_sectors": report["total"],
        "disk_guid": str(report["primary"]["disk_guid"]),
        "partition": {"name": PART_NAME, "slot": slot, "type": str(BASIC_DATA), "guid": str(guid),
                      "first_lba": first, "last_lba": last, "sectors": last - first + 1, "bytes": (last - first + 1) * SECTOR,
                      "alignment_sectors": ALIGN},
        "log_first_lba": report["total"] - LOG_RESERVED,
        "lba0_sha256": sha(report["lba0"]),
        "untouched_entries_sha256": sha(b"".join(e["raw"] for e in report["entries"])),
        "writes": [{"label": label, "lba": lba, "sectors": count, "before_sha256": sha(before[label]),
                    "after_sha256": sha(images[label])} for label, lba, count in ranges],
        "boot_args": f"sheng-data-persist=1 odin-data-guid={guid}",
        "boot_args_write": f"sheng-data-persist=2 odin-data-guid={guid}",
    }
    plan["id"] = sha(json.dumps({k: v for k, v in plan.items() if k != "id"}, sort_keys=True).encode())[:16]
    return plan, before, images, report


RESTORE = """#!/bin/bash
# Restores the sectors this plan changed. Reads the saved copies next to this file; verifies them first.
# usage: sudo bash restore.sh /dev/sdX      (only the card whose disk GUID is {disk_guid})
set -euo pipefail
dev=${{1:?device}}
cd "$(dirname "$0")"
sha256sum -c SHA256SUMS
[ "$(blockdev --getss "$dev")" = 512 ] && [ "$(blockdev --getsz "$dev")" = {total} ] || {{ echo "not the planned card" >&2; exit 1; }}
{commands}
sync
echo "restored; verify with: python3 scripts/h1/sd_data_partition.py inspect $dev"
"""


def write_backup(plan, before, out):
    out.mkdir(parents=True, exist_ok=True)
    sums, commands = [], []
    for item in plan["writes"]:
        name = item["label"].replace(" ", "-") + ".bin"
        (out / name).write_bytes(before[item["label"]])
        sums.append(f"{item['before_sha256']}  {name}")
        commands.append(f"dd if={name} of=\"$dev\" bs=512 seek={item['lba']} count={item['sectors']} conv=notrunc,fsync status=none")
    (out / "SHA256SUMS").write_text("\n".join(sums) + "\n")
    (out / "restore.sh").write_text(RESTORE.format(disk_guid=plan["disk_guid"], total=plan["device_size_sectors"],
                                                   commands="\n".join(commands)))
    (out / "plan.json").write_text(json.dumps(plan, indent=2, sort_keys=True) + "\n")


def mounted(path):
    real = os.path.realpath(path)
    name = os.path.basename(real)
    try:
        for line in Path("/proc/mounts").read_text().splitlines():
            device = line.split()[0]
            if device.startswith("/dev/") and os.path.basename(os.path.realpath(device)).startswith(name):
                return device
    except OSError:
        pass
    return None


def apply(disk, plan, confirm, strict):
    expected_id = plan["id"]
    if confirm != expected_id:
        stop(f"--confirm must be the plan id {expected_id}")
    if disk.is_block and not strict:
        stop("--any-card is for image files only")
    if disk.is_block and mounted(disk.path):
        stop(f"{mounted(disk.path)} is mounted; unmount every partition of the card first")
    again, before, images, report = make_plan(disk, uuid.UUID(plan["partition"]["guid"]), plan["partition"]["bytes"], strict)
    if again["id"] != expected_id:
        stop("the card no longer matches the plan (GPT changed or other card); make a new plan")
    # The plan is the commitment: the sectors we are about to write must be exactly those it hashed.
    for item in plan["writes"]:
        if sha(images[item["label"]]) != item["after_sha256"] or sha(before[item["label"]]) != item["before_sha256"]:
            stop(f"{item['label']} differs from the plan")
    first = plan["partition"]["first_lba"]
    for item in plan["writes"]:
        lba, count = item["lba"], item["sectors"]
        inside_protected = any(not (lba + count - 1 < e["first"] or lba > e["last"]) for e in report["entries"])
        if inside_protected:
            stop(f"{item['label']} would overlap an existing partition")   # defence in depth; layout() already excludes it
    for item in plan["writes"]:
        disk.write(item["lba"], images[item["label"]])
        disk.sync()
    # Verification: re-read everything through the parser, not through our own buffers.
    after = inspect(disk)
    names = [e["name"] for e in after["entries"]]
    if names != EXPECTED["names"] + [PART_NAME] and strict:
        stop(f"after the write the partitions are {names}")
    old_raw = b"".join(e["raw"] for e in report["entries"])
    kept = b"".join(e["raw"] for e in after["entries"] if e["name"] != PART_NAME)
    if kept != old_raw:
        stop("existing partition entries changed")
    if disk.read(0) != report["lba0"]:
        stop("protective MBR changed")
    mine = [e for e in after["entries"] if e["name"] == PART_NAME]
    part = plan["partition"]
    if len(mine) != 1 or (mine[0]["first"], mine[0]["last"], str(mine[0]["guid"])) != (part["first_lba"], part["last_lba"], part["guid"]):
        stop("new entry does not read back as planned")
    if disk.read(first, HEADER_ZERO_SECTORS) != b"\0" * (HEADER_ZERO_SECTORS * SECTOR):
        stop("Data header block did not read back as zero")
    return after


def main(argv):
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = parser.add_subparsers(dest="command", required=True)
    for name in ("inspect", "plan", "apply"):
        p = sub.add_parser(name)
        p.add_argument("target")
        p.add_argument("--any-card", action="store_true", help="skip the card-identity checks (image files in tests only)")
        if name == "plan":
            p.add_argument("--guid", default=None, help="unique GUID of the new partition (default: random)")
            p.add_argument("--size-gib", type=float, default=8.0)
            p.add_argument("--out", required=True)
        if name == "apply":
            p.add_argument("--plan", required=True)
            p.add_argument("--confirm", required=True)
    args = parser.parse_args(argv)
    strict = not args.any_card
    if args.command == "inspect":
        disk = Disk(args.target)
        print(describe(inspect(disk)))
        return 0
    if args.command == "plan":
        disk = Disk(args.target)
        guid = uuid.UUID(args.guid) if args.guid else uuid.uuid4()
        plan, before, images, report = make_plan(disk, guid, int(args.size_gib * 2**30) // 4096 * 4096, strict)
        print(describe(report))
        out = Path(args.out)
        if out.exists() and any(out.iterdir()):
            stop(f"{out} already exists and is not empty")
        write_backup(plan, before, out)
        part = plan["partition"]
        print(f"\nplan {plan['id']}: {PART_NAME} LBA {part['first_lba']}..{part['last_lba']} "
              f"({part['bytes'] / 2**30:.2f} GiB, 1 MiB aligned, GUID {part['guid']})")
        for item in plan["writes"]:
            print(f"  write {item['label']:18} LBA {item['lba']:>10} +{item['sectors']}")
        print(f"backup of those sectors: {out}  (restore.sh included, not run)")
        print(f"nothing was written to {args.target}.")
        print(f"boot args, inspect only : {plan['boot_args']}")
        print(f"boot args, write enabled: {plan['boot_args_write']}")
        print(f"to create it (after the user authorises): python3 scripts/h1/sd_data_partition.py apply {args.target} "
              f"--plan {out / 'plan.json'} --confirm {plan['id']}")
        return 0
    plan = json.loads(Path(args.plan).read_text())
    disk = Disk(args.target, write=True)
    try:
        after = apply(disk, plan, args.confirm, strict)
    finally:
        disk.close()
    print(describe(after))
    print(f"\nOK: {PART_NAME} created and read back; p1-p3 entries and LBA 0 unchanged.")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
