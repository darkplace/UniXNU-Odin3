#!/bin/bash
# Q2 on Linux: the part that needs neither hdiutil nor APFS.
#
# Input: the macOS outputs (scripts/q2/mac-osroot.sh), copied to the same paths:
#   out/qemu/tree/build/verified-root-host/{verified-root.dylib,keys/}
#   out/qemu/tree/build/verified-root-os/{verification.json,manifest.bin,trusted-key.bin,...}
#   out/qemu/tree/build/verified-root-systemos/...
#   UniXNU-qemu/.cache/firmware/owner-data/{seed.json,owner-data-1g.dbr}
#
# 1. scripts/build-os-root.py in the container: it skips the macOS steps (they exist) and runs the
#    compilation ones (storage ABI, verified-root probe, VirtIO/RamBlock bindings, display, trust cache).
# 2. qemu_root_defconfig, kernel, payload (TCG) and run-qemu.py --os-root.
#
# Usage: bash scripts/q2/linux-q2-finish.sh
set -uo pipefail

PROJECT=${PROJECT:-$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)}
FW="$PROJECT/firmware"
UNIXNU="$FW/UniXNU"
TREE="$UNIXNU/out/qemu/tree"
DEV="$FW/UniXNU-qemu"
SEED=${SEED:-99ddeb874ccd7dca}
IPSW=${IPSW:-$FW/ipsw/iPad_Pro_HFR_17.7.11_21H461_Restore.ipsw}
IMAGE=v1-qemu
LOG="$FW/out"
mkdir -p "$LOG"

say() { printf '\n== %s\n' "$*"; }
die() { echo "MISSING: $*" >&2; exit 1; }

docker_run() {  # docker_run <cwd> <command...>
  local cwd="$1"; shift
  docker run --rm --user "$(id -u):$(id -g)" \
    -e HOME=/tmp -e JOBS=6 -e UNIXNU_TOOLCHAIN=/opt/unixnu-linux \
    -e XNU_TOOLCHAIN_BIN="/opt/unixnu-linux/CommandLineTools/usr/bin /opt/unixnu-linux/XcodeDefault.xctoolchain/usr/bin" \
    -e UNIXNU_KERNELCACHE="$UNIXNU/out/ipsw/21H461/kernelcache.decompressed" \
    -e UNIXNU_IPSW="$IPSW" \
    -e PATH=/opt/unixnu-linux/bin:/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin \
    -v "$PROJECT:$PROJECT" -w "$cwd" "unixnu-toolchain:$IMAGE" "$@"
}

say "checks"
for p in "$TREE/build/verified-root-host/verified-root.dylib" \
         "$TREE/build/verified-root-os/verification.json" \
         "$TREE/build/verified-root-systemos/verification.json" \
         "$DEV/.cache/firmware/owner-data/seed.json" \
         "$DEV/.cache/firmware/owner-data/owner-data-1g.dbr"; do
  [ -e "$p" ] || die "$p (ejecuta antes scripts/q2/mac-osroot.sh en el Mac y copia el resultado)"
done
UUID=$(python3 -c "import json; print(json.load(open('$DEV/.cache/firmware/owner-data/seed.json'))['volume_uuid'])")
printf 'Data volume UUID: %s\n' "$UUID"

say "build-os-root.py (Linux steps only; the macOS ones already exist)"
docker_run "$TREE" python3 scripts/build-os-root.py 2>&1 | tee "$LOG/q2-osroot.log" | tail -40

say "qemu_root_defconfig + kernel"
cd "$UNIXNU"
docker_run "$UNIXNU" ./unixnu defconfig qemu_root_defconfig -D ../UniXNU-qemu
docker_run "$UNIXNU" ./unixnu build kernel -D ../UniXNU-qemu 2>&1 | tee "$LOG/q2-kernel.log" | tail -20

say "payload (TCG, with the macOS trust cache and Data)"
export XNU_OWNER_DATA_VOLUME_UUID="$UUID"
docker_run "$UNIXNU" env XNU_OWNER_DATA_VOLUME_UUID="$UUID" UNIXNU_QEMU_ACCEL=tcg ./unixnu build payload \
  -D ../UniXNU-qemu --unique-chip-id "$SEED" \
  --trust-cache "$TREE/build/original-static-system-trust/original-static-system.bundle" \
  --data-ramdisk "$DEV/.cache/firmware/owner-data/owner-data-1g.dbr" 2>&1 | tee "$LOG/q2-payload.log" | tail -20

say "QEMU --os-root (TCG)"
python3 "$TREE/scripts/run-qemu.py" --os-root --display none --screen firmware --seconds 1800 --every 30 \
  2>&1 | tee "$LOG/q2-qemu.log" | tail -40

say "done: screenshots and serial in out/qemu/run/<time>/"
