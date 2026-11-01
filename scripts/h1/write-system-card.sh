#!/bin/bash
# Write the Odin 3 system card: ESP with UniXNU + the iPadOS volumes as raw partitions, then verify every
# byte by SHA-256. Run as root:
#   pkexec bash scripts/h1/write-system-card.sh /dev/sda
#
# Layout (512-byte sectors, 2048 aligned):
#   p1  ESP        400 MiB   FAT32 "UNIXNU": \EFI\BOOT\BOOTAA64.EFI (UniXNU), unixnu.toml, Payloads\*
#   p2  OS         11 870 275 sectors  the whole OS volume image (its own GPT + APFS inside)
#   p3  SystemOS    6 451 200 sectors  the SystemOS APFS container
#
# The kernel still needs an SD/MMC block driver to read any of this; this script only prepares the medium.
set -euo pipefail

TARGET=${1:-/dev/sda}
REPO=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
OUT=${OUT:-$REPO/out}
ESP=${ESP:-$OUT/odin3-boot.img}
OS_IMG=${OS_IMG:-$OUT/target-volumes/044-76040-038.raw}
SYS_IMG=${SYS_IMG:-$OUT/target-volumes/044-76034-038.raw}
ESP_SECTORS=819200
OS_SECTORS=11870275
SYS_SECTORS=6451200
ESP_START=2048
OS_START=821248
SYS_START=12693504

[ -b "$TARGET" ] || { echo "$TARGET is not a block device" >&2; exit 1; }
[ "$(lsblk -dno TRAN "$TARGET")" = "usb" ] || { echo "refused: $TARGET is not USB" >&2; exit 1; }
for part in "$TARGET"1 "$TARGET"2 "$TARGET"3; do
    if findmnt -S "$part" >/dev/null 2>&1; then echo "refused: $part is mounted" >&2; exit 1; fi
done
for img in "$ESP" "$OS_IMG" "$SYS_IMG"; do
    [ -f "$img" ] || { echo "$img is missing (scripts/h1/make-target-volumes.sh writes the two volumes)" >&2; exit 1; }
done
need=$(( (SYS_START + SYS_SECTORS) * 512 ))
have=$(blockdev --getsize64 "$TARGET")
[ "$have" -ge "$need" ] || { echo "the card is smaller than the layout ($have < $need)" >&2; exit 1; }
echo "target $TARGET: $(lsblk -dno SIZE "$TARGET"), needs $(( need / 1024 / 1024 )) MiB"

# 1. Partition table: GPT, ESP + the two volume images as plain partitions.
printf 'label: gpt\nstart=%s, size=%s, type=U, name="UNIXNU"\nstart=%s, size=%s, type=linux, name="OS"\nstart=%s, size=%s, type=linux, name="SystemOS"\n' \
  "$ESP_START" "$ESP_SECTORS" "$OS_START" "$OS_SECTORS" "$SYS_START" "$SYS_SECTORS" | sfdisk -q --force "$TARGET"
partprobe "$TARGET" 2>/dev/null || true
sleep 1

# 2. ESP: copy the tree out of the prepared FAT image.
mkfs.vfat -F 32 -n UNIXNU "${TARGET}1" >/dev/null
export MTOOLS_SKIP_CHECK=1
mmd -i "${TARGET}1" ::/EFI ::/EFI/BOOT ::/EFI/UniXNU ::/EFI/UniXNU/Payloads
for path in EFI/BOOT/BOOTAA64.EFI EFI/UniXNU/unixnu.toml EFI/UniXNU/Payloads/kernel.macho \
            EFI/UniXNU/Payloads/kernel.macho.trustcache EFI/UniXNU/Payloads/kernel.macho.adt \
            EFI/UniXNU/Payloads/scratch-data.img EFI/UniXNU/Payloads/DarwinBoot.efi; do
    mcopy -i "$ESP" "::/$path" - | mcopy -i "${TARGET}1" - "::/$path"
done

# 3. The two volume images, raw.
echo "== OS $(basename "$OS_IMG")"
dd if="$OS_IMG" of="${TARGET}2" bs=8M conv=fsync status=progress
echo "== SystemOS $(basename "$SYS_IMG")"
dd if="$SYS_IMG" of="${TARGET}3" bs=8M conv=fsync status=progress
sync

# 4. Verify every byte.
echo "== verification"
sha256sum "$OS_IMG" "${TARGET}2"
sha256sum "$SYS_IMG" "${TARGET}3"
t=$(mktemp)
mcopy -i "${TARGET}1" ::/EFI/BOOT/BOOTAA64.EFI "$t"
echo "ESP BOOTAA64.EFI: $(sha256sum "$t" | cut -d' ' -f1)"
rm -f "$t"
sfdisk -l "$TARGET" | tail -4
