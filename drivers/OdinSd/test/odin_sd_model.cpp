// SPDX-License-Identifier: BSD-2-Clause-Patent
// Software model of an SDHCI v5 / Qualcomm SDCC2 controller and a 238 GB SDXC card, to run the real host code of
// OdinSd (SdHost.h) on the build machine. It checks the driver's control flow and its reading of the registers and
// of the card's answers against the behaviour documented by the SD Host Controller spec and by Linux
// (drivers/mmc/host/sdhci.c, sdhci-msm.c). It is NOT the hardware: where the model assumes (for example that the core
// stalls until the PWRCTL handshake is acknowledged) the assumption is named next to the code.
//
//   usage: odin_sd_model [--legacy]      (--legacy compiles the pre-review driver, see test-odin-sd.sh)
#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <vector>

typedef struct { int unused; } IOLock;
static IOLock gLockObject;
static IOLock *IOLockAlloc (void) { return &gLockObject; }
static void IOLockLock (IOLock *) {}
static void IOLockUnlock (IOLock *) {}
static unsigned long long gMicros = 0;
static void IODelay (unsigned us) { gMicros += us; }
static void IOSleep (unsigned ms) { gMicros += ms * 1000ULL; }
static bool gVerbose = false;
static void IOLog (const char *format, ...)
{
  if (!gVerbose) return;
  va_list arguments;
  va_start (arguments, format);
  vprintf (format, arguments);
  va_end (arguments);
}
#if !defined(__APPLE__) && !defined(__GLIBC__)
static size_t strlcpy (char *to, const char *from, size_t size)
{
  size_t length = strlen (from);
  if (size) {
    size_t n = length < size - 1 ? length : size - 1;
    memcpy (to, from, n);
    to[n] = 0;
  }
  return length;
}
#endif

#define ODIN_SD_TEST 1
#ifdef ODIN_SD_LEGACY
#define SD_DIAG_LINES 6
static char native_sd_diag[SD_DIAG_LINES][88];
static unsigned native_sd_diag_count = 0;
#include "SdHostLegacy.h"
#else
#include "SdHost.h"
#ifdef ODIN_SD_STORE_TEST
#include "SdStore.h"
static unsigned gBootMode = 0, gBootReset = 0;
static char gBootGuid[40] = "9dd50fa6-e20f-44d2-bf76-ddfb0e4cf42e";
extern "C" { uint8_t native_ram_block_seed_sha256[32] = { 7 }; }
static bool PE_parse_boot_argn (const char *name, void *value, unsigned bytes)
{
  if (!strcmp (name, "sheng-data-persist")) { memcpy (value, &gBootMode, bytes); return true; }
  if (!strcmp (name, "sheng-data-reset")) { memcpy (value, &gBootReset, bytes); return true; }
  if (!strcmp (name, "odin-data-guid")) { memset (value, 0, bytes); strncpy ((char *) value, gBootGuid, bytes); return true; }
  return false;
}
#include "StoreImplementation.h"
#endif
#endif

// ------------------------------------------------------------------ the card
static const uint64_t kBlocks = 500170752ULL;          // 238.5 GiB of 512-byte sectors
struct Partition { const char *name; uint64_t first, count; };
static const Partition kParts[3] = { { "UNIXNU", 2048, 819200 }, { "OS", 821248, 11870275 }, { "SystemOS", 12693504, 6451200 } };

static uint32_t pattern (uint64_t lba, unsigned word) { uint64_t x = lba * 0x9E3779B97F4A7C15ULL + word * 0xC2B2AE3D27D4EB4FULL; x ^= x >> 29; return (uint32_t) (x * 0xBF58476D1CE4E5B9ULL >> 32); }

static void putLe (uint8_t *p, uint64_t v, unsigned n) { for (unsigned i = 0; i < n; ++i) p[i] = (uint8_t) (v >> (8 * i)); }

static std::map<uint64_t, std::vector<uint8_t>> gWritten;   // sectors the driver wrote (its boot log)
unsigned gAdmaMutate = 0;

// A card that also carries UniXNUData (the layout of docs/builds/odin3-persistence-p4): valid, CRC-checked GPT copies.
static bool gStoreLayout = false;
static const uint64_t kDataFirst = 19144704, kDataSectors = 4194304;           // 2 GiB, 1 MiB aligned, right after p3
static uint8_t gGptArray[16384], gGptPrimary[512], gGptBackup[512];
static uint32_t crc32 (const uint8_t *p, size_t n) { uint32_t v = ~0u; for (size_t i = 0; i < n; ++i) { v ^= p[i]; for (int b = 0; b < 8; ++b) v = (v >> 1) ^ (0xedb88320u & (0u - (v & 1))); } return ~v; }
static void buildStoreGpt (const char *guid)
{
  memset (gGptArray, 0, sizeof gGptArray);
  for (unsigned i = 0; i < 3; ++i) {
    uint8_t *e = gGptArray + i * 128;
    memset (e, 0x11 + i, 16); memset (e + 16, 0x70 + i, 16);
    putLe (e + 32, kParts[i].first, 8); putLe (e + 40, kParts[i].first + kParts[i].count - 1, 8);
    for (unsigned c = 0; kParts[i].name[c]; ++c) putLe (e + 56 + c * 2, (uint8_t) kParts[i].name[c], 2);
  }
  uint8_t *e = gGptArray + 3 * 128;
  const uint8_t basic[16] = { 0xa2, 0xa0, 0xd0, 0xeb, 0xe5, 0xb9, 0x33, 0x44, 0x87, 0xc0, 0x68, 0xb6, 0xb7, 0x26, 0x99, 0xc7 };
  memcpy (e, basic, 16);
#if defined(ODIN_SD_STORE_TEST) && !defined(ODIN_SD_LEGACY)
  odin_store::guid (guid, e + 16);
#endif
  putLe (e + 32, kDataFirst, 8); putLe (e + 40, kDataFirst + kDataSectors - 1, 8);
  const char *name = "UniXNUData";
  for (unsigned c = 0; name[c]; ++c) putLe (e + 56 + c * 2, (uint8_t) name[c], 2);
  for (unsigned backup = 0; backup < 2; ++backup) {
    uint8_t *h = backup ? gGptBackup : gGptPrimary;
    memset (h, 0, 512); memcpy (h, "EFI PART", 8);
    putLe (h + 8, 0x00010000, 4); putLe (h + 12, 92, 4);
    putLe (h + 24, backup ? kBlocks - 1 : 1, 8); putLe (h + 32, backup ? 1 : kBlocks - 1, 8);
    putLe (h + 40, 34, 8); putLe (h + 48, kBlocks - 34, 8); h[56] = 0x42;
    putLe (h + 72, backup ? kBlocks - 33 : 2, 8); putLe (h + 80, 128, 4); putLe (h + 84, 128, 4);
    putLe (h + 88, crc32 (gGptArray, sizeof gGptArray), 4);
    putLe (h + 16, crc32 (h, 92), 4);
  }
}

