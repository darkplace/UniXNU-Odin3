#!/bin/bash
# Build the OpenABL boot medium for the Odin 3: a GPT disk image whose single EFI System Partition
# holds UniXNU as the loader. The script writes files only; it never opens a block device. The card
# is written afterwards by scripts/h1/sd-inspect.sh, after checking the target.
#
#   UNIXNU_TREE=<UniXNU checkout at upstream.rev> bash scripts/h1/make-boot-medium.sh
#
# Inputs are read from the build tree (BOARD_OUT). Override any of them with an environment
# variable; every one is listed below.
set -euo pipefail

REPO=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
UNIXNU_TREE=${UNIXNU_TREE:-$(cd "$REPO/../UniXNU" 2>/dev/null && pwd || echo "$REPO/../UniXNU")}
OUT=${OUT:-$REPO/out}
BOARD_OUT=${BOARD_OUT:-$UNIXNU_TREE/out/odin3/ipados-17.7.11-21H461/odin3}
PAYLOAD=${PAYLOAD:-$BOARD_OUT/payload/payload.img}
UNIXNU=${UNIXNU:-$BOARD_OUT/package/Build/Out/AARCH64-DEBUG/UniXNU.efi}
DARWINBOOT=${DARWINBOOT:-$BOARD_OUT/tree/build/darwinboot-efi/DarwinBoot.efi}
SIZE=${SIZE:-512M}
PART_START=${PART_START:-2048}
PART_SIZE=${PART_SIZE:-400M}
HARDWARE_DTB=${HARDWARE_DTB:-$BOARD_OUT/device-tree/hardware.dtb}
SPLASH_DTB=${SPLASH_DTB:-$OUT/odin3-splash.dtb}
ENTRY=${ENTRY:-$OUT/unixnu.conf}
HELLO=${HELLO:-$BOARD_OUT/package/Build/Out/AARCH64-DEBUG/HelloPayload.efi}
PROBE=${PROBE:-$OUT/dtprobe.conf}

export MTOOLS_SKIP_CHECK=1
mkdir -p "$OUT"
for path in "$PAYLOAD" "$UNIXNU" "$DARWINBOOT" "$HARDWARE_DTB"; do
    [ -f "$path" ] || { echo "missing $path (set BOARD_OUT to the build output)" >&2; exit 1; }
done

# 0. The tree OpenABL receives as the entry's devicetree: hardware.dtb plus
#    /reserved-memory/splash_region. Without that node OpenABL turns the panel off at
#    ExitBootServices and nothing UniXNU draws is visible.
"$(dirname "$0")/make-splash-dtb.sh" "$HARDWARE_DTB" "$SPLASH_DTB"

# BLS entry: OpenABL (Boot/Booter.c StartEntry) installs this devicetree before
# StartImage and keeps the display alive; the default file (Efi/DefaultFiles.c)
# carries no tree, and BootDrives falls back to it when no entry boots.
# So that the only reachable path is the one that passes a tree:
#   - UniXNU lives in \UNIXNU.EFI, not in \EFI\BOOT\BOOTAA64.EFI.
#   - the tree is named ODIN3.DTB (8.3): Core/Fat32.h:9 documents that the fallback
#     reader resolves 8.3 only, and a long name in the entry depends on the XBL
#     driver exposing long names.
cat > "$ENTRY" <<'EOF'
title UniXNU Odin 3
linux \UNIXNU.EFI
devicetree \ODIN3.DTB
EOF

# Probe: HelloPayload reports for 15 s on screen (including whether EFI_DTB_TABLE
# exists) and returns, so OpenABL goes back to its menu and leaves its log. It shows
# whether the entry carried the tree and what SplashHandOver decided.
cat > "$PROBE" <<'EOF'
title DT probe
linux \HELLO.EFI
devicetree \ODIN3.DTB
EOF

# 1. ESP: the payload volume plus the loader and DarwinBoot, with no default file.
esp="$OUT/odin3-boot.img"
cp -f "$PAYLOAD" "$esp"
mmd -i "$esp" ::/loader ::/loader/entries ::/DTBS ::/DTBS/QCOM
mcopy -o -i "$esp" "$UNIXNU" ::/UNIXNU.EFI
mcopy -o -i "$esp" "$DARWINBOOT" ::/EFI/UniXNU/Payloads/DarwinBoot.efi
mcopy -o -i "$esp" "$SPLASH_DTB" ::/ODIN3.DTB
mcopy -o -i "$esp" "$SPLASH_DTB" ::/DTBS/QCOM/ODIN3.DTB
mcopy -o -i "$esp" "$ENTRY" ::/loader/entries/unixnu.conf
mcopy -o -i "$esp" "$HELLO" ::/HELLO.EFI
mcopy -o -i "$esp" "$PROBE" ::/loader/entries/dtprobe.conf
# The default file must not exist: if it does, OpenABL can boot it without
# installing a devicetree and the panel goes off at ExitBootServices.
if mdir -i "$esp" ::/EFI/BOOT/BOOTAA64.EFI >/dev/null 2>&1; then
    mdel -i "$esp" ::/EFI/BOOT/BOOTAA64.EFI
