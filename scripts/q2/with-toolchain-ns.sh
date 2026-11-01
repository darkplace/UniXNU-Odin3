#!/bin/bash
# Run a build-tree script on the Linux host with the container's toolchain, without installing anything.
#
# Why: the container has the patched clang-24 and the Mach-O tools, but its QEMU 8.2 does not expose
# SCTLR_EL1 in the gdbstub target.xml, which the ABI probes need; the host's QEMU 11.1.2 does. The compile
# records of the XNU build name /opt/unixnu-linux/llvm/bin/clang-24, so the toolchain has to appear at that
# path. A user+mount namespace overlays /opt with a tmpfs and binds the copy extracted from the image
# there: the change is visible only inside this process and disappears with it.
#
#   bash scripts/q2/with-toolchain-ns.sh <script> [args...]
set -euo pipefail

PROJECT=${PROJECT:-$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)}
TOOLCHAIN=${TOOLCHAIN:-$PROJECT/firmware/toolchain/opt}
TREE=${TREE:-$PROJECT/firmware/UniXNU/out/qemu/tree}
[ -x "$TOOLCHAIN/bin/xcrun" ] || { echo "$TOOLCHAIN is missing (docker cp of /opt/unixnu-linux and /usr/bin/{xcrun,plutil,codesign})" >&2; exit 1; }
[ $# -ge 1 ] || { echo "uso: $0 <script.py> [args...]" >&2; exit 1; }

# gdb-stop.py resolves share/qemu/edk2-*.fd next to the qemu-system-aarch64 it finds (the host has the same
# firmware as AAVMF). A wrapper script, not a symlink, keeps that share path inside the toolchain.
mkdir -p "$TOOLCHAIN/share/qemu"
ln -sf /usr/share/AAVMF/AAVMF_CODE.fd "$TOOLCHAIN/share/qemu/edk2-aarch64-code.fd"
ln -sf /usr/share/AAVMF/AAVMF_VARS.fd "$TOOLCHAIN/share/qemu/edk2-arm-vars.fd"
printf '#!/bin/sh\nexec /usr/bin/qemu-system-aarch64 "$@"\n' > "$TOOLCHAIN/bin/qemu-system-aarch64"
chmod +x "$TOOLCHAIN/bin/qemu-system-aarch64"
# xnu-host-cc (the compiler of XNU's SETUP host tools) hardcodes the image's /usr/lib/llvm-18/bin/clang,
# which does not exist on the host. Point it at the host clang; the SDK headers and libmacho come from the
# extracted toolchain, so only the driver path changes.
HOST_CC="$TOOLCHAIN/CommandLineTools/usr/bin/xnu-host-cc"
if [ -f "$HOST_CC" ]; then
    sed -i 's|^exec "/usr/lib/llvm-18/bin/clang"|exec "/usr/bin/clang"|' "$HOST_CC"
fi

# build-native-legacy-display.py asks `brew --prefix llvm` for llvm-objcopy.
mkdir -p "$TOOLCHAIN/llvm-prefix/bin"
ln -sf /usr/bin/llvm-objcopy "$TOOLCHAIN/llvm-prefix/bin/llvm-objcopy"
cat > "$TOOLCHAIN/bin/brew" <<'BREW'
#!/bin/sh
# brew(1) shim: only `--prefix [llvm]` is used, for the LLVM binutils of this Linux host.
here=$(cd "$(dirname "$0")" && pwd)
if [ "${1:-}" = "--prefix" ]; then
    if [ "${2:-}" = "llvm" ]; then echo "$here/../llvm-prefix"; else echo "$here/.."; fi
    exit 0
fi
echo "brew shim: unsupported call: $*" >&2
exit 1
BREW
chmod +x "$TOOLCHAIN/bin/brew"
export TOOLCHAIN TREE

exec unshare -r -m /bin/bash -c '
set -euo pipefail
mount -t tmpfs tmpfs /opt
mkdir -p /opt/unixnu-linux
mount --bind "$TOOLCHAIN" /opt/unixnu-linux
# scripts/unixnu/kernel.py requires /usr/bin/xcrun to exist (setup-toolchain.py installs the shim there).
# An overlay over /usr/bin gives that path without touching the host.
overlay=$(mktemp -d)
mkdir -p "$overlay/u" "$overlay/w"
if mount -t overlay overlay -o "lowerdir=/usr/bin,upperdir=$overlay/u,workdir=$overlay/w" /usr/bin 2>/dev/null; then
    for shim in xcrun codesign plutil; do
        [ -x "/opt/unixnu-linux/bin/$shim" ] && ln -sf "/opt/unixnu-linux/bin/$shim" "/usr/bin/$shim"
    done
    # mig(1) from cctools asks /usr/bin/arch (a macOS habit); coreutils has it in the container, not here.
    printf "#!/bin/sh\nexec uname -m\n" > /usr/bin/arch && chmod +x /usr/bin/arch
fi
export UNIXNU_TOOLCHAIN=/opt/unixnu-linux
export PATH=/opt/unixnu-linux/bin:/opt/unixnu-linux/llvm/bin:/usr/local/bin:/usr/bin:/bin
export XNU_TOOLCHAIN_BIN="/opt/unixnu-linux/CommandLineTools/usr/bin /opt/unixnu-linux/XcodeDefault.xctoolchain/usr/bin"
export UNIXNU_SDKROOT="$TREE/.cache/fakeroot"
export CCC_OVERRIDE_OPTIONS="# +-Wno-missing-designated-field-initializers +-Wno-unused-but-set-global +-Wno-unsafe-buffer-usage +-Wno-cast-function-type-strict +-Wno-shadow-header"
cd "$TREE"
exec "$@"
' bash "$@"
