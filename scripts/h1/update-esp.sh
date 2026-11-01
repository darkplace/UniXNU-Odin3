#!/bin/bash
# Refresh only the ESP (partition 1) of the Odin 3 card, leaving the OS and SystemOS partitions untouched.
# Run as root:
#   pkexec bash scripts/h1/update-esp.sh /dev/sda
#
# The source is the ESP image the boot-medium script just built (odin3-boot.img: payload volume plus
# \UNIXNU.EFI, the BLS entry and the board's DarwinBoot.efi). Its SHAs are in the MANIFEST.sha256 next to it. The script refuses any target that is not USB, any partition
# layout that is not the 400 MiB ESP followed by the two system partitions, and any mounted partition.
set -euo pipefail

TARGET=${1:-/dev/sda}
REPO=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
ESP_IMG=${ESP_IMG:-$REPO/out/odin3-boot.img}
SPLASH_DTB=${SPLASH_DTB:-$REPO/out/odin3-splash.dtb}
EXPECTED_ESP_SECTORS=819200

[ -b "$TARGET" ] || { echo "$TARGET no es un dispositivo de bloque" >&2; exit 1; }
[ "$(lsblk -dno TRAN "$TARGET")" = "usb" ] || { echo "refused: $TARGET is not USB" >&2; exit 1; }
[ -f "$ESP_IMG" ] || { echo "$ESP_IMG is missing" >&2; exit 1; }
for part in "$TARGET"1 "$TARGET"2 "$TARGET"3; do
    [ -b "$part" ] || { echo "partition $part is missing" >&2; exit 1; }
    if findmnt -S "$part" >/dev/null 2>&1; then echo "refused: $part is mounted" >&2; exit 1; fi
done
if [ -b "$TARGET"4 ] && findmnt -S "$TARGET"4 >/dev/null 2>&1; then
    echo "refused: ${TARGET}4 is mounted" >&2; exit 1
fi

# p1-p3 as write-system-card.sh left them. p4 UniXNUData, if present, is only checked and never formatted.
mapfile -t rows < <(sfdisk -d "$TARGET" | grep -E '^/dev/')
[ "${#rows[@]}" -eq 3 ] || [ "${#rows[@]}" -eq 4 ] || { echo "refused: expected 3 or 4 partitions, found ${#rows[@]}" >&2; exit 1; }
python3 - "${rows[@]}" <<'PY'
import sys
base = [('2048', '821247', '819200'), ('821248', '12691522', '11870275'), ('12693504', '19144703', '6451200')]
data = ('19144704', '35921919', '16777216')
expected = base + [data] if len(sys.argv) == 5 else base
if len(sys.argv) - 1 != len(expected):
    raise SystemExit(f'se esperaban {len(expected)} particiones')
for row, want in zip(sys.argv[1:], expected):
    body = row.split(':', 1)[1]
    fields = {k.strip(): v.strip() for k, v in (part.split('=', 1) for part in body.split(','))}
    size = int(fields['size'])
    got = (fields['start'], str(int(fields['start']) + size - 1), str(size))
    if got != want:
        raise SystemExit(f'unexpected layout: {row.strip()} (expected {want})')
print('layout ok')
PY

export MTOOLS_SKIP_CHECK=1
echo "== formateando $TARGET 1 (400 MiB, FAT32, UNIXNU)"
mkfs.vfat -F 32 -n UNIXNU "$TARGET"1 >/dev/null
mmd -i "$TARGET"1 ::/EFI ::/EFI/UniXNU ::/EFI/UniXNU/Payloads \
    ::/loader ::/loader/entries ::/DTBS ::/DTBS/QCOM

echo "== copiando desde $ESP_IMG"
for path in UNIXNU.EFI HELLO.EFI EFI/UniXNU/unixnu.toml EFI/UniXNU/Payloads/kernel.macho \
            EFI/UniXNU/Payloads/kernel.macho.trustcache EFI/UniXNU/Payloads/kernel.macho.adt \
            EFI/UniXNU/Payloads/scratch-data.img EFI/UniXNU/Payloads/DarwinBoot.efi; do
    mcopy -i "$ESP_IMG" "::/$path" - | mcopy -i "$TARGET"1 - "::/$path"
done
[ -f "$SPLASH_DTB" ] || { echo "$SPLASH_DTB is missing (scripts/h1/make-splash-dtb.sh)" >&2; exit 1; }
# 8.3 y mayusculas: Core/Fat32.h:9 documenta que el lector de reserva solo
# resuelve nombres 8.3, y la entrada BLS es una cadena literal.
mcopy -o -i "$TARGET"1 "$SPLASH_DTB" ::/ODIN3.DTB
mcopy -o -i "$TARGET"1 "$SPLASH_DTB" ::/DTBS/QCOM/ODIN3.DTB
# Entrada BLS con devicetree: es la que hace que OpenABL conserve la pantalla
# encendida en ExitBootServices (Efi/DisplayHandover.c KeepAtExit). Sin
# \EFI\BOOT\BOOTAA64.EFI el fichero por defecto no existe, asi que el unico
# camino de arranque es este.
tmp_entry=$(mktemp)
cat > "$tmp_entry" <<'EOF'
title UniXNU Odin 3
linux \UNIXNU.EFI
devicetree \ODIN3.DTB
EOF
mcopy -o -i "$TARGET"1 "$tmp_entry" ::/loader/entries/unixnu.conf
# Sonda: HelloPayload informa 15 s y retorna; OpenABL entonces vuelve al menu y
# su pantalla Log conserva las lineas del intento (devicetree/splash).
cat > "$tmp_entry" <<'EOF'
title DT probe
linux \HELLO.EFI
devicetree \ODIN3.DTB
EOF
mcopy -o -i "$TARGET"1 "$tmp_entry" ::/loader/entries/dtprobe.conf
rm -f "$tmp_entry"
sync

echo "== verification (read back from the card)"
mdir -i "$TARGET"1 ::/ | tail -5
mdir -i "$TARGET"1 ::/EFI/UniXNU/Payloads | tail -8
mdir -i "$TARGET"1 ::/loader/entries | tail -3
if mdir -i "$TARGET"1 ::/EFI/BOOT/BOOTAA64.EFI >/dev/null 2>&1; then
    echo "ERROR: sigue existiendo el fichero por defecto" >&2; exit 1
fi
tmp=$(mktemp)
for path in UNIXNU.EFI EFI/UniXNU/unixnu.toml EFI/UniXNU/Payloads/kernel.macho EFI/UniXNU/Payloads/kernel.macho.adt \
            EFI/UniXNU/Payloads/kernel.macho.trustcache EFI/UniXNU/Payloads/scratch-data.img \
            EFI/UniXNU/Payloads/DarwinBoot.efi ODIN3.DTB DTBS/QCOM/ODIN3.DTB loader/entries/unixnu.conf; do
    mcopy -i "$TARGET"1 "::/$path" "$tmp"
    printf '%-45s %s %s B\n' "$path" "$(sha256sum "$tmp" | cut -d' ' -f1)" "$(stat -c %s "$tmp")"
done
rm -f "$tmp"
# El nodo que decide que el panel siga encendido, leido de la tarjeta.
mcopy -i "$TARGET"1 ::/ODIN3.DTB - | dtc -I dtb -O dts - 2>/dev/null |
    sed -n '/splash_region {/,/};/p'
sfdisk -l "$TARGET" | tail -4
