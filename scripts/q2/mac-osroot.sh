#!/bin/bash
# macOS-only steps of scripts/build-os-root.py, run on the Mac (no Xcode needed).
#
# build-os-root.py mixes host-neutral compile steps (Linux container: clang-24 patched for XNU) with
# macOS-only disk-image plumbing (hdiutil/diskutil, /dev/rdiskN, APFS create/resize). This script runs
# only the latter, in the same order, in the same build tree:
#
#   2  extract-target-volume.py          -> done on Linux, the .dmg files are copied in
#   3  kits/VerifiedRoot/tests/check.py  -> owner keys + host verifier dylib
#   4  build-owner-os-policy.py          -> with the OS image attached read-only at os-mount
#      prepare-owner-data-fixture.py     -> 32 MiB APFS Data seed from the OS's private/var
#   5  build-owner-systemos-policy.py
#   8  Data seed grown to 1 GiB + springboard prefs + packed_ramdisk
#
# Everything else (storage ABI, verified-root probe, VirtIO/RamBlock/display bindings, trust cache) stays
# on Linux. Usage:
#   bash scripts/q2/mac-osroot.sh
set -euo pipefail

PROJECT=${PROJECT:-$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)}
ROOT=${ROOT:-$PROJECT/firmware/UniXNU/out/qemu/tree}
PY=/opt/homebrew/bin/python3.12
OPENSSL=/opt/homebrew/opt/openssl@3
export PATH="$PROJECT/tools:/opt/homebrew/bin:$PATH"
# check.py cross-compiles one object for arm64-apple-ios17.0 with -Werror. This Mac has only the Command
# Line Tools, so the shim answers the iPhoneOS SDK query with the macOS SDK and clang then warns
# -Wincompatible-sysroot; CCC_OVERRIDE_OPTIONS appends -Wno-incompatible-sysroot. The object is a
# freestanding compile (no system headers), but it is built against the macOS SDK: a documented deviation.
export CCC_OVERRIDE_OPTIONS="+-Wno-incompatible-sysroot"
VOL=$ROOT/.cache/firmware/target-volumes
DATA=$ROOT/.cache/firmware/owner-data
OS_DMG=$VOL/044-76040-038.dmg
SYS_DMG=$VOL/044-76034-038.dmg
MOUNT=$VOL/os-mount

say() { printf '\n== %s\n' "$*"; }
die() { echo "FALTA: $*" >&2; exit 1; }

cd "$ROOT"
mkdir -p "$VOL" "$DATA" "$ROOT/build"
[ -f "$OS_DMG" ] || die "$OS_DMG"
[ -f "$SYS_DMG" ] || die "$SYS_DMG"

say "host"
sw_vers -productVersion
"$PY" --version
"$OPENSSL/bin/openssl" version

say "3. owner keys + host verifier (kits/VerifiedRoot/tests/check.py)"
"$PY" kits/VerifiedRoot/tests/check.py --output build/verified-root-host --openssl-prefix "$OPENSSL"
ls -l build/verified-root-host/verified-root.dylib build/verified-root-host/keys/owner.pem

say "4. attach the original OS image read-only"
mkdir -p "$MOUNT"
hdiutil detach "$MOUNT" 2>/dev/null || true
hdiutil attach -readonly -nobrowse -noverify -noautofsck -mountpoint "$MOUNT" -plist "$OS_DMG" > "$ROOT/build/os-attach.plist"
"$PY" - "$ROOT/build/os-attach.plist" <<'EOF'
import plistlib, sys
info = plistlib.loads(open(sys.argv[1], 'rb').read())
whole = [e['dev-entry'] for e in info['system-entities'] if e.get('content-hint') == 'GUID_partition_scheme']
print(whole[0])
open(sys.argv[1] + '.whole', 'w').write(whole[0] + '\n')
EOF
WHOLE=$(cat "$ROOT/build/os-attach.plist.whole")
echo "whole device: $WHOLE"

cleanup() { hdiutil detach "$WHOLE" >/dev/null 2>&1 || hdiutil detach "$MOUNT" -force >/dev/null 2>&1 || true; }
trap cleanup EXIT

say "4a. build-owner-os-policy.py (signs and verifies every 4 KiB block)"
if [ ! -f build/verified-root-os/verification.json ]; then
  "$PY" scripts/build-owner-os-policy.py
else
  echo "already built: build/verified-root-os/verification.json"
fi

say "4b. prepare-owner-data-fixture.py (32 MiB APFS seed of private/var)"
if [ ! -f "$DATA/seed.json" ]; then
  "$PY" scripts/prepare-owner-data-fixture.py --output "$DATA"
else
  echo "already built: $DATA/seed.json"
fi

say "detach the OS image"
cleanup
trap - EXIT
hdiutil info | grep -c "$OS_DMG" || true

say "5. build-owner-systemos-policy.py"
if [ ! -f build/verified-root-systemos/verification.json ]; then
  "$PY" scripts/build-owner-systemos-policy.py
else
  echo "already built: build/verified-root-systemos/verification.json"
fi

say "8. Data seed grown to 1 GiB + springboard prefs + packed ramdisk"
"$PY" "$PROJECT/scripts/q2/mac-grow-data.py" --tree "$ROOT"

say "resultado"
for p in build/verified-root-host/verified-root.dylib build/verified-root-host/keys/owner-public.pem \
         build/verified-root-os/verification.json build/verified-root-systemos/verification.json \
         "$DATA/seed.json" "$DATA/owner-data-1g.dbr"; do
  [ -e "$p" ] && stat -f '%N %z' "$p" || echo "FALTA $p"
done
shasum -a 256 "$DATA/owner-data-1g.dbr" build/verified-root-host/keys/owner-public.pem \
  build/verified-root-os/manifest.bin build/verified-root-systemos/manifest.bin | tee "$ROOT/build/q2-mac-sha256.txt"
echo
echo "Devolver a Linux: build/verified-root-host build/verified-root-os build/verified-root-systemos"
echo "y .cache/firmware/owner-data (seed.json + owner-data-1g.dbr)"
