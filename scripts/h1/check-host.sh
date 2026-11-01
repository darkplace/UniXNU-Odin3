#!/bin/bash
# Check everything an hour-long build needs, before it starts. Reads only: it writes nothing.
#
#   bash scripts/h1/check-host.sh [--ipsw FILE] [--tree DIR] [--repo DIR]
#
# Every missing item prints the command or the document that fixes it. Exit 0 when nothing is
# missing; exit 1 otherwise. Warnings (the two inputs that need a Mac, an unfinished build) do not
# fail the check.
set -uo pipefail
REPO=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
UNIXNU_TREE=${UNIXNU_TREE:-$(cd "$REPO/../UniXNU" 2>/dev/null && pwd || echo "$REPO/../UniXNU")}
IPSW=${IPSW:-${UNIXNU_IPSW:-$UNIXNU_TREE/../ipsw/iPad_Pro_HFR_17.7.11_21H461_Restore.ipsw}}
DEVICE=$REPO
[ -f "$DEVICE/board.toml" ] || DEVICE=$REPO/devices/odin3-pagani

while [ $# -gt 0 ]; do
    case "$1" in
        --ipsw) IPSW=$2; shift 2 ;;
        --tree) UNIXNU_TREE=$2; shift 2 ;;
        --repo) REPO=$2; DEVICE=$2; shift 2 ;;
        -h|--help) sed -n '2,9p' "$0"; exit 0 ;;
        *) echo "unknown option: $1" >&2; exit 2 ;;
    esac
done

fail=0
warn=0
ok()   { printf '  ok    %s\n' "$*"; }
miss() { printf '  MISS  %s\n         -> %s\n' "$1" "$2"; fail=1; }
note() { printf '  warn  %s\n' "$*"; warn=1; }