static void sector (uint64_t lba, uint8_t *out)
{
  auto written = gWritten.find (lba);
  if (written != gWritten.end ()) { memcpy (out, written->second.data (), 512); return; }
  memset (out, 0, 512);
  if (gStoreLayout) {
    if (lba == 1) { memcpy (out, gGptPrimary, 512); return; }
    if (lba >= 2 && lba < 34) { memcpy (out, gGptArray + (lba - 2) * 512, 512); return; }
    if (lba >= kBlocks - 33 && lba < kBlocks - 1) { memcpy (out, gGptArray + (lba - (kBlocks - 33)) * 512, 512); return; }
    if (lba == kBlocks - 1) { memcpy (out, gGptBackup, 512); return; }
    if (lba >= kDataFirst && lba < kDataFirst + kDataSectors) return;          // a blank partition reads as zeros
  }
  if (lba == 0) { out[510] = 0x55; out[511] = 0xAA; return; }
  if (lba == 1) {
    memcpy (out, "EFI PART", 8);
    putLe (out + 8, 0x00010000, 4); putLe (out + 12, 92, 4);
    putLe (out + 24, 1, 8); putLe (out + 32, kBlocks - 1, 8);
    putLe (out + 40, 34, 8); putLe (out + 48, kBlocks - 34, 8);
    putLe (out + 72, 2, 8); putLe (out + 80, 128, 4); putLe (out + 84, 128, 4);
    return;
  }
  if (lba >= 2 && lba < 34) {
    unsigned first = (unsigned) (lba - 2) * 4;         // four 128-byte entries per sector
    for (unsigned e = 0; e < 4; ++e) {
      unsigned index = first + e;
      if (index >= 3) continue;
      uint8_t *entry = out + e * 128;
      memset (entry, 0x11 + index, 16);                  // type GUID
      memset (entry + 16, 0x70 + index, 16);             // unique GUID
      putLe (entry + 32, kParts[index].first, 8);
      putLe (entry + 40, kParts[index].first + kParts[index].count - 1, 8);
      for (unsigned c = 0; kParts[index].name[c]; ++c) putLe (entry + 56 + c * 2, (uint8_t) kParts[index].name[c], 2);
    }
    return;
  }
  for (unsigned w = 0; w < 128; ++w) putLe (out + w * 4, pattern (lba, w), 4);
}