fi

# 2. Disk image: GPT with one EFI System Partition (type EF00) holding FAT32. Qualcomm's PartitionDxe in
#    the ABL is a GPT implementation, so an MBR-only medium is not guaranteed to be enumerated at all.
disk="$OUT/odin3-sd.img"
rm -f "$disk"
truncate -s "$SIZE" "$disk"
printf 'label: gpt\nstart=%s, size=+%s, type=U, name="UNIXNU"\n' "$PART_START" "$PART_SIZE" | sfdisk -q "$disk"
# --offset sets only the start. Without an explicit filesystem block count,
# mkfs fills the remaining DISK (511 MiB), not the GPT partition (400 MiB).
# BLOCK-COUNT is in 1024-byte units; sfdisk size is in 512-byte sectors here.
partition_blocks=$(sfdisk --json "$disk" | python3 -c '
import json, sys
t = json.load(sys.stdin)["partitiontable"]
p = t["partitions"][0]
assert t["label"] == "gpt" and t["sectorsize"] == 512
assert len(t["partitions"]) == 1 and p["start"] == int(sys.argv[1])
assert p["size"] > 0 and p["size"] % 2 == 0
print(p["size"] // 2)
' "$PART_START")
mkfs.vfat -F 32 -S 512 -h "$PART_START" -n UNIXNU --offset "$PART_START" "$disk" "$partition_blocks" >/dev/null
at="$disk@@$((PART_START * 512))"
mmd -i "$at" ::/EFI ::/EFI/UniXNU ::/EFI/UniXNU/Payloads ::/loader ::/loader/entries ::/DTBS ::/DTBS/QCOM
for path in UNIXNU.EFI HELLO.EFI EFI/UniXNU/unixnu.toml EFI/UniXNU/Payloads/kernel.macho \
            EFI/UniXNU/Payloads/kernel.macho.trustcache EFI/UniXNU/Payloads/kernel.macho.adt \
            EFI/UniXNU/Payloads/scratch-data.img EFI/UniXNU/Payloads/DarwinBoot.efi \
            loader/entries/unixnu.conf loader/entries/dtprobe.conf ODIN3.DTB DTBS/QCOM/ODIN3.DTB; do
    mcopy -i "$esp" "::/$path" - | mcopy -i "$at" - "::/$path"
done

# Manifest of what the ESP holds, read back from the image (not from the inputs).
manifest="$OUT/MANIFEST.sha256"
tmp=$(mktemp -d "$OUT/manifest.XXXXXX")
: > "$manifest"
for path in UNIXNU.EFI HELLO.EFI ODIN3.DTB EFI/UniXNU/unixnu.toml EFI/UniXNU/Payloads/kernel.macho \
            EFI/UniXNU/Payloads/kernel.macho.adt EFI/UniXNU/Payloads/kernel.macho.trustcache \
            EFI/UniXNU/Payloads/scratch-data.img EFI/UniXNU/Payloads/DarwinBoot.efi loader/entries/unixnu.conf; do
    mcopy -i "$at" "::/$path" "$tmp/f"
    printf '%s  %s  %s B\n' "$(sha256sum "$tmp/f" | cut -d' ' -f1)" "$path" "$(stat -c %s "$tmp/f")" >> "$manifest"
done
rm -rf "$tmp"
cat "$manifest"
echo
mdir -i "$at" ::/ | tail -c 400
mdir -i "$at" ::/loader/entries | tail -c 300
mdir -i "$at" ::/EFI/UniXNU/Payloads | tail -c 600
echo
sha256sum "$esp" "$disk"

# Seal: record every input that produced these images next to them (SEAL.json). Best effort: a
# missing docker or git leaves a warning, never a half-written medium.
python3 "$(dirname "$0")/seal-medium.py" --out "$OUT" --repo "$REPO" --tree "$UNIXNU_TREE" \
    ${UNIXNU_IPSW:+--ipsw "$UNIXNU_IPSW"} \
    || echo "warning: the medium is built but not sealed" >&2
