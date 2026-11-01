#!/bin/bash
# Derive from hardware.dtb the tree handed to OpenABL as the boot entry devicetree.
#
# OpenABL turns the display off at ExitBootServices unless it has seen a tree with
# /reserved-memory/splash_region (Efi/Splash.c: SplashNode, Efi/DisplayHandover.c:
# KeepAtExit). With the node present it rewrites its reg with the XBL framebuffer and
# keeps the panel on ("the devicetree keeps the splash: the display stays on",
# Efi/DisplayHandover.c:412-415), which is what makes the DarwinBoot bar, the XNU -v
# console and the panic screen visible.
#
# The kernel tree is not touched: XNU receives the payload ADT, not this FDT.
# The initial reg is <0 0 0 0> because OpenABL replaces it with the real one.
#
#   scripts/h1/make-splash-dtb.sh [hardware.dtb] [output.dtb]
set -euo pipefail

REPO=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
UNIXNU_TREE=${UNIXNU_TREE:-$(cd "$REPO/../UniXNU" 2>/dev/null && pwd || echo "$REPO/../UniXNU")}
IN=${1:-$UNIXNU_TREE/out/odin3/ipados-17.7.11-21H461/odin3/device-tree/hardware.dtb}
OUT=${2:-$REPO/out/odin3-splash.dtb}

for tool in dtc fdtput fdtget; do
    command -v "$tool" >/dev/null || { echo "$tool is missing (package device-tree-compiler)" >&2; exit 1; }
done
[ -f "$IN" ] || { echo "$IN is missing" >&2; exit 1; }

# Only a valid FDT: magic d00dfeed and a parseable structure.
dtc -I dtb -O dts "$IN" >/dev/null 2>&1 || { echo "$IN is not a valid FDT" >&2; exit 1; }

tmp=$(mktemp); trap 'rm -f "$tmp"' EXIT
cp -f "$IN" "$tmp"

# Idempotent: create the node only if it is absent.
have_node() { fdtget "$tmp" "$1" 2>/dev/null >/dev/null; }
have_node /reserved-memory || fdtput -c "$tmp" /reserved-memory
fdtput -t x "$tmp" /reserved-memory "#address-cells" 2
fdtput -t x "$tmp" /reserved-memory "#size-cells" 2
have_node /reserved-memory/splash_region || fdtput -c "$tmp" /reserved-memory/splash_region
fdtput -t x "$tmp" /reserved-memory/splash_region reg 0 0 0 0

# Verification with the same reads OpenABL performs.
[ "$(fdtget -t x "$tmp" /reserved-memory '#address-cells')" = "2" ]
[ "$(fdtget -t x "$tmp" /reserved-memory '#size-cells')" = "2" ]
[ "$(fdtget -t x "$tmp" /reserved-memory/splash_region reg)" = "0 0 0 0" ]
dtc -I dtb -O dts "$tmp" 2>/dev/null | grep -q 'splash_region'

mkdir -p "$(dirname "$OUT")"
cp -f "$tmp" "$OUT"
echo "splash dtb: $OUT ($(stat -c %s "$OUT") B, sha256 $(sha256sum "$OUT" | cut -d' ' -f1))"
dtc -I dtb -O dts "$OUT" 2>/dev/null | sed -n '/reserved-memory {/,/};/p'