// ------------------------------------------------------------------ the controller
extern uint32_t gGcc[];
extern unsigned gRailMv;
struct Model {
  uint8_t r[0x400] = {};
  // Qualcomm power-control block
  uint32_t pwrctlStatus = 0, pwrctlMask = 0, pwrctlCtl = 0;
  bool pwrPending = false;               // ASSUMPTION (sdhci-msm.c:1515-1580): the core waits for the PWRCTL acknowledgement
  // card
  enum { IDLE, READY, IDENT, STBY, TRAN } state = TRAN;
  bool appCommand = false, bus4 = false;
  unsigned acmd41Polls = 0;
  uint32_t rca = 0x1234;
  // data
  std::vector<uint8_t> block;
  size_t blockPos = 0;
  unsigned blocksLeft = 0;
  bool writing = false, multi = false;
  uint64_t lba = 0;
  std::vector<uint8_t> writeBuffer;
  // statistics and options
  unsigned timeoutsWithoutEnable = 0, commands = 0;
  bool firmwareLeftUhs = false;
  bool stallAdoptedCommands = false;      // the adopted state is unusable until a RESET_ALL (exercises the fallback)
  bool handshakeStalls = true;            // model option: the PWRCTL acknowledgement is mandatory
  bool resetKeepsEnable = false;          // model option: RESET_ALL leaves INT_ENABLE untouched
  bool switchUnsupported = false;         // model option: the card does not select high-speed
  bool switchRefused = false;             // supports high-speed, but CMD6 did NOT select it
  bool suppressResponse = false, suppressDataEnd = false; // software deadlines without hardware error bits
  // UHS-I card and rail model. The electrical rules are the SD Physical Layer ones, written as checks on every data command:
  // the host's pad voltage must match the card's signalling, and the clock may only exceed what the selected function allows.
  bool uhsCard = false;                  // answers S18A and supports SDR12/SDR25/SDR50 once at 1.8 V
  bool ignoreCmd11 = false;              // the card does not react to CMD11 (stays a 3.3 V card)
  bool cardKeepsDatLow = false;          // after CMD11 the card never releases DAT[3:0] (switch failed)
  bool refuseSdr50 = false;              // CMD6 does not select function 2
  bool sdr50Flaky = false;               // reads at >50 MHz return CRC errors
  bool datLow = false, voltagePending = false, signal18Card = false;
  unsigned cardFn = 0, cmd0s = 0, cmd11s = 0;
  bool s18rSeen = false;
  unsigned dataFaults = 0;
  unsigned bufferReads = 0;
  uint32_t get32 (unsigned o) const { uint32_t v; memcpy (&v, r + o, 4); return v; }
  void set32 (unsigned o, uint32_t v) { memcpy (r + o, &v, 4); }
  uint16_t get16 (unsigned o) const { uint16_t v; memcpy (&v, r + o, 2); return v; }
  void latch (uint32_t bits)
  {
    if (suppressResponse) bits &= ~1u;
    if (suppressDataEnd) bits &= ~2u;
    uint32_t enable = get32 (0x34);
    uint32_t effective = bits & enable & ~0x8000U;
    if (!effective) { if (bits) ++timeoutsWithoutEnable; return; }
    uint32_t status = get32 (0x30) | effective;
    if (effective & 0xFFFF0000U) status |= 0x8000U;
    set32 (0x30, status);
  }
  void loadBlock ()
  {
    block.assign (512, 0);
    sector (lba, block.data ());
    blockPos = 0;
  }
  void setResponse (uint32_t v) { set32 (0x10, v); }
  void setResponse136 (const uint8_t csd[16])
  {
    // SDHCI keeps bits 127:8 of the 136-bit answer in RESPONSE[119:0] (the CRC byte is dropped).
    uint8_t reg[16] = {};
    for (unsigned i = 0; i < 15; ++i) reg[i] = csd[14 - i];   // reg[0] = lowest byte = csd[14]
    memcpy (r + 0x10, reg, 16);
  }
  unsigned clockHz () const
  {
    // GCC SDCC2 cfg_rcgr: source select in [10:8] (6 = GPLL0_OUT_EVEN 300 MHz, 0 = XO 19.2 MHz), HID divider in [4:0] = 2*div-1.
    uint32_t cfg = gGcc[0x20 / 4];
    unsigned source = (cfg >> 8) & 7, half = cfg & 31;
    unsigned parent = source == 6 ? 300000000U : 19200000U;
    return half ? (unsigned) ((uint64_t) parent * 2 / (half + 1)) : parent;
  }
  bool walkAdma (bool writing)
  {
    uint64_t table = static_cast<uint64_t> (get32 (0x58)) | (static_cast<uint64_t> (get32 (0x5C)) << 32);
    if (!table) return false;
    unsigned expect = blocksLeft * 512, got = 0;
    bool end = false;
    // Linux sdhci.h:377-395: wire stride depends on HC2.v4_mode, NOT on MSM-v5 compatible.
    const unsigned stride = (get16 (0x3E) & (1U << 12)) ? 16 : 12;
    for (unsigned i = 0; i < 8 && !end; ++i) {
      const uint8_t *descriptor = reinterpret_cast<const uint8_t *> (static_cast<uintptr_t> (table + i * stride));
      uint16_t attr = 0, len16 = 0; uint32_t lo = 0, hi = 0;
      memcpy (&attr, descriptor, 2); memcpy (&len16, descriptor + 2, 2);
      memcpy (&lo, descriptor + 4, 4); memcpy (&hi, descriptor + 8, 4);
      if (!(attr & 1) || ((attr >> 4) & 3) != 2) return false;
      uint32_t len = len16 ? len16 : 65536;
      uint64_t address = lo | (static_cast<uint64_t> (hi) << 32);
      // Validate the full span before touching memory (malformed descriptors are test inputs).
      if (!address || (len & 511) || got > expect || len > expect - got) return false;
      uint8_t *pointer = reinterpret_cast<uint8_t *> (static_cast<uintptr_t> (address));
      for (uint32_t at = 0; at < len; at += 512) {
        if (writing) gWritten[lba].assign (pointer + at, pointer + at + 512);
        else sector (lba, pointer + at);
        ++lba; if (blocksLeft) --blocksLeft;
      }
      got += len;
      end = (attr & 2) != 0;
    }
    return end && got == expect && blocksLeft == 0;
  }
  bool linkValid () const
  {
    // What a real card and pad would need for the transfer to survive. A violated rule is a data CRC error.
    unsigned hc2 = get16 (0x3E), uhs = hc2 & 7, hz = clockHz ();
    bool v18 = (hc2 & 8) != 0, hispd = (r[0x28] & 4) != 0;
    if (v18 != signal18Card) return false;                       // pad rail and card signalling must agree
    if (hz > 25000000U && !hispd) return false;
    if (hz > 25000000U && cardFn < 1) return false;              // the card must have been told high speed
    if (hz > 50000000U) return v18 && cardFn == 2 && uhs == 2 && !sdr50Flaky;
    if (signal18Card && hz > 25000000U) return uhs >= 1 && cardFn >= 1;
    return true;
  }
  void command (uint16_t value)
  {
    unsigned index = value >> 8;
    ++commands;
    bool clockOn = (r[0x2C] & 0x04) && (r[0x2D] == 0 || true);
    if ((handshakeStalls && pwrPending) || !clockOn || !(r[0x29] & 1) || stallAdoptedCommands) { latch (0x10000); return; }
    uint32_t argument = get32 (0x08);
    bool app = appCommand;
    appCommand = false;
    switch (index) {
      case 0: state = IDLE; ++cmd0s; setResponse (0); latch (1); return;
      case 8: setResponse (argument & 0xFFF); latch (1); return;
      case 55: appCommand = true; setResponse (0x120); latch (1); return;
      case 41:
        if (!app) { latch (0x10000); return; }
        if (argument & (1U << 24)) s18rSeen = true;
        setResponse (++acmd41Polls < 3 ? 0x40FF8000U : (0xC0FF8000U | ((uhsCard && (argument & (1U << 24))) ? (1U << 24) : 0U)));
        if (acmd41Polls >= 3) state = READY;
        latch (1); return;
      case 2: { uint8_t cid[16]; for (unsigned i = 0; i < 16; ++i) cid[i] = (uint8_t) (0xA0 + i); setResponse136 (cid); state = IDENT; latch (1); return; }
      case 3: setResponse (rca << 16 | 0x500); state = STBY; latch (1); return;
      case 9: {
        uint8_t csd[16] = {};
        csd[0] = 0x40;                                       // CSD_STRUCTURE 1 (SDHC/SDXC)
        uint32_t csize = (uint32_t) (kBlocks / 1024 - 1);
        csd[7] = (uint8_t) ((csize >> 16) & 0x3F); csd[8] = (uint8_t) (csize >> 8); csd[9] = (uint8_t) csize;
        setResponse136 (csd); latch (1); return;
      }
      case 7: setResponse (0x700); state = TRAN; latch (1 | 2); return;
      case 16: setResponse (0x900); latch (1); return;
      case 11:
        ++cmd11s;
        if (ignoreCmd11) { latch (0x10000); return; }
        setResponse (0x900); datLow = true; voltagePending = true; latch (1); return;
      case 51:
        if (!app) { latch (0x10000); return; }
        block.assign (8, 0);
        block[0] = 0x02; block[1] = 0x85; block[2] = 0x80; block[3] = 0x03;       // SD_SPEC 2, SD_SPEC3, 4-bit, CMD20/23
        blockPos = 0; blocksLeft = 1; writing = false;
        setResponse (0x920); latch (1 | 0x20); return;
      case 6:
        if (app) { bus4 = (argument & 3) == 2; setResponse (0x920); latch (1); return; }
        // SWITCH_FUNC: 64-byte status. Function group 1 reports high-speed supported (bit 0)
        // and selected (bit 1), so the driver may raise the clock.
        block.assign (64, 0);
        {
          bool set = (argument >> 31) & 1; unsigned fn = argument & 15;
          unsigned supported = switchUnsupported ? 0x01 : (signal18Card ? 0x07 : 0x03);
          if (set && !switchRefused && fn != 15 && ((supported >> fn) & 1) && !(fn == 2 && refuseSdr50)) cardFn = fn;
          block[13] = (uint8_t) supported;
          block[16] = (uint8_t) cardFn;
        }
        blockPos = 0; blocksLeft = 1; writing = false;
        setResponse (0x920); latch (1 | 0x20); return;
      case 13: setResponse (0x900); latch (1); return;
      case 12: blocksLeft = 0; block.clear (); setResponse (0x900); latch (1 | 2); return;
      case 17: case 18: case 24: case 25: {
        if (state != TRAN) { latch (0x10000); return; }
        writing = (index == 24 || index == 25);
        multi = (index == 18 || index == 25);
        lba = argument;                                      // block addressing (SDXC)
        if (!linkValid ()) { ++dataFaults; setResponse (0x900); latch (1); latch (0x00200000U); return; }
        unsigned count = multi ? get16 (0x06) : 1;
        blocksLeft = count;
        if (get16 (0x0C) & 1) {
          setResponse (0x900);
          if (!walkAdma (writing)) { ++dataFaults; latch (1); latch (0x02000000U); return; }
          latch (1 | 2);
          return;
        }
        writeBuffer.clear ();
        setResponse (0x900);
        if (writing) { latch (1 | 0x10); }
        else { loadBlock (); latch (1 | 0x20); }
        return;
      }
      default: latch (0x10000); return;
    }
  }
  uint32_t readReg (unsigned offset, unsigned width)
  {
    if (offset == 0x20 && width == 4) {
      ++bufferReads;
      uint32_t v = 0;
      if (blockPos + 4 <= block.size ()) { memcpy (&v, block.data () + blockPos, 4); blockPos += 4; }
      if (!block.empty () && blockPos == block.size () && !writing) {
        --blocksLeft; ++lba;
        if (blocksLeft) { if (block.size () == 512) loadBlock (); latch (0x20); }
        else { block.clear (); latch (2); }
      }
      return v;
    }
    if (offset == 0x24) {
      uint32_t v = 0x00010000 | (datLow ? 0U : 0x00F00000U);   // card present, DAT[3:0] pin levels
      if (!block.empty () && !writing) v |= 0x800;
      return v;
    }
    if (offset >= 0x240 && offset < 0x250) {
      if (offset == 0x240) return pwrctlStatus;
      if (offset == 0x244) return pwrctlMask;
      if (offset == 0x24C) return pwrctlCtl;
      return 0;
    }
    uint32_t v = 0;
    memcpy (&v, r + offset, width);
    return v;
  }
  void writeReg (unsigned offset, unsigned width, uint32_t value)
  {
    if (offset == 0x20 && width == 4) {
      if (writeBuffer.size () < 512) { for (unsigned i = 0; i < 4; ++i) writeBuffer.push_back ((uint8_t) (value >> (8 * i))); }
      if (writeBuffer.size () == 512) {
        gWritten[lba] = writeBuffer; writeBuffer.clear (); --blocksLeft; ++lba;
        if (blocksLeft) latch (0x10); else latch (2);
      }
      return;
    }
    if (offset == 0x2F) {
      if (value & 1) {
        bool wasOn = r[0x29] & 1;
        set32 (0x30, 0); if (!resetKeepsEnable) set32 (0x34, 0); set32 (0x38, 0); stallAdoptedCommands = false;
        r[0x28] = 0; r[0x29] = 0; r[0x3E] = r[0x3F] = 0; r[0x2C] = r[0x2D] = 0;
        if (wasOn) { pwrctlStatus |= 1; pwrPending = true; }     // BUS_OFF
      }
      if (value & 6) { set32 (0x30, get32 (0x30) & 0xFFFF0000U & 0); block.clear (); blocksLeft = 0; }
      r[0x2F] = 0;
      return;
    }
    if (offset == 0x29 && width == 1) {
      uint8_t previous = r[0x29];
      r[0x29] = (uint8_t) value;
      if ((value & 1) && !(previous & 1)) { pwrctlStatus |= 2; pwrPending = true; }
      else if (!(value & 1) && (previous & 1)) { pwrctlStatus |= 1; pwrPending = true; }
      return;
    }
    if (offset == 0x3E && width == 2) {
      uint16_t previous = get16 (0x3E);
      memcpy (r + 0x3E, &value, 2);
      if ((previous ^ value) & 8) { pwrctlStatus |= (value & 8) ? 4 : 8; pwrPending = true; }
      return;
    }
    if (offset == 0x2C && width == 2) {
      uint16_t v = (uint16_t) value;
      if (v & 1) v |= 2; else v &= (uint16_t) ~2;           // internal clock stable
      memcpy (r + 0x2C, &v, 2);
      if ((v & 4) && voltagePending && (get16 (0x3E) & 8) && gRailMv == 1800 && !cardKeepsDatLow) {
        datLow = false; voltagePending = false; signal18Card = true;   // clock restarted at a real 1.8 V: the card completes the switch
      }
      return;
    }
    if (offset == 0x30) {                                     // write 1 to clear (8/16/32-bit)
      uint32_t mask = width == 4 ? value : width == 2 ? value : value;
      set32 (0x30, get32 (0x30) & ~mask);
      if (!(get32 (0x30) & 0x7FFF0000U)) set32 (0x30, get32 (0x30) & ~0x8000U);
      return;
    }
    if (offset == 0x0E && width == 2) { command ((uint16_t) value); return; }
    if (offset == 0x244) { pwrctlMask = value; return; }
    if (offset == 0x248) { pwrctlStatus &= ~value; return; }
    if (offset == 0x24C) {
      pwrctlCtl = value;
      if ((value & 5) && !pwrctlStatus) pwrPending = false;
      return;
    }
    memcpy (r + offset, &value, width);
  }
};
static Model gModel;
extern "C" uint32_t odinSdTestRead (unsigned offset, unsigned width) { return gModel.readReg (offset, width); }
extern "C" void odinSdTestWrite (unsigned offset, unsigned width, uint32_t value) { gModel.writeReg (offset, width, value); }
extern "C" uint64_t odinSdTestNowUs (void) { return gMicros; }

