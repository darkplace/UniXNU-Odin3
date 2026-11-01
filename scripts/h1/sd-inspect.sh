#!/bin/bash
# The Odin 3 microSD, with pkexec. On its own it picks the USB disk with 512-byte
# sectors, the UNIXNU label and the three-partition map. If the logical sector is
# 4096 it stops: that would be the internal UFS.
#
#   bash scripts/h1/sd-inspect.sh
#   bash scripts/h1/sd-inspect.sh write <odin3-boot.img> <sha256>
#
# write formats p1 only and copies that ESP (update-esp.sh). It does not touch p2
# or p3 and does not use write-medium.sh. The SHA is checked before formatting.
set -euo pipefail

MODE=read
ESP_IMG=
EXPECT_SHA=
if [ "${1:-}" = "write" ]; then
  MODE=write
  ESP_IMG=${2:-}
  EXPECT_SHA=${3:-}
  [ -n "$ESP_IMG" ] && [ -n "$EXPECT_SHA" ] || {
    echo "usage: $0 write <odin3-boot.img> <sha256>" >&2
    exit 1
  }
elif [ $# -ne 0 ]; then
  echo "usage: $0 [write <odin3-boot.img> <sha256>]" >&2
  exit 1
fi

if [ "$MODE" = "write" ]; then
  [ -f "$ESP_IMG" ] || { echo "refused: $ESP_IMG is missing" >&2; exit 1; }
  [[ "$EXPECT_SHA" =~ ^[0-9a-f]{64}$ ]] || { echo "refused: sha256 is not 64 hexadecimal digits" >&2; exit 1; }
  actual=$(sha256sum "$ESP_IMG" | awk '{print $1}')
  [ "$actual" = "$EXPECT_SHA" ] || { echo "refused: SHA of $ESP_IMG is $actual" >&2; exit 1; }
fi

if [ "$(id -u)" -ne 0 ]; then
  exec pkexec "$0" "$@"
fi

export MTOOLS_SKIP_CHECK=1
HERE=$(cd "$(dirname "$0")" && pwd)

reject() { echo "refused: $*" >&2; exit 1; }

logical_size() {
  local name=${1#/dev/}
  cat "/sys/block/${name}/queue/logical_block_size"
}

layout_ok() {
  local target=$1
  local -a rows
  mapfile -t rows < <(sfdisk -d "$target" | grep -E '^/dev/')
  [ "${#rows[@]}" -eq 3 ] || [ "${#rows[@]}" -eq 4 ] || return 1
  python3 - "${rows[@]}" <<'PY'
import sys
base = [('2048', '821247', '819200'), ('821248', '12691522', '11870275'), ('12693504', '19144703', '6451200')]
data = ('19144704', '35921919', '16777216')
expected = base + [data] if len(sys.argv) == 5 else base
if len(sys.argv) - 1 != len(expected):
    raise SystemExit(1)
for row, want in zip(sys.argv[1:], expected):
    body = row.split(':', 1)[1]
    fields = {k.strip(): v.strip() for k, v in (part.split('=', 1) for part in body.split(','))}
    size = int(fields['size'])
    got = (fields['start'], str(int(fields['start']) + size - 1), str(size))
    if got != want:
        raise SystemExit(1)
PY
}

choose_disk() {
  local name tran dev ss label
  local -a found=()
  while read -r name tran; do
    [ "$tran" = "usb" ] || continue
    dev="/dev/$name"
    [ -b "$dev" ] || continue
    ss=$(logical_size "$dev")
    if [ "$ss" != "512" ]; then
      echo "skipped $dev: logical sector $ss (this operation requires 512)" >&2
      continue
    fi
    label=$(lsblk -no LABEL "${dev}1" 2>/dev/null | head -1 || true)
    [ "$label" = "UNIXNU" ] || continue
    if ! layout_ok "$dev"; then
      echo "skipped $dev: UNIXNU label but the three-partition map does not match" >&2
      continue
    fi
    found+=("$dev")
  done < <(lsblk -dn -o NAME,TRAN)
  [ "${#found[@]}" -eq 1 ] || reject "expected one UNIXNU microSD and found ${#found[@]}: ${found[*]:-none}"
  printf '%s\n' "${found[0]}"
}

TARGET=$(choose_disk)
parts=("${TARGET}1" "${TARGET}2" "${TARGET}3")
[ -b "${TARGET}4" ] && parts+=("${TARGET}4")
for part in "${parts[@]}"; do
  [ -b "$part" ] || reject "$part is missing"
  if findmnt -S "$part" >/dev/null 2>&1; then
    reject "$part is mounted"
  fi
done

if [ "$MODE" = "write" ]; then
  REPO=$(cd "$(dirname "$0")/../.." && pwd)
  SPLASH_DTB=${SPLASH_DTB:-$REPO/out/odin3-splash.dtb}
  [ -f "$SPLASH_DTB" ] || reject "$SPLASH_DTB is missing (run make-boot-medium.sh first)"
  echo "== writing ${TARGET}1 only, from $ESP_IMG"
  ESP_IMG=$ESP_IMG SPLASH_DTB=$SPLASH_DTB bash "$HERE/update-esp.sh" "$TARGET"
  exit 0
fi

echo "== disk $TARGET  usb transport  512 sectors  three-partition map"
lsblk -o NAME,SIZE,FSTYPE,LABEL,TRAN "$TARGET"
echo

echo "== XNSDLOG1 record"
bash "$HERE/read-sd-log.sh" "$TARGET"
echo

echo "== ESP p1"
mdir -i "${TARGET}1" ::/
echo
mdir -i "${TARGET}1" ::/EFI/UniXNU
echo
mdir -i "${TARGET}1" ::/EFI/UniXNU/Payloads
echo

echo "== unixnu.toml"
mtype -i "${TARGET}1" ::/EFI/UniXNU/unixnu.toml
echo

echo "== hashes read back from p1"
python3 - "$TARGET" <<'PY'
import hashlib, subprocess, sys
target = sys.argv[1] + "1"
paths = [
    "EFI/UniXNU/unixnu.toml",
    "EFI/UniXNU/Payloads/kernel.macho",
    "EFI/UniXNU/Payloads/kernel.macho.adt",
    "EFI/UniXNU/Payloads/kernel.macho.trustcache",
    "ODIN3.DTB",
    "DTBS/QCOM/ODIN3.DTB",
    "UNIXNU.EFI",
    "EFI/UniXNU/Payloads/DarwinBoot.efi",
]
for path in paths:
    data = subprocess.check_output(["mtype", "-i", target, "::/" + path])
    print(f"{hashlib.sha256(data).hexdigest()}  {len(data):10d}  {path}")
PY