tools=$(python3 - "$REPO/tools.toml" <<'PY' 2>/dev/null
import sys, tomllib
t = tomllib.load(open(sys.argv[1], 'rb'))
print(t.get('toolchain', {}).get('image', ''))
print(t.get('toolchain', {}).get('dockerfile', ''))
print(t.get('tool', {}).get('git', ''))
print(t.get('tool', {}).get('commit_file', ''))
print(t.get('tool', {}).get('patches', ''))
print(t.get('firmware', {}).get('git', ''))
print(t.get('firmware', {}).get('commit', ''))
print(t.get('firmware', {}).get('marker', ''))
print(t.get('macos', {}).get('git', ''))
print(t.get('macos', {}).get('commit', ''))
PY
) || tools=""
mapfile -t T <<<"$tools"
IMAGE=${T[0]:-unixnu-toolchain:v1}
DOCKERFILE=${T[1]:-scripts/toolchain/Dockerfile.toolchain}
TOOL_GIT=${T[2]:-https://github.com/UniXNU/UniXNU}
COMMIT_FILE=${T[3]:-upstream.rev}
PATCHES=${T[4]:-patches/unixnu}
FIRMWARE_GIT=${T[5]:-}
FIRMWARE_COMMIT=${T[6]:-}
FIRMWARE_MARKER=${T[7]:-}
MACOS_GIT=${T[8]:-}
MACOS_COMMIT=${T[9]:-}
[ -n "${T[0]:-}" ] || miss "tools.toml" "missing or unreadable: $REPO/tools.toml"

echo "== host tools"
for tool in docker git python3 mformat mcopy dtc sfdisk sha256sum; do
    command -v "$tool" >/dev/null 2>&1 && ok "$tool" || miss "$tool" "install it (mtools, device-tree-compiler, util-linux)"
done

echo "== toolchain image"
if ! command -v docker >/dev/null 2>&1; then
    miss "docker" "install Docker; the build runs in $IMAGE"
elif docker image inspect "$IMAGE" >/dev/null 2>&1; then
    ok "$IMAGE ($(docker image inspect --format '{{.Id}}' "$IMAGE" | cut -c8-19))"
else
    miss "image $IMAGE" "docker build -t $IMAGE -f $DOCKERFILE ."
fi

echo "== build tree"
if [ -d "$UNIXNU_TREE/.git" ] || [ -f "$UNIXNU_TREE/.git" ]; then
    found=$(git -C "$UNIXNU_TREE" rev-parse HEAD 2>/dev/null)
    want=$(cat "$DEVICE/$COMMIT_FILE" 2>/dev/null)
    if [ "$found" = "$want" ]; then
        ok "$UNIXNU_TREE at ${want:0:12}"
    else
        miss "the tree is at ${found:0:12}, the board pins ${want:0:12}" \
             "git -C $UNIXNU_TREE checkout $want   (docs/build.md, step 1)"
    fi
    applied=0
    total=0
    for patch in "$DEVICE/$PATCHES"/*.patch; do
        [ -f "$patch" ] || continue
        total=$((total + 1))
        git -C "$UNIXNU_TREE" apply --check --reverse "$patch" >/dev/null 2>&1 && applied=$((applied + 1))
    done
    if [ "$total" -eq 0 ]; then
        note "no patches under $PATCHES"
    elif [ "$applied" -eq "$total" ]; then
        ok "$total host patches applied"
    else
        miss "$applied of $total host patches applied" \
             "for p in $DEVICE/$PATCHES/*.patch; do git -C $UNIXNU_TREE apply \"\$p\"; done"
    fi
else
    miss "$UNIXNU_TREE is not a checkout" "git clone --recurse-submodules $TOOL_GIT $UNIXNU_TREE"
fi

echo "== restore image"
if [ -f "$IPSW" ]; then
    size=$(stat -c %s "$IPSW")
    if [ "$size" -gt 6000000000 ]; then
        ok "$(basename "$IPSW") ($((size / 1024 / 1024 / 1024)) GiB)"
    else
        miss "$(basename "$IPSW") is only $((size / 1024 / 1024)) MiB" "a full 21H461 restore image is about 6.8 GiB"
    fi
else
    miss "no restore image at $IPSW" "put your own iPad_Pro_HFR_17.7.11_21H461_Restore.ipsw there, or export UNIXNU_IPSW"
fi
cache=$UNIXNU_TREE/out/ipsw/21H461/kernelcache.decompressed
[ -f "$cache" ] && ok "kernelcache cache present" \
    || note "no kernelcache cache yet: run 'unixnu ipsw fetch -D $DEVICE --ipsw \"\$UNIXNU_IPSW\"' once"

echo "== firmware sources"
if [ -z "$FIRMWARE_GIT" ]; then
    note "tools.toml has no [firmware] entry; 'build package' will need --uefi"
else
    uefi=""
    for candidate in "${UNIXNU_UEFI:-}" "$UNIXNU_TREE/../Mu-Silicium" "$REPO/../Mu-Silicium" "$REPO/../../Mu-Silicium"; do
        [ -n "$candidate" ] && [ -f "$candidate/$FIRMWARE_MARKER" ] && { uefi=$candidate; break; }
    done
    if [ -z "$uefi" ]; then
        miss "no Mu-Silicium checkout with $FIRMWARE_MARKER" \
             "git clone $FIRMWARE_GIT <dir> && git -C <dir> checkout $FIRMWARE_COMMIT && export UNIXNU_UEFI=<dir>"
    else
        at=$(git -C "$uefi" rev-parse --short=12 HEAD 2>/dev/null || echo '?')
        case "$FIRMWARE_COMMIT" in
            "$at"*) ok "Mu-Silicium at $uefi ($at)" ;;
            *) note "Mu-Silicium at $uefi is $at; tools.toml pins $FIRMWARE_COMMIT" ;;
        esac
    fi
fi

echo "== inputs the kernel build reads"
project=${IDRAGON_PROJECT:-$REPO}
for input in \
    "$project/firmware/UniXNU/out/qemu/tree/build/verified-root-os/manifest.bin" \
    "$project/firmware/UniXNU/out/qemu/tree/build/verified-root-systemos/manifest.bin" \
    "$project/firmware/UniXNU-qemu/.cache/firmware/target-volumes/044-76034-038.dmg"; do
    [ -f "$input" ] || note "missing $(basename "$input") in $(dirname "$input")"
done
[ -z "$MACOS_GIT" ] || echo "         the three above come from macOS: $MACOS_GIT at $MACOS_COMMIT (scripts/q2/mac-osroot.sh)"

echo "== disk space"
free=$(df -Pk "$REPO" | awk 'NR==2 {print $4}')
if [ "${free:-0}" -gt $((30 * 1024 * 1024)) ]; then
    ok "$((free / 1024 / 1024)) GiB free"
else
    miss "only $((free / 1024 / 1024)) GiB free" "the IPSW cache, the tree and the card images need about 30 GiB"
fi

echo
if [ "$fail" -ne 0 ]; then
    echo "RESULT: something is missing; fix the MISS lines above before building."
    exit 1
fi
[ "$warn" -eq 0 ] && echo "RESULT: ready." || echo "RESULT: ready, with warnings."
exit 0