// ------------------------------------------------------------------ checks
static unsigned gFailures = 0;
#define CHECK(condition, ...) do { if (!(condition)) { ++gFailures; printf ("  FAIL  "); printf (__VA_ARGS__); printf ("\n"); } else { printf ("  ok    "); printf (__VA_ARGS__); printf ("\n"); } } while (0)

uint32_t gGcc[0x1000 / 4];
unsigned gRailMv = 3000, gHookCalls = 0;
bool gRailOk = true;
static bool railHook (bool low18)
{
  ++gHookCalls;
  if (!gRailOk) return false;
  gRailMv = low18 ? 1800 : 3000;
  return true;
}

static void freshHost (void)
{
  gModel = Model ();
  gWritten.clear ();
  memset (gGcc, 0, sizeof (gGcc));
  gGcc[1] = 1;                                              // branch enable bit set, not halted
  gSd = SdState ();
  gSd.gcc = reinterpret_cast<uintptr_t> (gGcc);
  gSd.base = 1;
  gSd.lock = IOLockAlloc ();
  gLogUsed = SD_LOG_HEADER;
  native_sd_diag_count = 0;
#ifndef ODIN_SD_LEGACY
  gSdUhsMode = 0; gSdSignalHook = nullptr; gSdIoArmed = false; gSdDmaRequest = 0; gAdmaNoted = false; gAdmaMutate = 0;
  gRailMv = 3000; gHookCalls = 0; gRailOk = true;
  gModel.set32 (0x40, 0x04000000U);                         // CAPABILITIES: 1.8 V
  gModel.set32 (0x44, 0x00000007U);                         // CAPABILITIES_1: SDR50, SDR104, DDR50; no tuning bit
#endif
}

static void firmwareState (bool interruptsEnabled, bool powerOn)
{
  // What UEFI left behind: power on at 3.3 V, clock running, card selected; the interrupt status-enable register is
  // whatever the UEFI driver wrote.
  gModel.r[0x29] = powerOn ? 0x0F : 0;
  gModel.set32 (0x34, interruptsEnabled ? SDHCI_INT_ENABLE_MASK : 0);
  gModel.r[0x2C] = 0x07;
  gModel.pwrctlMask = 0xF;
}

static bool verifyReads (void)
{
  bool ok = true;
  const uint64_t probes[] = { 0, 1, 2, 33, 2048, 821248, 821248 + 11870274, 12693504, 12693504 + 6451199, 99999999, kBlocks - 8192, kBlocks - 1 };
  for (uint64_t lba : probes) {
    uint8_t buffer[512], expected[512];
    sector (lba, expected);
    if (!sdReadBlocks (lba, 1, buffer) || memcmp (buffer, expected, 512)) { printf ("    read of LBA %llu differs\n", (unsigned long long) lba); ok = false; }
  }
  static uint8_t big[64 * 1024];
  uint8_t expected[512];
  if (!sdReadBlocks (821248 + 4096, 128, big)) ok = false;
  for (unsigned i = 0; ok && i < 128; ++i) { sector (821248 + 4096 + i, expected); if (memcmp (big + i * 512, expected, 512)) { printf ("    multi-block read differs at block %u\n", i); ok = false; } }
  return ok;
}

