#!/bin/bash
# Build build/qemu-board-stubs/libQemuBoardStubs.a from qemu-board-stubs.c, for the kernel link.
# Run inside scripts/q2/with-toolchain-ns.sh:
#   bash scripts/q2/with-toolchain-ns.sh bash scripts/q2/build-qemu-board-stubs.sh
set -euo pipefail
TREE=${TREE:-${PROJECT:-$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)}/firmware/UniXNU/out/qemu/tree}
SOURCE=$(dirname "$0")/qemu-board-stubs.c
OUT="$TREE/build/qemu-board-stubs"
mkdir -p "$OUT"
xcrun clang -target arm64-apple-ios17.0 -std=c11 -O2 -ffreestanding -c "$SOURCE" -o "$OUT/QemuBoardStubs.o"
xcrun ar rcs "$OUT/libQemuBoardStubs.a" "$OUT/QemuBoardStubs.o"
echo "== $(stat -c %s "$OUT/libQemuBoardStubs.a") B $OUT/libQemuBoardStubs.a"
xcrun nm -g "$OUT/libQemuBoardStubs.a" | grep " [TDBV] "
