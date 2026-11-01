#!/bin/bash
# Read the boot log SdhciMsm leaves on the card (the 32 KiB block just before the backup GPT).
#   pkexec bash scripts/h1/read-sd-log.sh /dev/sda
#
# The driver writes "XNSDLOG1" + a sequence number + the text at LBA capacity-8192, so this never touches
# the partition table or any partition. Reading only.
set -euo pipefail

TARGET=${1:-/dev/sda}
[ -b "$TARGET" ] || { echo "$TARGET no es un dispositivo de bloque" >&2; exit 1; }
capacity=$(blockdev --getsz "$TARGET")
skip=$((capacity - 8192))
[ "$skip" -gt 0 ] || { echo "$TARGET is too small" >&2; exit 1; }

echo "== $TARGET: $capacity sectores; log en $skip (+64)"
dd if="$TARGET" bs=512 skip="$skip" count=64 status=none | python3 -c '
import sys
data = sys.stdin.buffer.read()
if data[:8] != b"XNSDLOG1":
    print("no driver log (magic absent); the kernel never wrote it, or never booted this medium")
    sys.exit(0)
sequence = int.from_bytes(data[8:12], "little")
length = int.from_bytes(data[12:16], "little")
cards = int.from_bytes(data[16:20], "little")
print(f"secuencia {sequence}  tarjeta {cards} sectores  {length} bytes de texto")
print("-" * 72)
sys.stdout.write(data[32:length].decode("utf-8", "replace"))
'