static void scenario (const char *name, bool interruptsEnabled, bool powerOn, bool stallAdopt, bool strictHandshake = true, bool keepEnable = false, bool noHighSpeed = false, bool refuseHighSpeed = false)
{
  printf ("\n== %s\n", name);
  freshHost ();
  firmwareState (interruptsEnabled, powerOn);
  gModel.stallAdoptedCommands = stallAdopt;
  gModel.handshakeStalls = strictHandshake;
  gModel.resetKeepsEnable = keepEnable;
  gModel.switchUnsupported = noHighSpeed;
  gModel.switchRefused = refuseHighSpeed;
  bool init = sdCardInit ();
  CHECK (init, "sdCardInit returned %s (%s)", init ? "true" : "false", gSd.reason);
  if (!init) { printf ("    last log lines:\n"); for (unsigned i = 0; i < SD_DIAG_LINES && i < native_sd_diag_count; ++i) printf ("      %s\n", native_sd_diag[(native_sd_diag_count + i) % SD_DIAG_LINES]); return; }
  CHECK (gSd.highCapacity, "SDHC/SDXC addressing detected");
  CHECK (gSd.blocks == kBlocks, "capacity %llu blocks (card has %llu)", (unsigned long long) gSd.blocks, (unsigned long long) kBlocks);
  CHECK (gSd.fourBit, "4-bit bus");
  bool selectedHighSpeed = !noHighSpeed && !refuseHighSpeed;
  CHECK (gSd.clockHz == (selectedHighSpeed ? 50000000U : 25000000U), "service clock %u Hz", gSd.clockHz);
  CHECK (!!(sd8 (SDHCI_HOST_CONTROL) & SDHCI_CTRL_HISPD) == selectedHighSpeed, "high-speed host bit %s", selectedHighSpeed ? "set" : "clear");
  SdWindow windows[4] = {};
  unsigned count = 0;
  const char *why = "";
  bool parsed = gptRead (windows, &count, &why);
  CHECK (parsed && count == 2, "GPT: %u windows (%s)", count, why);
  for (unsigned i = 0; parsed && i < count; ++i) {
    const Partition *p = !strcmp (windows[i].name, "OS") ? &kParts[1] : &kParts[2];
    CHECK (windows[i].firstLba == p->first && windows[i].lastLba == p->first + p->count - 1, "window %s LBA %llu..%llu", windows[i].name,
           (unsigned long long) windows[i].firstLba, (unsigned long long) windows[i].lastLba);
  }
  CHECK (verifyReads (), "reads across the card match the model's sectors");
  gLogFirstLba = gSd.blocks - SD_LOG_RESERVED;
  gLogCardReady = true;
#ifndef ODIN_SD_LEGACY
  gSdWriteWindow[0] = { gLogFirstLba, gLogFirstLba + SD_LOG_SECTORS };
#endif
  CHECK (sdFlushLog () && gWritten.count (gLogFirstLba) && !memcmp (gWritten[gLogFirstLba].data (), SD_LOG_MAGIC, 8), "boot log written at LBA %llu (outside p1-p3 and the backup GPT)", (unsigned long long) gLogFirstLba);
  bool outside = true;
  for (auto &kv : gWritten) { uint64_t l = kv.first; if (l < kParts[2].first + kParts[2].count || l >= kBlocks - 33) { if (!(l >= kBlocks - 8192 - 0 && l < kBlocks - 8192 + SD_LOG_SECTORS)) outside = false; } }
  CHECK (outside, "the only sectors written are the log's 64");
  printf ("    model: %u commands, %u status bits that would have been lost because INT_ENABLE did not cover them\n", gModel.commands, gModel.timeoutsWithoutEnable);
}

#ifndef ODIN_SD_LEGACY
static bool logHas (const char *needle)
{
  for (unsigned i = 0; i < SD_DIAG_LINES; ++i) if (strstr (native_sd_diag[i], needle)) return true;
  return strstr (gLog + SD_LOG_HEADER, needle) != nullptr;
}

struct UhsOptions {
  unsigned mode = 2; bool hook = true, uhsCard = true, tuningBit = false, railOk = true, ignoreCmd11 = false, keepsDatLow = false;
  bool refuseSdr50 = false, sdr50Flaky = false;
};

static bool uhsScenario (const char *name, const UhsOptions &o)
{
  printf ("\n== UHS: %s\n", name);
  freshHost ();
  firmwareState (true, true);
  gSdUhsMode = o.mode;
  gSdSignalHook = o.hook ? railHook : nullptr;
  gModel.uhsCard = o.uhsCard; gModel.ignoreCmd11 = o.ignoreCmd11; gModel.cardKeepsDatLow = o.keepsDatLow;
  gModel.refuseSdr50 = o.refuseSdr50; gModel.sdr50Flaky = o.sdr50Flaky;
  if (o.tuningBit) gModel.set32 (0x44, gModel.get32 (0x44) | SDHCI_USE_SDR50_TUNING);
  gRailOk = o.railOk;
  return sdCardInit ();
}

