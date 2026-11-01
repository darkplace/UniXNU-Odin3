#!/bin/bash
# Resolve a kernel virtual address (an ELR/PC read off the diagnostic page) to the symbol of
# this build's unstripped kernel. Usage: bash scripts/h1/resolve-pc.sh 0xfffffff0079e6d58 [...]
set -euo pipefail
PROJECT=${PROJECT:-$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)}
U=${UNSTRIPPED:-$PROJECT/firmware/UniXNU-enc3l/out/odin3/ipados-17.7.11-21H461/odin3/tree/build/xnu/ios-obj/DEVELOPMENT_ARM64_BCM2837/mach.development.bcm2837.unstripped.noctf}
syms=$(bash "$PROJECT/scripts/h1/container-run.sh" "$PROJECT" /opt/unixnu-linux/CommandLineTools/usr/bin/nm -n "$U" | grep -E '^[0-9a-f]{16} [TtSs] ')
echo "unstripped sha256 $(sha256sum "$U" | cut -d' ' -f1)"
for addr in "$@"; do
  python3 -I -c '
import sys
target = int(sys.argv[1], 16)
best = None
for line in sys.stdin:
    value, _, name = line.split()
    if int(value, 16) <= target: best = (int(value, 16), name)
    else: break
print(f"{sys.argv[1]} -> {best[1]}+{target - best[0]:#x}" if best else f"{sys.argv[1]} -> no symbol")
' "$addr" <<<"$syms"
done
