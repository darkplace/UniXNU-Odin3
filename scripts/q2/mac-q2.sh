#!/bin/bash
# Q2 on macOS: the OS and SystemOS images, the Data seed, the policies and the qemu_root kernel.
# It does NOT run the final userland QEMU (a 6 GiB guest on 8 GB of RAM): that runs on Linux.
# Usage: PROJECT=<this repository> IPSW=/path/to/ipad.ipsw bash scripts/q2/mac-q2.sh
#
# The tree layout is the build tree of a development host: firmware/UniXNU, firmware/UniXNU-qemu
# and firmware/ipsw next to each other. Point PROJECT at the directory that holds them.
set -euo pipefail

PROJECT=${PROJECT:-$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)}
FW="$PROJECT/firmware"
UNIXNU="$FW/UniXNU"
DEV="$FW/UniXNU-qemu"
IPSW=${IPSW:-"$FW/ipsw/iPad_Pro_HFR_17.7.11_21H461_Restore.ipsw"}
SEED=${SEED:-99ddeb874ccd7dca}
FONT=${FONT:-"$PROJECT/scripts/q2/Font.h"}
export UNIXNU_IPSW="$IPSW"
export JOBS=${JOBS:-4}
export PATH="/opt/homebrew/bin:/opt/homebrew/opt/openssl@3/bin:$PATH"

say() { printf '\n== %s\n' "$*"; }
need() { command -v "$1" >/dev/null 2>&1 || { echo "$1 is missing"; exit 1; }; }

say "macOS checks"
for t in git python3 qemu-system-aarch64 hdiutil diskutil codesign dtc mformat mcopy; do need "$t"; done
xcrun --sdk iphoneos --show-sdk-path >/dev/null
test -f "$IPSW" || { echo "IPSW is missing: $IPSW"; exit 1; }
test -f "$FONT" || { echo "Font.h is missing: $FONT"; exit 1; }
mkdir -p "$FW"

say "clones (if missing)"
if [ ! -d "$UNIXNU/.git" ]; then
  git clone --recurse-submodules https://github.com/UniXNU/UniXNU.git "$UNIXNU"
  git -C "$UNIXNU" checkout 40d21cc65171c3a24faa130a2a8e899f85790485
fi
if [ ! -d "$DEV/.git" ]; then
  git clone https://github.com/xVanTuring/UniXNU-qemu.git "$DEV"
  git -C "$DEV" checkout 97b75b1
fi

say "qemu_defconfig: kernel, package and payload"
cd "$UNIXNU"
./unixnu defconfig qemu_defconfig -D ../UniXNU-qemu
./unixnu ipsw fetch -D ../UniXNU-qemu --ipsw "$IPSW"
./unixnu fetch -D ../UniXNU-qemu
./unixnu build package kernel -D ../UniXNU-qemu

say "panic screen Font.h into the tree"
mkdir -p out/qemu/tree/uefi-sheng/Silicon/Silicium/SiliciumPkg/Library/FrameBufferSerialPortLib
cp "$FONT" out/qemu/tree/uefi-sheng/Silicon/Silicium/SiliciumPkg/Library/FrameBufferSerialPortLib/Font.h

./unixnu build payload -D ../UniXNU-qemu --unique-chip-id "$SEED"

say "build-os-root.py (hdiutil/diskutil/codesign; short QEMU for the ABI)"
cd out/qemu/tree
python3 scripts/build-os-root.py
cd "$UNIXNU"

say "Data seed UUID"
export XNU_OWNER_DATA_VOLUME_UUID=$(python3 -c "import json; print(json.load(open('../UniXNU-qemu/.cache/firmware/owner-data/seed.json'))['volume_uuid'])")
printf '%s\n' "$XNU_OWNER_DATA_VOLUME_UUID" > out/qemu/tree/build/owner-data-uuid.txt

say "qemu_root_defconfig: kernel with owner root/display"
./unixnu defconfig qemu_root_defconfig -D ../UniXNU-qemu
./unixnu build kernel -D ../UniXNU-qemu

say "outputs to copy to Linux"
for p in \
  out/qemu/kernel/kernel.macho \
  out/qemu/kernel/kernel.json \
  out/qemu/tree/build/original-static-system-trust/original-static-system.bundle \
  ../UniXNU-qemu/.cache/firmware/owner-data/owner-data-1g.dbr \
  ../UniXNU-qemu/.cache/firmware/owner-data/seed.json \
  out/qemu/tree/.cache/firmware/target-volumes; do
  test -e "$p" && stat -f '%N %z' "$p" || echo "MISSING $p"
done
shasum -a 256 out/qemu/kernel/kernel.macho \
  out/qemu/tree/build/original-static-system-trust/original-static-system.bundle \
  ../UniXNU-qemu/.cache/firmware/owner-data/owner-data-1g.dbr | tee out/qemu/q2-mac-sha256.txt
echo
echo "Copy to Linux: out/qemu/kernel, out/qemu/tree/build/original-static-system-trust,"
echo "out/qemu/tree/.cache/firmware/target-volumes, ../UniXNU-qemu/.cache/firmware/owner-data"
echo "and out/qemu/tree/build/owner-data-uuid.txt"
