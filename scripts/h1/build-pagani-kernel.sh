#!/bin/bash
# Build the Odin 3 kernel (or package, payload, ...) inside the unixnu-toolchain:v1 container.
#
#   UNIXNU_TREE=<UniXNU checkout at upstream.rev> bash scripts/h1/build-pagani-kernel.sh build kernel
#
# The device repository passed to the tool as -D is this repository (or devices/odin3-pagani inside it,
# which is where the private tree keeps it). IPSW is your own restore image, also exported as
# UNIXNU_IPSW; the toolchain reads it for the kernelcache, the trust cache and the ADT inputs.
set -uo pipefail
REPO=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
UNIXNU_TREE=${UNIXNU_TREE:-$(cd "$REPO/../UniXNU" 2>/dev/null && pwd || echo "$REPO/../UniXNU")}
IPSW=${IPSW:-$UNIXNU_TREE/../ipsw/iPad_Pro_HFR_17.7.11_21H461_Restore.ipsw}
STEP=${*:-build kernel}
IDRAGON_PROJECT=${IDRAGON_PROJECT:-$REPO}
SHIM_DIR=${SHIM_DIR:-$(realpath "$(dirname "$0")/shim")}
DEVICE=${DEVICE:-$REPO}
if [ ! -f "$DEVICE/board.toml" ] && [ -f "$REPO/devices/odin3-pagani/board.toml" ]; then
    DEVICE=$REPO/devices/odin3-pagani
fi
[ -f "$IPSW" ] || { echo "IPSW not found: $IPSW (export IPSW=...)" >&2; exit 1; }
[ -d "$UNIXNU_TREE" ] || { echo "UNIXNU_TREE not found: $UNIXNU_TREE" >&2; exit 1; }
[ -f "$DEVICE/board.toml" ] || { echo "device repository not found: $DEVICE" >&2; exit 1; }

# Reproducibility: XNU writes its build date into version.c through newvers.pl
# (config/newvers.pl:162 honours KERNEL_BUILD_DATE), and clang stamps __DATE__/__TIME__ from
# SOURCE_DATE_EPOCH. Both default to the date of the pinned revision, so two builds of the same
# sources report the same date instead of the hour they ran at.
if [ -z "${SOURCE_DATE_EPOCH:-}" ]; then
    SOURCE_DATE_EPOCH=$(git -C "$UNIXNU_TREE" log -1 --format=%ct 2>/dev/null) || SOURCE_DATE_EPOCH=1700000000
fi
if [ -z "${KERNEL_BUILD_DATE:-}" ]; then
    KERNEL_BUILD_DATE=$(LC_ALL=C date -u -d "@$SOURCE_DATE_EPOCH" '+%a %b %e %H:%M:%S UTC %Y')
fi
export SOURCE_DATE_EPOCH KERNEL_BUILD_DATE
echo "reproducible build date: $KERNEL_BUILD_DATE (epoch $SOURCE_DATE_EPOCH)"

docker run --rm --user "$(id -u):$(id -g)" \
  -e HOME=/tmp -e JOBS=${JOBS:-10} -e UNIXNU_TOOLCHAIN=/opt/unixnu-linux \
  -e XNU_TOOLCHAIN_BIN="/opt/unixnu-linux/CommandLineTools/usr/bin /opt/unixnu-linux/XcodeDefault.xctoolchain/usr/bin" \
  -e UNIXNU_IPSW="$IPSW" \
  -e SOURCE_DATE_EPOCH="$SOURCE_DATE_EPOCH" -e KERNEL_BUILD_DATE="$KERNEL_BUILD_DATE" \
  -e IDRAGON_PROJECT="$IDRAGON_PROJECT" \
  -e PATH="$SHIM_DIR:/opt/unixnu-linux/bin:/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin" \
  -v /usr:/hostusr:ro \
  -v "$(dirname "$REPO"):$(dirname "$REPO")" \
  -v "$(dirname "$DEVICE"):$(dirname "$DEVICE")" \
  -v "$UNIXNU_TREE:$UNIXNU_TREE" \
  -v "$(dirname "$IPSW"):$(dirname "$IPSW")" \
  -w "$UNIXNU_TREE" unixnu-toolchain:v1 \
  ./unixnu $STEP -D "$DEVICE"