static void uhsScenarios (void)
{
  { // everything works: SDR50 at 100 MHz, 1.8 V
    UhsOptions o; bool ok = uhsScenario ("UHS-I card, rail moves: SDR50 100 MHz", o);
    CHECK (ok && gSd.identified, "init succeeded");
    CHECK (gSd.signal18 && gSd.uhsCommitted && gRailMv == 1800 && gHookCalls == 1, "CMD11 sent once, rail moved once to %u mV (hook calls %u)", gRailMv, gHookCalls);
    CHECK (!strcmp (gSd.speed, "SDR50") && gSd.clockHz == 100000000U, "speed %s, clock %u Hz", gSd.speed, gSd.clockHz);
    CHECK ((sd16 (SDHCI_HOST_CONTROL2) & 15) == (SDHCI_CTRL_VDD_180 | SDHCI_CTRL_UHS_SDR50), "HOST_CONTROL2 = 1.8 V + SDR50 (0x%04x)", sd16 (SDHCI_HOST_CONTROL2));
    CHECK ((sd32 (SDHCI_MSM_VENDOR_SPEC) & SDHCI_MSM_IO_PAD_PWR_SWITCH) && (sd32 (SDHCI_MSM_VENDOR_SPEC) & SDHCI_MSM_IO_PAD_PWR_SWITCH_EN), "I/O pad switch follows the rail (vendor 0x%08x)", sd32 (SDHCI_MSM_VENDOR_SPEC));
    CHECK ((sd32 (SDHCI_MSM_DLL_CONFIG) & (SDHCI_MSM_DLL_RST | SDHCI_MSM_DLL_PDN)) == (SDHCI_MSM_DLL_RST | SDHCI_MSM_DLL_PDN), "tuning DLL held in reset/power-down at 100 MHz");
    CHECK (gModel.s18rSeen && gModel.cmd11s == 1 && gModel.cmd0s == 1, "ACMD41 carried S18R, one CMD11, no re-init");
    CHECK (verifyReads (), "reads across the card at 100 MHz match (the model rejects an invalid link as a CRC error)");
    CHECK (gModel.dataFaults == 0, "no link rule was violated");
    CHECK (logHas ("UHS: SDR50 100 MHz verified"), "log says SDR50 verified");
  }
  { UhsOptions o; o.tuningBit = true; bool ok = uhsScenario ("controller asks for SDR50 tuning: stop at SDR25 50 MHz (no DLL tuning)", o);
    CHECK (ok && !strcmp (gSd.speed, "SDR25") && gSd.clockHz == 50000000U && gSd.signal18, "speed %s at %u Hz, 1.8 V", gSd.speed, gSd.clockHz);
    CHECK (verifyReads () && gModel.dataFaults == 0, "reads fine at SDR25"); }
  { UhsOptions o; o.refuseSdr50 = true; bool ok = uhsScenario ("card does not select SDR50: SDR25 50 MHz", o);
    CHECK (ok && !strcmp (gSd.speed, "SDR25") && gSd.clockHz == 50000000U, "speed %s at %u Hz", gSd.speed, gSd.clockHz);
    CHECK (verifyReads () && gModel.dataFaults == 0, "reads fine"); }
  { UhsOptions o; o.sdr50Flaky = true; bool ok = uhsScenario ("SDR50 reads fail the check: fall back to SDR25 50 MHz, then verified", o);
    CHECK (ok && !strcmp (gSd.speed, "SDR25") && gSd.clockHz == 50000000U && gSd.signal18, "speed %s at %u Hz, still 1.8 V", gSd.speed, gSd.clockHz);
    CHECK (logHas ("UHS: SDR50 failed; back to SDR25 50 MHz"), "fallback is logged");
    CHECK (gModel.cardFn == 1 && (sd16 (SDHCI_HOST_CONTROL2) & 7) == SDHCI_CTRL_UHS_SDR25, "card and host agree on SDR25 (fn %u hc2 0x%04x)", gModel.cardFn, sd16 (SDHCI_HOST_CONTROL2));
    gModel.sdr50Flaky = false; CHECK (verifyReads (), "reads fine after the fallback"); }
  { UhsOptions o; o.uhsCard = false; bool ok = uhsScenario ("card without S18A: nothing changes, 3.3 V high-speed 50 MHz", o);
    CHECK (ok && !gSd.signal18 && !gSd.uhsCommitted && gHookCalls == 0 && gRailMv == 3000 && gSd.clockHz == 50000000U, "3.3 V, rail untouched, %u Hz", gSd.clockHz);
    CHECK (gModel.cmd11s == 0, "no CMD11"); }
  { UhsOptions o; o.hook = false; bool ok = uhsScenario ("no rail hook (cmd-db entry not resolved): S18R is not even requested", o);
    CHECK (ok && !gModel.s18rSeen && gModel.cmd11s == 0 && !gSd.signal18 && gSd.clockHz == 50000000U, "3.3 V path, %u Hz", gSd.clockHz); }
  { UhsOptions o; o.mode = 0; bool ok = uhsScenario ("mode 0 (default): bit-identical to the pre-UHS behaviour", o);
    CHECK (ok && !gModel.s18rSeen && gModel.cmd11s == 0 && gHookCalls == 0 && gSd.clockHz == 50000000U && !logHas ("PROBE:"), "no S18R, no probe, %u Hz", gSd.clockHz); }
  { UhsOptions o; o.mode = 1; bool ok = uhsScenario ("mode 1 (probe): read-only, bus unchanged", o);
    CHECK (ok && !gModel.s18rSeen && gModel.cmd11s == 0 && gHookCalls == 0 && !gSd.signal18 && gSd.clockHz == 50000000U, "no S18R, no CMD11, %u Hz", gSd.clockHz);
    CHECK (logHas ("PROBE: SCR 02858003") && logHas ("SD_SPEC 2 SD_SPEC3 1"), "SCR decoded");
    CHECK (logHas ("PROBE: CMD6 check: group1 support 0x0003") && logHas ("PROBE: host caps"), "function table and host caps logged"); }
  { UhsOptions o; o.railOk = false; bool ok = uhsScenario ("rail change refused by the platform after CMD11: fail, no retry", o);
    CHECK (!ok && gSd.uhsCommitted && !gSd.identified, "init failed (%s)", gSd.reason);
    CHECK (strstr (gSd.reason, "power cycle") != nullptr && gModel.cmd0s == 1, "reason asks for a power cycle; CMD0 sent once (no full-reset retry)");
    CHECK (gHookCalls == 1 && !(sd32 (SDHCI_MSM_VENDOR_SPEC) & SDHCI_MSM_IO_PAD_PWR_SWITCH), "pad switch not moved without a confirmed rail"); }
  { UhsOptions o; o.keepsDatLow = true; bool ok = uhsScenario ("card keeps DAT low after the clock restart: fail, no retry", o);
    CHECK (!ok && gSd.uhsCommitted && gModel.cmd0s == 1, "init failed (%s); CMD0 sent once", gSd.reason); }
  { UhsOptions o; o.ignoreCmd11 = true; bool ok = uhsScenario ("card ignores CMD11: still a usable 3.3 V card", o);
    CHECK (ok && !gSd.uhsCommitted && !gSd.signal18 && gSd.clockHz == 50000000U && gHookCalls == 0, "continues at 3.3 V, %u Hz, rail untouched", gSd.clockHz);
    CHECK (verifyReads (), "reads fine"); }
}
#endif

