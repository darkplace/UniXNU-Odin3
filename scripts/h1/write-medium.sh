#!/bin/bash
# Write the OpenABL boot medium to the USB flash drive, with guards. Run as root:
#   pkexec bash scripts/h1/write-medium.sh [/dev/sda]
set -euo pipefail
TARGET=${1:-/dev/sda}
REPO=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
IMAGE=${IMAGE:-$REPO/out/odin3-sd.img}

[ -b "$TARGET" ] || { echo "$TARGET is not a block device" >&2; exit 1; }
MODEL=$(lsblk -dno MODEL "$TARGET" | tr -d ' ')
SIZE=$(lsblk -dno SIZE "$TARGET")
TRAN=$(lsblk -dno TRAN "$TARGET")
echo "target: $TARGET  model='$MODEL'  size=$SIZE  transport=$TRAN"
[ "$TRAN" = "usb" ] || { echo "refused: $TARGET is not USB (transport '$TRAN')" >&2; exit 1; }
if findmnt -S "$TARGET" >/dev/null 2>&1 || findmnt -S "${TARGET}1" >/dev/null 2>&1; then
    echo "refused: something is mounted from $TARGET; unmount it first" >&2; exit 1
fi
echo "image: $IMAGE  $(sha256sum "$IMAGE" | cut -d' ' -f1)"

echo "== dd"
dd if="$IMAGE" of="$TARGET" bs=4M conv=fsync status=progress
sync
echo "== backup GPT at the end of the device"
sgdisk -e "$TARGET" >/dev/null
sgdisk -v "$TARGET" | tail -2

echo "== read back"
sfdisk -l "$TARGET" | tail -3
mdir -i "${TARGET}@@1M" ::/EFI/BOOT | tail -3
tmp=$(mktemp)
mcopy -i "${TARGET}@@1M" ::/EFI/BOOT/BOOTAA64.EFI "$tmp"
echo "BOOTAA64.EFI on the medium: $(sha256sum "$tmp" | cut -d' ' -f1)  ($(stat -c %s "$tmp") B)"
rm -f "$tmp"
