#!/bin/bash
# Runs the real host code of the OdinSd driver (drivers/OdinSd/SdHost.h) against a software model of the SDHCI v5 /
# Qualcomm SDCC2 controller and of a 238 GB SDXC card, on the build machine. No device, no card access.
#
#   bash scripts/h1/test-odin-sd.sh            # reviewed driver, then the pre-review driver for comparison
#
# The pre-review host code is the one of commit d519259 (the build the user booted at kernel 3f61e107); its register
# accessors are rewired to the model, nothing else is changed.
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
DRV=$ROOT/devices/odin3-pagani/drivers/OdinSd
OUT=${OUT:-$(mktemp -d)}
CXX=${CXX:-g++}
FLAGS="-std=c++17 -O1 -Wall -Wno-unused-function -Wno-unused-variable -Wno-format -Wno-unused-parameter -DODIN_SD_TEST=1 -I$DRV/test/include -I$DRV"

# The real store adapter (OdinSd.cpp, from "ABI names retained" to the probe) is compiled into the model, so the store, the
# fence and the transport are tested together against the card model.
python3 -I - "$DRV/OdinSd.cpp" "$OUT/StoreImplementation.h" <<'PY'
import sys
from pathlib import Path
source = Path(sys.argv[1]).read_text()
Path(sys.argv[2]).write_text(source[source.index("// ABI names retained for NativeRamBlock"):source.index("IOService *NativeUfs::probe")])
PY
$CXX $FLAGS -DODIN_SD_STORE_TEST=1 -I"$OUT" -o "$OUT/model" "$DRV/test/odin_sd_model.cpp"
echo "######## reviewed driver"
"$OUT/model"
status=$?

# Pre-review host code: lines 41-664 of OdinSd.cpp at d519259, accessors replaced by the model hooks.
git -C "$ROOT" show d519259:devices/odin3-pagani/drivers/OdinSd/OdinSd.cpp > "$OUT/old.cpp"
python3 -I - "$OUT/old.cpp" "$OUT/SdHostLegacy.h" <<'PY'
import sys
from pathlib import Path
lines = Path(sys.argv[1]).read_text().split('\n')
host = '\n'.join(lines[40:664])
start = host.index('static inline uint8_t sd8 (unsigned offset)')
end = host.index('// Polls the combined interrupt status')
hooks = '''extern "C" uint32_t odinSdTestRead (unsigned offset, unsigned width);
extern "C" void odinSdTestWrite (unsigned offset, unsigned width, uint32_t value);
static inline uint8_t sd8 (unsigned offset) { return static_cast<uint8_t> (odinSdTestRead (offset, 1)); }
static inline uint16_t sd16 (unsigned offset) { return static_cast<uint16_t> (odinSdTestRead (offset, 2)); }
static inline uint32_t sd32 (unsigned offset) { return odinSdTestRead (offset, 4); }
static inline void sdWrite8 (unsigned offset, uint8_t value) { odinSdTestWrite (offset, 1, value); }
static inline void sdWrite16 (unsigned offset, uint16_t value) { odinSdTestWrite (offset, 2, value); }
static inline void sdWrite32 (unsigned offset, uint32_t value) { odinSdTestWrite (offset, 4, value); }
'''
host = host[:start] + hooks + '\n' + host[end:]
host = host.replace('volatile uint32_t *fifo = reinterpret_cast<volatile uint32_t *> (gSd.base + SDHCI_BUFFER);', '')
host = host.replace('uint32_t value = *fifo;', 'uint32_t value = sd32 (SDHCI_BUFFER);').replace('*fifo = value;', 'sdWrite32 (SDHCI_BUFFER, value);')
Path(sys.argv[2]).write_text('#include "GccSdcc.h"\n#include "SdhciRegs.h"\n' + host + '\n')
PY
$CXX $FLAGS -DODIN_SD_LEGACY=1 -I"$OUT" -o "$OUT/model-legacy" "$DRV/test/odin_sd_model.cpp"
echo
echo "######## pre-review driver (expected to fail)"
"$OUT/model-legacy" || true
exit $status