#if defined(ODIN_SD_STORE_TEST) && !defined(ODIN_SD_LEGACY)
// The real store adapter on the real transport against the card model: where do the bytes land?
static bool openStore (uint64_t blocks = 262153)
{
  uint64_t first = 0; const char *why = "";
  return native_ufs_store_open (blocks, &first, &why);
}
// The store reports its outcome in the card log (STORE: lines), so the log sectors are legitimate writes; count the others.
static size_t nonLogWrites (void)
{
  size_t n = 0;
  for (auto &kv : gWritten) if (kv.first < gLogFirstLba || kv.first >= gLogFirstLba + SD_LOG_SECTORS) ++n;
  return n;
}
static void resetStoreState (void)
{
  dataRegion = {}; dataBlocks = 0; dataPoisoned = false; gStoreWrites = 0; gSdWriteWindow[1] = {};
}
static void storeScenarios (void)
{
  printf ("\n== STORE on the real transport (model card with UniXNUData at LBA %llu)\n", (unsigned long long) kDataFirst);
  freshHost (); firmwareState (true, true);
  strcpy (gBootGuid, "9dd50fa6-e20f-44d2-bf76-ddfb0e4cf42e");
  gStoreLayout = true; buildStoreGpt (gBootGuid);
  CHECK (sdCardInit (), "card init");
  gLogFirstLba = gSd.blocks - SD_LOG_RESERVED; gLogCardReady = true;
  gSdWriteWindow[0] = { gLogFirstLba, gLogFirstLba + SD_LOG_SECTORS };
  resetStoreState ();
  uint8_t page[4096], again[4096];
  for (unsigned i = 0; i < 4096; ++i) page[i] = (uint8_t) (i * 7 + 3);

  gBootMode = 0; CHECK (!openStore () && nonLogWrites () == 0, "mode 0: refused, nothing written");
  gBootMode = 1; resetStoreState (); CHECK (openStore () && gSdWriteWindow[1].end == gSdWriteWindow[1].first, "mode 1: opens for inspection with NO write window");
  CHECK (!native_ufs_store_write (3, 1, page) && nonLogWrites () == 0, "mode 1: a write is refused and reaches no sector");
  CHECK (!sdTransfer (false, kDataFirst + 24, 8, page) && nonLogWrites () == 0, "mode 1: even a direct transport write into p4 is fenced");
  CHECK (native_ufs_store_read (3, 1, again) && !memcmp (again, std::vector<uint8_t> (4096, 0).data (), 4096), "mode 1: reads of the blank partition work");
  CHECK (native_ufs_store_gpt_check () == 0, "mode 1: GPT copies re-validate");

  gBootMode = 2; resetStoreState (); gWritten.clear ();
  CHECK (openStore (), "mode 2: opens (GUID pinned, blank header)");
  CHECK (gSdWriteWindow[1].first == kDataFirst && gSdWriteWindow[1].end == kDataFirst + 262153ULL * 8, "mode 2: window = [%llu, +%llu sectors)", (unsigned long long) kDataFirst, 262153ULL * 8);
  CHECK (native_ufs_store_write (5, 1, page) && native_ufs_store_read (5, 1, again) && !memcmp (page, again, 4096), "page written through SDCC2 CMD25 and read back");
   uint8_t run[3 * 4096], runAgain[sizeof run];
   for (unsigned i = 0; i < sizeof run; ++i) run[i] = static_cast<uint8_t> (i * 7 + i / 4096 + 3);
   CHECK (native_ufs_store_write (9, 3, run) && native_ufs_store_sync () && native_ufs_store_gpt_check () == 0 &&
          native_ufs_store_read (9, 3, runAgain) && !memcmp (run, runAgain, sizeof run), "3 distinct pages written/read back, sync, GPT still valid");
  bool inside = true; size_t sectors = 0, logSectors = 0;
  for (auto &kv : gWritten) {
    bool log = kv.first >= gLogFirstLba && kv.first < gLogFirstLba + SD_LOG_SECTORS;
    if (log) { ++logSectors; continue; }
    ++sectors;
    if (kv.first < kDataFirst || kv.first >= kDataFirst + 262153ULL * 8) inside = false;
  }
  CHECK (inside && sectors == 4 * 8, "every written sector (%zu) lies inside p4's window; none in p1-p3 or either GPT", sectors);
  CHECK (logSectors > 0 && logSectors <= SD_LOG_SECTORS, "the rest (%zu) is the 32 KiB STORE log in its reserved area", logSectors);
  CHECK (gWritten.count (kDataFirst + 5 * 8) && gWritten.count (kDataFirst + 5 * 8 + 7) && !gWritten.count (kDataFirst + 4 * 8 + 7), "page 5 occupies exactly sectors 40..47 of the window");
  CHECK (!sdTransfer (false, kParts[2].first + kParts[2].count - 1, 1, page) && !sdTransfer (false, kBlocks - 1, 1, page) && !sdTransfer (false, 1, 1, page),
         "the same mode-2 session still cannot write p3's last sector, the backup GPT or the primary GPT");
  CHECK (!native_ufs_store_write (262153, 1, page) && dataPoisoned && gSdWriteWindow[1].end == gSdWriteWindow[1].first, "an out-of-window request poisons the store and closes the window");
  CHECK (!native_ufs_store_write (5, 1, page) && !sdTransfer (false, kDataFirst + 40, 8, page), "after poisoning nothing more is written, even to a valid page");

  gBootMode = 2; resetStoreState ();
  std::vector<uint8_t> junk (512, 0x33); gWritten.clear (); gWritten[kDataFirst] = junk;
  CHECK (!openStore () && gSdWriteWindow[1].end == gSdWriteWindow[1].first, "foreign data in the header block: refused, window closed");
  gWritten.clear (); resetStoreState ();
  strcpy (gBootGuid, "9dd50fa6-e20f-44d2-bf76-ddfb0e4cf42f");
  CHECK (!openStore () && nonLogWrites () == 0, "a different GUID finds no partition: refused");
  strcpy (gBootGuid, "9dd50fa6-e20f-44d2-bf76-ddfb0e4cf42e"); resetStoreState ();
  CHECK (!openStore (4194304ULL / 8 + 1), "overlay larger than the partition: refused");
  gStoreLayout = false;
}
#endif

int main (int argc, char **argv)
{
  for (int i = 1; i < argc; ++i) if (!strcmp (argv[i], "-v")) gVerbose = true;
#ifdef ODIN_SD_LEGACY
  printf ("driver under test: PRE-REVIEW host code\n");
#else
  printf ("driver under test: reviewed host code\n");
#endif
  scenario ("firmware left power on, status-enable 0", false, true, false);
  scenario ("firmware left power on, status-enable set", true, true, false);
  scenario ("firmware left power off", false, false, false);
  scenario ("adopted state unusable (clock/power dropped at ExitBootServices): full-reset fallback", true, true, true);
  scenario ("isolating the CSD parse: no PWRCTL stall, INT_ENABLE survives RESET_ALL", true, true, false, false, true);
  scenario ("card supports only default speed: clock stays 25 MHz", true, true, false, true, false, true);
  scenario ("card supports high-speed but refuses selection: stay 25 MHz", true, true, false, true, false, false, true);
#ifndef ODIN_SD_LEGACY
  freshHost (); firmwareState (true, true); CHECK (sdCardInit (), "deadline fixture init");
  uint8_t scratch[512] = {};
  gModel.suppressResponse = true;
  CHECK (!sdReadBlocks (1, 1, scratch), "missing RESPONSE is failure without a hardware timeout bit");
  gModel.suppressResponse = false; gModel.suppressDataEnd = true;
  CHECK (!sdReadBlocks (1, 1, scratch), "missing DATA_END is failure, not a completed read");
  gSdWriteWindow[1] = { 4096, 4104 };                       // inside a window, so the fence is not what refuses it
  CHECK (!sdWriteBlock (4096, scratch), "missing DATA_END cannot acknowledge a durable write");
  gSdWriteWindow[1] = {};
  unsigned commands = gModel.commands;
  CHECK (!sdReadBlocks (~0ULL, 1, scratch) && gModel.commands == commands, "overflow LBA rejected before a command");
  CHECK (!sdReadBlocks (1, 1, nullptr) && gModel.commands == commands, "null transfer buffer rejected");
  gModel.suppressDataEnd = false;

  // Write fence: only the boot log and a pinned Data range are writable; p1-p3, both GPTs and everything else are not.
  freshHost (); firmwareState (true, true); CHECK (sdCardInit (), "write-fence fixture init");
  gLogFirstLba = gSd.blocks - SD_LOG_RESERVED; gLogCardReady = true;
  gSdWriteWindow[0] = { gLogFirstLba, gLogFirstLba + SD_LOG_SECTORS };
  gSdWriteWindow[1] = {};
  uint8_t block[512 * 8]; memset (block, 0xA5, sizeof block);
  const uint64_t forbidden[] = { 0, 1, 2, 33, 2048, 821248, 12693504, 12693504 + 6451199, 19144704 - 1, kBlocks - 33, kBlocks - 1, gLogFirstLba - 1, gLogFirstLba + SD_LOG_SECTORS };
  commands = gModel.commands; size_t before = gWritten.size ();
  bool allRefused = true;
  for (uint64_t lba : forbidden) allRefused &= !sdTransfer (false, lba, 1, block);
  CHECK (allRefused && gModel.commands == commands && gWritten.size () == before, "13 writes into p1-p3/GPT/neighbours refused before any command");
  CHECK (!sdTransfer (false, gLogFirstLba + SD_LOG_SECTORS - 1, 2, block) && !sdTransfer (false, gLogFirstLba - 1, 2, block),
         "a write that straddles the log window is refused");
  CHECK (!sdTransfer (false, 0, 0, block), "zero-length write refused");
  CHECK (gSd.writesRefused >= 15 && native_sd_write_refused == gSd.writesRefused, "refusals are counted (%u)", gSd.writesRefused);
  CHECK (sdTransfer (false, gLogFirstLba, 1, block) && gWritten.count (gLogFirstLba), "the log window itself is writable");
  { // BootProgress userland snapshots: card log only, the six-line page ring keeps the driver's own last lines.
    uint32_t ring = native_sd_diag_count; size_t used = gLogUsed;
    native_sd_log_note ("UL t=30 main=1 procs=54");
    native_sd_log_note (nullptr);
    CHECK (native_sd_diag_count == ring && gLogUsed == used + strlen ("UL t=30 main=1 procs=54") + 1 &&
           strstr (gLog + SD_LOG_HEADER, "UL t=30 main=1 procs=54\n") != nullptr, "a note reaches the card log and leaves the page ring alone");
    gLogUsed = sizeof (gLog) - 4; native_sd_log_note ("does not fit");
    CHECK (gLogUsed == sizeof (gLog) - 4, "a note that does not fit is dropped, not truncated into the header area");
    gLogUsed = used; gLog[gLogUsed] = 0; }
  gSdWriteWindow[1] = { 19144704, 19144704 + 4096 };      // a pinned Data range (first 2 MiB of a would-be p4)
  CHECK (sdTransfer (false, 19144704, 8, block) && sdTransfer (false, 19144704 + 4088, 8, block), "Data window writable at both ends (CMD25 x8)");
  CHECK (!sdTransfer (false, 19144704 + 4089, 8, block) && !sdTransfer (false, 19144703, 1, block), "Data window refuses one sector over either edge");
  // Throughput accounting: bytes and microseconds are counted per direction, only for transfers that succeeded.
  uint64_t rb = native_sd_read_bytes, wb = native_sd_write_bytes;
  uint8_t big[512 * 16];
  CHECK (sdReadBlocks (821248, 16, big) && native_sd_read_bytes - rb == 512 * 16, "read accounting counts 8192 B");
  CHECK (sdTransfer (false, 19144704, 8, block) && native_sd_write_bytes - wb == 4096, "write accounting counts 4096 B");
  CHECK (native_sd_read_us > 0 || gMicros > 0, "time inside transfers is measured");
#endif
#ifndef ODIN_SD_LEGACY
  uhsScenarios ();
  {
    printf ("\n== DMA: ADMA2-64 reads and one log write, no FIFO\n");
    freshHost (); firmwareState (true, true);
    gModel.set32 (0x40, 0x04000000U | SDHCI_CAN_DO_ADMA2 | SDHCI_CAN_64BIT);
    gSdDmaRequest = 1;
    CHECK (sdCardInit () && logHas ("DMA: ADMA2-64 selected"), "init selects ADMA2-64");
    unsigned fifo = gModel.bufferReads;
     CHECK (verifyReads () && gModel.dataFaults == 0 && gModel.bufferReads == fifo, "data reads match and do not touch the FIFO (%u buffer reads)", gModel.bufferReads - fifo);
     // Host-only chain qualification; this does NOT raise the 128-sector device read limit.
     // Linux sdhci.h:377-395: legacy ADMA64 is 12 bytes; v4 mode selects 16.
     static_assert (sizeof (SdAdma64) == 12 && offsetof (SdAdma64, len) == 2 &&
                    offsetof (SdAdma64, addrLo) == 4 && offsetof (SdAdma64, addrHi) == 8,
                    "legacy ADMA2-64 wire layout");
     const unsigned chainBlocks[] = { 1, 127, 128, 129, 256, 512 };
     for (unsigned n : chainBlocks) {
       std::vector<uint8_t> chain (n * 512);
       bool match = sdTransfer (true, 100000, n, chain.data ());
       uint8_t expected[512];
       for (unsigned b = 0; match && b < n; ++b) {
         sector (100000 + b, expected);
         match = !memcmp (chain.data () + b * 512, expected, 512);
       }
       CHECK (match, "host-only ADMA chain %u sectors (%u descriptors) matches", n, (n + 127) / 128);
     }
    gLogFirstLba = gSd.blocks - SD_LOG_RESERVED; gLogCardReady = true;
    gSdWriteWindow[0] = { gLogFirstLba, gLogFirstLba + SD_LOG_SECTORS };
    uint8_t block[512]; memset (block, 0x5A, sizeof block);
     CHECK (sdTransfer (false, gLogFirstLba, 1, block) && gWritten.count (gLogFirstLba) && gWritten[gLogFirstLba][0] == 0x5A, "DMA write lands in the log window");
     gModel.dataFaults = 0;
     gAdmaMutate = 1;
    uint8_t one[512];
    CHECK (!sdReadBlocks (0, 1, one) && gModel.dataFaults == 1, "a descriptor without END fails and is not retried by PIO");
    gAdmaMutate = 2; gModel.dataFaults = 0;
    CHECK (!sdReadBlocks (0, 1, one) && gModel.dataFaults == 1, "a descriptor length of 1 byte fails");
    gAdmaMutate = 0; gModel.dataFaults = 0;
    CHECK (sdReadBlocks (0, 1, one), "the next transfer with a valid table still works");
  }
  {
    printf ("\n== DMA: requested, controller has no ADMA2: stay on PIO\n");
    freshHost (); firmwareState (true, true);
    gSdDmaRequest = 1;
    CHECK (sdCardInit () && logHas ("lack ADMA2-64") && verifyReads (), "missing capability keeps the PIO path");
  }
#endif
#if defined(ODIN_SD_STORE_TEST) && !defined(ODIN_SD_LEGACY)
  storeScenarios ();
#endif
  printf ("\n%s: %u failure(s)\n", gFailures ? "FAIL" : "PASS", gFailures);
  return gFailures ? 1 : 0;
}
