// SPDX-License-Identifier: BSD-2-Clause-Patent
// SdHost.h - SDHCI v5 / Qualcomm SDCC2 host side of OdinSd: boot log, controller access, SD card identification,
// PIO transfers and GPT parsing. The host test (ODIN_SD_TEST) compiles this file with no IOKit.
// The device build uses one buffer inside the kernel image. ml_static_vtop is not used: it panics
// outside a 1 MiB window. ml_vtophys walks the live tables and returns the physical address
// (mmu_kvtop, AT S1E1R). ADMA is programmed only when that address sits in 32-bit DRAM.
#ifndef ODIN_SD_HOST_H
#define ODIN_SD_HOST_H

#include "GccSdcc.h"
#include "SdhciRegs.h"
#include "StackFold.h"
// ---------------------------------------------------------------- boot log on the card

#define SD_LOG_BYTES       (32 * 1024)
#define SD_LOG_SECTORS     (SD_LOG_BYTES / 512)
#define SD_LOG_MAGIC       "XNSDLOG1"
#define SD_LOG_HEADER      32
#define SD_LOG_RESERVED    8192          // sectors kept clear of the backup GPT at the end of the card

static char gLog[SD_LOG_BYTES];
static size_t gLogUsed = SD_LOG_HEADER;
static uint32_t gLogSeq = 0;
static bool gLogCardReady = false;
static bool gLogBusy = false;
static uint64_t gLogFirstLba = 0;

// The last lines of the log are also kept in a small ring the boot-progress panic page prints (BootProgress.c,
// "SD DRIVER"): the card log needs the card, the page does not.
#define SD_DIAG_LINES 6
#define SD_DIAG_COLS  88
extern "C" {
char native_sd_diag[SD_DIAG_LINES][SD_DIAG_COLS];
volatile uint32_t native_sd_diag_count = 0;
volatile uint32_t native_sd_probe_calls = 0;
// Throughput accounting of the PIO transport (shown by BootProgress): bytes moved and microseconds spent inside
// sdTransfer(), so two photos of the page give a measured MiB/s instead of an estimate.
volatile uint64_t native_sd_read_bytes = 0;
volatile uint64_t native_sd_read_us = 0;
volatile uint64_t native_sd_write_bytes = 0;
volatile uint64_t native_sd_write_us = 0;
volatile uint32_t native_sd_write_refused = 0;
// Stack margin probe (changes sd-stack-probe, mount-panic): bytes still free on the kernel stack when each point of the
// chain ran; the smallest value seen wins (StackFold.h). SD_STACK_UNREACHED means the point never ran; a negative value
// means the SP was already below the stack base (guard page). BootProgress prints them on the panic page.
//   0 OdinSdWindow::doAsyncReadWrite  1 readVerified entry   2 readVerified per block  3 sdReadBlocks
//   4 sdTransfer                      5 sdTransferCommand    6 after sdProgramAdma     7 sdDmaPrepare
//   8 native_ufs_store_read           9 native_ufs_store_write (frame holds readback[4096])
//  10 sdCommand                      11 sdWaitBits (poll loop: where a timer AST most likely finds the thread)
enum { kSdStackSites = 12 };
volatile int64_t native_sd_stack_min[kSdStackSites] = {
  SD_STACK_UNREACHED, SD_STACK_UNREACHED, SD_STACK_UNREACHED, SD_STACK_UNREACHED,
  SD_STACK_UNREACHED, SD_STACK_UNREACHED, SD_STACK_UNREACHED, SD_STACK_UNREACHED,
  SD_STACK_UNREACHED, SD_STACK_UNREACHED, SD_STACK_UNREACHED, SD_STACK_UNREACHED };
}
#ifndef ODIN_SD_TEST
#include <libkern/OSDebug.h>
#endif
// Read-only observation: no allocation, no lock, no branch that changes the transport. Not atomic: two threads can lose
// each other's minimum, never invent one.
static inline void sdStackMark (unsigned site)
{
#ifdef ODIN_SD_TEST
  (void)site;
#else
  if (site >= kSdStackSites) return;
  native_sd_stack_min[site] = sdStackFold (native_sd_stack_min[site], static_cast<int64_t> (OSKernelStackRemaining ()));
#endif
}

#ifdef ODIN_SD_TEST
extern "C" uint64_t odinSdTestNowUs (void);
static inline uint64_t sdNowUs (void) { return odinSdTestNowUs (); }
#else
static inline uint64_t sdNowUs (void)
{
  uint64_t ns = 0;
  absolutetime_to_nanoseconds (mach_absolute_time (), &ns);
  return ns / 1000;
}
#endif

// Write fence of the transport. The card also holds p1-p3 (ESP, OS, SystemOS); nothing may reach them from here.
// Only two ranges are ever writable: the 32 KiB boot log (set by OdinSd.cpp) and the Data partition found and pinned
// by the store (native_ufs_store_open). sdTransfer() refuses every other write, whoever asks.
struct SdWriteWindow {
  uint64_t first = 0;
  uint64_t end = 0;            // exclusive
};
static SdWriteWindow gSdWriteWindow[2];   // [0] boot log, [1] Data store

static bool sdWriteAllowed (uint64_t lba, uint32_t count)
{
  if (!count) return false;
  for (const SdWriteWindow &window : gSdWriteWindow) {
    if (window.end > window.first && lba >= window.first && lba < window.end && count <= window.end - lba) return true;
  }
  return false;
}

// Card log only: no ring slot. Same unlocked append as before, only moved out of sdLogText.
static void sdLogAppend (const char *text, size_t length)
{
  if (gLogUsed + length + 2 >= sizeof (gLog)) return;
  memcpy (gLog + gLogUsed, text, length);
  gLogUsed += length;
  gLog[gLogUsed++] = '\n';
  gLog[gLogUsed] = 0;
}

static void sdLogText (const char *text)
{
  if (!text) return;
  size_t length = strlen (text);
  uint32_t slot = native_sd_diag_count % SD_DIAG_LINES;
  size_t keep = length < SD_DIAG_COLS - 1 ? length : SD_DIAG_COLS - 1;
  memcpy (native_sd_diag[slot], text, keep);
  native_sd_diag[slot][keep] = 0;
  native_sd_diag_count = native_sd_diag_count + 1;
  sdLogAppend (text, length);
}

struct SdState;
static bool sdFlushLog (void);

// Other drivers may append to the same card log (NativeOdinPanel does). Same buffer, same flush points.
extern "C" void native_sd_log_text (const char *text)
{
  sdLogText (text);
}

extern "C" bool native_sd_log_flush (void)
{
  return sdFlushLog ();
}

// A line for the card log that must not displace the driver's own last lines on the panic page (the six-line ring):
// BootProgress's userland snapshots (odin-userland-log) use this one. Flush with native_sd_log_flush.
extern "C" void native_sd_log_note (const char *text)
{
  if (text) sdLogAppend (text, strlen (text));
}

#ifdef ODIN_SD_TEST
static inline void sdStageHook (const char *) {}
#else
extern "C" void boot_progress_stage (const char *);
// Mirrors the milestone lines into the boot-progress stage history (BootProgress.c) so they survive on the photographed page.
static void sdStageHook (const char *line)
{
  if (!line) return;
  static const char *const milestones[] = { "START:", "INIT:", "GPT:", "VERIFY:", "PUBLISH:", "PROBE:", "PWRCTL:", "IDENT: OCR", "IDENT: CSD", "IDENT: status", "RAIL:", "UHS:", "CLOCK:" };
  for (const char *m : milestones) {
    if (!strncmp (line, m, strlen (m))) {
      char text[80];
      snprintf (text, sizeof (text), "OdinSd %.68s", line);
      boot_progress_stage (text);
      return;
    }
  }
}
#endif

static void sdLogf (const char *format, ...) __attribute__ ((format (printf, 1, 2)));

static void sdLogf (const char *format, ...)
{
  char line[256];
  va_list arguments;
  va_start (arguments, format);
  vsnprintf (line, sizeof (line), format, arguments);
  va_end (arguments);
  sdLogText (line);
  IOLog ("OdinSd: %s\n", line);
  sdStageHook (line);
}

// ---------------------------------------------------------------- controller state

struct SdState {
  volatile uintptr_t base = 0;
  volatile uintptr_t gcc = 0;
  IOLock *lock = nullptr;
  bool mapped = false;
  bool identified = false;
  bool highCapacity = false;
  bool fourBit = false;
  bool started = false;
  uint32_t rca = 0;
  uint32_t ocr = 0;
  uint32_t csd[4] = {};
  uint64_t blocks = 0;
  uint32_t clockHz = 0;
  uint32_t lastStatus = 0;
  uint32_t cmdErrors = 0;
  uint32_t dataErrors = 0;
  uint32_t timeouts = 0;
  uint32_t writesRefused = 0;
  bool signal18 = false;       // the card was told CMD11 and the I/O rail is at 1.8 V
  bool uhsCommitted = false;   // CMD11 was answered: only a power cycle returns the card to 3.3 V signalling
  uint32_t scr[2] = {};
  char speed[24] = {};
  char reason[96] = {};
};

static SdState gSd;

// UHS-I stage. Default 0 = never: 3.3 V, 50 MHz high-speed, exactly the behaviour measured so far.
//   1 = probe only: read the SCR and the CMD6 function table and log them; nothing about the bus changes.
//   2 = attempt SDR50 (1.8 V, 100 MHz): needs the platform to move the card's I/O rail (gSdSignalHook).
// OdinSd.cpp sets both from the boot argument odin-sd-uhs and from what it could resolve of the PMIC rail.
static unsigned gSdUhsMode = 0;
// 1 selects ADMA2-64 for data transfers when CAPABILITIES announces it. 0 keeps the PIO loop.
// OdinSd.cpp sets this from odin-sd-dma. The default stays 0: the measured boot does not use DMA.
static unsigned gSdDmaRequest = 0;
#ifdef ODIN_SD_TEST
extern unsigned gAdmaMutate;
#endif
static bool (*gSdSignalHook) (bool low18) = nullptr;   // true = the platform moved the I/O rail and confirmed it
static bool gSdIoArmed = false;                         // the hook runs only while the voltage switch below is in progress

#ifdef ODIN_SD_TEST
extern "C" uint32_t odinSdTestRead (unsigned offset, unsigned width);
extern "C" void odinSdTestWrite (unsigned offset, unsigned width, uint32_t value);
static inline uint8_t sd8 (unsigned offset) { return static_cast<uint8_t> (odinSdTestRead (offset, 1)); }
static inline uint16_t sd16 (unsigned offset) { return static_cast<uint16_t> (odinSdTestRead (offset, 2)); }
static inline uint32_t sd32 (unsigned offset) { return odinSdTestRead (offset, 4); }
static inline void sdWrite8 (unsigned offset, uint8_t value) { odinSdTestWrite (offset, 1, value); }
static inline void sdWrite16 (unsigned offset, uint16_t value) { odinSdTestWrite (offset, 2, value); }
static inline void sdWrite32 (unsigned offset, uint32_t value) { odinSdTestWrite (offset, 4, value); }
#else
static inline uint8_t sd8 (unsigned offset)
{
  return *reinterpret_cast<volatile uint8_t *> (gSd.base + offset);
}

static inline uint16_t sd16 (unsigned offset)
{
  return *reinterpret_cast<volatile uint16_t *> (gSd.base + offset);
}

static inline uint32_t sd32 (unsigned offset)
{
  return *reinterpret_cast<volatile uint32_t *> (gSd.base + offset);
}

static inline void sdWrite8 (unsigned offset, uint8_t value)
{
  *reinterpret_cast<volatile uint8_t *> (gSd.base + offset) = value;
  __asm__ volatile ("dsb sy" ::: "memory");
}

static inline void sdWrite16 (unsigned offset, uint16_t value)
{
  *reinterpret_cast<volatile uint16_t *> (gSd.base + offset) = value;
  __asm__ volatile ("dsb sy" ::: "memory");
}

static inline void sdWrite32 (unsigned offset, uint32_t value)
{
  *reinterpret_cast<volatile uint32_t *> (gSd.base + offset) = value;
  __asm__ volatile ("dsb sy" ::: "memory");
}
#endif

// Polls the combined interrupt status (normal in the low half, error in the high half).
static uint32_t sdWaitInterrupt (uint32_t wanted, uint32_t timeoutMs, const char *step)
{
  for (uint32_t elapsed = 0; elapsed <= timeoutMs; ++elapsed) {
    uint32_t status = sd32 (SDHCI_INT_STATUS);
    gSd.lastStatus = status;
    if (status & SDHCI_INT_ERROR) {
      gSd.cmdErrors += (status & SDHCI_INT_CMD_ERRORS) ? 1 : 0;
      gSd.dataErrors += (status & SDHCI_INT_DATA_ERRORS) ? 1 : 0;
      sdLogf ("%s: error status 0x%08x", step, status);
      return status;
    }
    if (status & wanted) return status;
    IODelay (1000);
  }
  gSd.timeouts++;
  sdLogf ("%s: timeout after %u ms (status 0x%08x)", step, timeoutMs, sd32 (SDHCI_INT_STATUS));
  return sd32 (SDHCI_INT_STATUS);
}

static bool sdSoftReset (uint8_t bits, const char *step)
{
  sdWrite8 (SDHCI_SOFTWARE_RESET, bits);
  for (unsigned spin = 0; spin < 1000; ++spin) {
    if (!(sd8 (SDHCI_SOFTWARE_RESET) & bits)) return true;
    IODelay (100);
  }
  sdLogf ("%s: reset bits 0x%x never cleared", step, bits);
  return false;
}

static void sdClearInterrupts (void)
{
  sdWrite32 (SDHCI_INT_STATUS, 0xFFFFFFFF);
}

// Programmes the SDCC2 root generator and then the core's own clock gate. `plan` carries the encoded
// frequency selection; the caller logs the readback so a mismatch is visible in the boot log.
static bool sdSetClock (const SdccClockPlan &plan)
{
  sdWrite16 (SDHCI_CLOCK_CONTROL, 0);
  IODelay (1000);
  uint32_t cfg = gccProgramSdcc2 (gSd.gcc, plan);
  if ((cfg & (GCC_CFG_SRC_DIV_MASK | (0x7U << GCC_CFG_SRC_SEL_SHIFT) | GCC_CFG_MODE_MASK)) !=
      (plan.cfg & (GCC_CFG_SRC_DIV_MASK | (0x7U << GCC_CFG_SRC_SEL_SHIFT) | GCC_CFG_MODE_MASK))) {
    sdLogf ("clock %u Hz: GCC cfg read back 0x%08x, wanted 0x%08x", plan.hz, cfg, plan.cfg);
  }
  sdWrite16 (SDHCI_CLOCK_CONTROL, SDHCI_CLOCK_INT_EN);
  for (unsigned spin = 0; spin < 1000; ++spin) {
    if (sd16 (SDHCI_CLOCK_CONTROL) & SDHCI_CLOCK_INT_STABLE) break;
    IODelay (100);
  }
  if (!(sd16 (SDHCI_CLOCK_CONTROL) & SDHCI_CLOCK_INT_STABLE)) {
    sdLogf ("clock %u Hz: internal clock never stabilised (0x%04x)", plan.hz, sd16 (SDHCI_CLOCK_CONTROL));
    return false;
  }
  sdWrite16 (SDHCI_CLOCK_CONTROL, SDHCI_CLOCK_INT_EN | SDHCI_CLOCK_INT_STABLE | SDHCI_CLOCK_CARD_EN);
  gSd.clockHz = plan.hz;
  return true;
}

// One command. `response` receives the four SDHCI response words, most significant first (as mmc's
// cmd->response[] orders a 136-bit answer). Returns the combined interrupt status, with bit 0 set when
// the command completed without error.
static uint32_t sdCommand (uint8_t index, uint32_t argument, uint8_t responseType, bool crcCheck,
                           bool indexCheck, bool dataPresent, uint32_t timeoutMs, uint32_t response[4])
{
  sdStackMark (10);
  if (response) for (unsigned i = 0; i < 4; ++i) response[i] = 0;
  for (unsigned spin = 0; spin < 1000; ++spin) {
    if (!(sd32 (SDHCI_PRESENT_STATE) & (SDHCI_CMD_INHIBIT | SDHCI_DATA_INHIBIT))) break;
    IODelay (100);
  }
  sdClearInterrupts ();
  // sdhci_set_transfer_mode: a command without data must not inherit multi-block / auto-CMD12 bits of the previous transfer.
  if (!dataPresent) sdWrite16 (SDHCI_TRANSFER_MODE, 0);
  sdWrite32 (SDHCI_ARGUMENT, argument);
  uint16_t flags = responseType;
  if (crcCheck) flags |= SDHCI_CMD_CRC;
  if (indexCheck) flags |= SDHCI_CMD_INDEX;
  if (dataPresent) flags |= SDHCI_CMD_DATA;
  sdWrite16 (SDHCI_COMMAND, static_cast<uint16_t> ((index << 8) | flags));
  uint32_t status = sdWaitInterrupt (SDHCI_INT_RESPONSE, timeoutMs, "command");
  // A software deadline need not latch a hardware timeout bit. Never turn a
  // zero status into successful command completion (especially for writes).
  if (!(status & SDHCI_INT_RESPONSE)) status |= SDHCI_INT_ERROR | SDHCI_INT_TIMEOUT;
  if (responseType == SDHCI_CMD_RESP_SHORT_BUSY && (status & SDHCI_INT_RESPONSE) && !(status & SDHCI_INT_ERROR)) {
    // R1b: the card holds DAT0 low while busy; the host reports it as transfer complete.
    uint32_t busy = sdWaitInterrupt (SDHCI_INT_DATA_END, timeoutMs, "busy");
    status |= busy;
    if (!(busy & SDHCI_INT_DATA_END)) status |= SDHCI_INT_ERROR | SDHCI_INT_TIMEOUT;
  }
  if (response && !(status & (SDHCI_INT_ERROR | SDHCI_INT_TIMEOUT))) {
    if (responseType == SDHCI_CMD_RESP_LONG) {
      response[0] = sd32 (SDHCI_RESPONSE + 12);
      response[1] = sd32 (SDHCI_RESPONSE + 8);
      response[2] = sd32 (SDHCI_RESPONSE + 4);
      response[3] = sd32 (SDHCI_RESPONSE + 0);
      // The controller strips the CRC byte of a 136-bit response, so the register image is the answer shifted right by
      // eight bits. sdhci_read_rsp_136 (sdhci.c:1786-1791) shifts it back; without this every CSD field is read one byte off.
      for (unsigned i = 0; i < 4; ++i) {
        response[i] <<= 8;
        if (i != 3) response[i] |= response[i + 1] >> 24;
      }
    } else if (responseType != SDHCI_CMD_RESP_NONE) {
      response[3] = sd32 (SDHCI_RESPONSE);
    }
  }
  if (status & SDHCI_INT_ERROR) {
    sdClearInterrupts ();
    sdSoftReset (SDHCI_RESET_CMD, "command reset");
  }
  return status;
}

static inline uint8_t responseByte (const uint32_t response[4], unsigned at)
{
  return static_cast<uint8_t> ((response[at / 4] >> (8 * (3 - (at % 4)))) & 0xFF);
}

static void fifoRead (uint8_t *out, unsigned words)
{
  for (unsigned word = 0; word < words; ++word) {
    uint32_t value = sd32 (SDHCI_BUFFER);
    out[word * 4 + 0] = static_cast<uint8_t> (value);
    out[word * 4 + 1] = static_cast<uint8_t> (value >> 8);
    out[word * 4 + 2] = static_cast<uint8_t> (value >> 16);
    out[word * 4 + 3] = static_cast<uint8_t> (value >> 24);
  }
}

static void fifoWrite (const uint8_t *in, unsigned words)
{
  for (unsigned word = 0; word < words; ++word) {
    uint32_t value = static_cast<uint32_t> (in[word * 4 + 0]) |
                     (static_cast<uint32_t> (in[word * 4 + 1]) << 8) |
                     (static_cast<uint32_t> (in[word * 4 + 2]) << 16) |
                     (static_cast<uint32_t> (in[word * 4 + 3]) << 24);
    sdWrite32 (SDHCI_BUFFER, value);
  }
}

static uint32_t cardAddress (uint64_t lba)
{
  return gSd.highCapacity ? static_cast<uint32_t> (lba) : static_cast<uint32_t> (lba * 512);
}

// Waits for `wanted` in the latched interrupt status (it only latches because SDHCI_INT_ENABLE is written). Spins
// briefly and then polls every 10 us: a 512-byte block needs ~40 us at 25 MHz and ~10 ms at 400 kHz.
static uint32_t sdWaitBits (uint32_t wanted, uint32_t timeoutMs, const char *step)
{
  sdStackMark (11);
  for (uint32_t spin = 0; spin < timeoutMs * 100 + 64; ++spin) {
    uint32_t status = sd32 (SDHCI_INT_STATUS);
    gSd.lastStatus = status;
    if (status & SDHCI_INT_ERROR) {
      gSd.cmdErrors += (status & SDHCI_INT_CMD_ERRORS) ? 1 : 0;
      gSd.dataErrors += (status & SDHCI_INT_DATA_ERRORS) ? 1 : 0;
      sdLogf ("%s: error status 0x%08x", step, status);
      return status;
    }
    if (status & wanted) return status;
    if (spin >= 64) IODelay (10);
  }
  gSd.timeouts++;
  sdLogf ("%s: timeout (status 0x%08x, present 0x%08x)", step, sd32 (SDHCI_INT_STATUS), sd32 (SDHCI_PRESENT_STATE));
  return sd32 (SDHCI_INT_STATUS);
}

// Waits for a buffer-ready bit of PRESENT_STATE. Unlike the interrupt status, these bits are
// self-clearing: buffer-read-enable drops as the FIFO is drained and rises again with the next block,
// which is what a polling PIO loop needs. Any error interrupt ends the wait.
static bool sdWaitPresent (uint32_t mask, uint32_t timeoutMs, const char *step)
{
  // 10 us per poll: a 512-byte block needs ~40 us at 25 MHz and ~10 ms at 400 kHz, so this keeps a
  // healthy buffer without spinning hard.
  for (uint32_t spin = 0; spin < timeoutMs * 100; ++spin) {
    uint32_t status = sd32 (SDHCI_INT_STATUS);
    gSd.lastStatus = status;
    if (status & SDHCI_INT_ERROR) {
      gSd.cmdErrors += (status & SDHCI_INT_CMD_ERRORS) ? 1 : 0;
      gSd.dataErrors += (status & SDHCI_INT_DATA_ERRORS) ? 1 : 0;
      sdLogf ("%s: error status 0x%08x", step, status);
      return false;
    }
    if (sd32 (SDHCI_PRESENT_STATE) & mask) return true;
    IODelay (10);
  }
  gSd.timeouts++;
  sdLogf ("%s: timeout (status 0x%08x, present 0x%08x)", step, sd32 (SDHCI_INT_STATUS),
          sd32 (SDHCI_PRESENT_STATE));
  return false;
}

static void sdAbortData (const char *step)
{
  sdLogf ("%s: aborting the data transfer", step);
  sdSoftReset (SDHCI_RESET_DATA, "data reset");
  sdCommand (12, 0, SDHCI_CMD_RESP_SHORT_BUSY, true, true, false, 500, nullptr);
  sdClearInterrupts ();
}

static bool sdTransferCommand (bool read, uint64_t lba, uint32_t count, uint8_t *data, bool *usedDma);

// ADMA2 64-bit descriptor (Linux sdhci.h, struct sdhci_adma2_64_desc). Length 0 means 65536 bytes.
struct SdAdma64 {
  uint16_t attr;
  uint16_t len;
  uint32_t addrLo;
  uint32_t addrHi;
};
static SdAdma64 gAdma[8] __attribute__((aligned(8)));
static bool gAdmaNoted = false;

// One sdReadBlocks chunk (512 blocks) plus a page for the descriptor table.
static const uint32_t kDmaPayload = 512 * 512;
static const uint32_t kDmaTable = 4096;

#ifdef ODIN_SD_TEST
static uint64_t sdDmaPhys (void *pointer) { return reinterpret_cast<uintptr_t> (pointer); }
static void sdDmaClean (const void *, size_t) {}
static void sdDmaInvalidate (const void *, size_t) {}
// The model treats the caller's buffer as identity-mapped, so the test DMA writes it directly.
static bool sdDmaBind (uint8_t *data, uint32_t bytes, uint8_t **bound, SdAdma64 **table,
                       uint64_t *dataPhys, uint64_t *tablePhys)
{
  if (!data || !bytes || bytes > 8 * 65536) return false;
  *bound = data;
  *table = gAdma;
  *dataPhys = sdDmaPhys (data);
  *tablePhys = sdDmaPhys (gAdma);
  return *dataPhys && *tablePhys;
}
#else
static void sdDmaClean (const void *pointer, size_t bytes)
{
  uintptr_t address = reinterpret_cast<uintptr_t> (pointer) & ~static_cast<uintptr_t> (63);
  uintptr_t end = (reinterpret_cast<uintptr_t> (pointer) + bytes + 63) & ~static_cast<uintptr_t> (63);
  for (; address < end; address += 64) __asm__ volatile("dc cvac, %0" :: "r"(address) : "memory");
  __asm__ volatile("dsb sy" ::: "memory");
}
static void sdDmaInvalidate (const void *pointer, size_t bytes)
{
  uintptr_t address = reinterpret_cast<uintptr_t> (pointer) & ~static_cast<uintptr_t> (63);
  uintptr_t end = (reinterpret_cast<uintptr_t> (pointer) + bytes + 63) & ~static_cast<uintptr_t> (63);
  for (; address < end; address += 64) __asm__ volatile("dc ivac, %0" :: "r"(address) : "memory");
  __asm__ volatile("dsb sy" ::: "memory");
}
// One contiguous heap bounce. A static kernel buffer's ml_vtophys came back at
// 0x8c7f59000 and the boot stayed on PIO, then panicked anyway. 16 KiB is the
// ADMA size that returned the real GPT entries (sequence 15). Larger reads stay PIO.
static IOBufferMemoryDescriptor *gDmaBuf = nullptr;
static uint8_t *gDmaVirt = nullptr;
static uint64_t gDmaPhys = 0;
static int gDmaPrepared = 0;
static bool sdDmaPrepare (void)
{
  sdStackMark (7);
  if (gDmaPrepared) return gDmaPrepared > 0;
  gDmaPrepared = -1;
  gDmaBuf = IOBufferMemoryDescriptor::inTaskWithOptions (
    kernel_task, static_cast<IOOptionBits> (kIODirectionInOut) | static_cast<IOOptionBits> (kIOMemoryPhysicallyContiguous) |
    static_cast<IOOptionBits> (kIOMemoryMapperNone),
    kDmaPayload + kDmaTable, 4096);
  if (!gDmaBuf) {
    sdLogf ("DMA: contiguous allocation refused; PIO");
    return false;
  }
  gDmaVirt = static_cast<uint8_t *> (gDmaBuf->getBytesNoCopy ());
  IOByteCount contiguous = 0;
  gDmaPhys = gDmaBuf->getPhysicalSegment (0, &contiguous, kIOMemoryMapperNone);
  if (!gDmaVirt || !gDmaPhys || contiguous < kDmaPayload + kDmaTable) {
    sdLogf ("DMA: segment phys 0x%llx contiguous %llu; PIO", gDmaPhys, static_cast<unsigned long long> (contiguous));
    gDmaVirt = nullptr;
    gDmaPhys = 0;
    return false;
  }
  gDmaPrepared = 1;
  sdLogf ("DMA: bounce phys 0x%llx bytes %u cap 16KiB", gDmaPhys, kDmaPayload);
  return true;
}
static bool sdDmaBind (uint8_t *, uint32_t bytes, uint8_t **bound, SdAdma64 **table,
                       uint64_t *dataPhys, uint64_t *tablePhys)
{
  if (!bytes || bytes > kDmaPayload || !sdDmaPrepare ()) return false;
  *bound = gDmaVirt;
  *table = reinterpret_cast<SdAdma64 *> (gDmaVirt + kDmaPayload);
  *dataPhys = gDmaPhys;
  *tablePhys = gDmaPhys + kDmaPayload;
  return true;
}
#endif

static bool sdAdmaUsable (void)
{
  if (gSdDmaRequest != 1) return false;
  uint32_t caps = sd32 (SDHCI_CAPABILITIES);
  if ((caps & SDHCI_CAN_DO_ADMA2) && (caps & SDHCI_CAN_64BIT)) {
    if (!gAdmaNoted) { sdLogf ("DMA: ADMA2-64 selected"); gAdmaNoted = true; }
    return true;
  }
  if (!gAdmaNoted) {
    sdLogf ("DMA: odin-sd-dma=1 but capabilities 0x%08x lack ADMA2-64; PIO", caps);
    gAdmaNoted = true;
  }
  return false;
}

// Fills `table` for `bytes` at `dataPhys` and points the controller at `tablePhys`.
// A failure here does not issue the command. `data` is the CPU view of the same bytes (the bounce on device).
static bool sdProgramAdma (uint8_t *data, uint32_t bytes, uint64_t dataPhys, SdAdma64 *table, uint64_t tablePhys)
{
  if (!data || !table || !dataPhys || !tablePhys) return false;
  unsigned count = 0;
  uint32_t done = 0;
  memset (table, 0, 8 * sizeof (SdAdma64));
  while (done < bytes) {
    if (count >= 8) return false;
    uint32_t piece = bytes - done > 65536 ? 65536 : bytes - done;
    uint64_t physical = dataPhys + done;
    table[count].attr = 0x21;                                 // VALID | ACT_TRAN
    if (done + piece == bytes) table[count].attr |= 0x2;      // END
    table[count].len = piece == 65536 ? 0 : static_cast<uint16_t> (piece);
    table[count].addrLo = static_cast<uint32_t> (physical);
    table[count].addrHi = static_cast<uint32_t> (physical >> 32);
    ++count;
    done += piece;
  }
#ifdef ODIN_SD_TEST
  if (gAdmaMutate == 1) table[count - 1].attr = static_cast<uint16_t> (table[count - 1].attr & ~0x2);
  if (gAdmaMutate == 2) table[count - 1].len = 1;
#endif
  sdDmaClean (table, 8 * sizeof (SdAdma64));
  sdDmaClean (data, bytes);
  sdWrite32 (SDHCI_ADMA_ADDRESS, static_cast<uint32_t> (tablePhys));
  sdWrite32 (SDHCI_ADMA_ADDRESS_HI, static_cast<uint32_t> (tablePhys >> 32));
  uint8_t control = sd8 (SDHCI_HOST_CONTROL);
  sdWrite8 (SDHCI_HOST_CONTROL, static_cast<uint8_t> ((control & ~SDHCI_CTRL_DMA_MASK) | SDHCI_CTRL_ADMA64));
  return true;
}

// PIO transfer of `count` 512-byte blocks. `read` selects CMD18/CMD17 or CMD25/CMD24. Writes pass the write fence.
static bool sdTransfer (bool read, uint64_t lba, uint32_t count, uint8_t *data)
{
  sdStackMark (4);
  if (!count || count > 0xFFFF) return false;
  if (!data || lba >= gSd.blocks || count > gSd.blocks - lba ||
      lba > UINT32_MAX || count - 1 > UINT32_MAX - lba) return false;
  if (!read && !sdWriteAllowed (lba, count)) {
    gSd.writesRefused++;
    native_sd_write_refused = gSd.writesRefused;
    sdLogf ("WRITE refused: LBA %llu +%u is outside the log and the Data store", lba, count);
    return false;
  }
  uint64_t began = sdNowUs ();
  bool usedDma = false;
  bool ok = sdTransferCommand (read, lba, count, data, &usedDma);
#ifndef ODIN_SD_TEST
  // An ADMA fault must not take the root down with it. The command has been aborted; the rest of the boot is PIO.
  if (!ok && usedDma) {
    uint8_t control = sd8 (SDHCI_HOST_CONTROL);
    sdWrite8 (SDHCI_HOST_CONTROL, static_cast<uint8_t> (control & ~SDHCI_CTRL_DMA_MASK));
    sdLogf ("DMA: transfer failed (status 0x%08x); PIO for the rest of this boot", gSd.lastStatus);
    gSdDmaRequest = 0;
    ok = sdTransferCommand (read, lba, count, data, &usedDma);
  }
#endif
  uint64_t spent = sdNowUs () - began;
  if (read) {
    native_sd_read_us += spent;
    if (ok) native_sd_read_bytes += static_cast<uint64_t> (count) * 512;
  } else {
    native_sd_write_us += spent;
    if (ok) native_sd_write_bytes += static_cast<uint64_t> (count) * 512;
  }
  return ok;
}

static bool sdTransferCommand (bool read, uint64_t lba, uint32_t count, uint8_t *data, bool *usedDma)
{
  sdStackMark (5);
  if (usedDma) *usedDma = false;
  bool dma = sdAdmaUsable ();
  uint8_t *bound = data;
  SdAdma64 *table = gAdma;
  uint64_t dataPhys = 0, tablePhys = 0;
  if (dma) {
    bool programmed = sdDmaBind (data, count * 512, &bound, &table, &dataPhys, &tablePhys);
    if (programmed && !read && bound != data) memcpy (bound, data, static_cast<size_t> (count) * 512);
    if (!programmed || !sdProgramAdma (bound, count * 512, dataPhys, table, tablePhys)) {
      dma = false;
#ifndef ODIN_SD_TEST
      uint8_t control = sd8 (SDHCI_HOST_CONTROL);
      sdWrite8 (SDHCI_HOST_CONTROL, static_cast<uint8_t> (control & ~SDHCI_CTRL_DMA_MASK));
      sdLogf ("DMA: not programmed; PIO");
      gSdDmaRequest = 0;
#endif
    } else {
      sdStackMark (6);
    }
  }
  if (usedDma) *usedDma = dma;
  sdWrite16 (SDHCI_BLOCK_SIZE, 512);
  sdWrite16 (SDHCI_BLOCK_COUNT, static_cast<uint16_t> (count));
  // The Qualcomm core sets SDHCI_QUIRK_MULTIBLOCK_READ_ACMD12 (Linux sdhci-msm.c:2340): it wants the
  // controller to close a multi-block transfer itself, so AUTO_CMD12 travels in the transfer mode.
  uint16_t mode = SDHCI_TRNS_BLK_CNT_EN | (count > 1 ? (SDHCI_TRNS_MULTI | SDHCI_TRNS_AUTO_CMD12) : 0) |
                  (read ? SDHCI_TRNS_READ : 0) | (dma ? SDHCI_TRNS_DMA : 0);
  sdWrite16 (SDHCI_TRANSFER_MODE, mode);
  uint8_t index = static_cast<uint8_t> (read ? (count > 1 ? 18 : 17) : (count > 1 ? 25 : 24));
  uint32_t status = sdCommand (index, cardAddress (lba), SDHCI_CMD_RESP_SHORT, true, true, true, 500, nullptr);
  if (status & SDHCI_INT_ERROR) {
    sdWrite16 (SDHCI_BLOCK_COUNT, 0);
    if (dma) sdAbortData (read ? "adma read" : "adma write");
    return false;
  }
  if (dma) {
    uint32_t done = sdWaitInterrupt (SDHCI_INT_DATA_END, 10000, read ? "adma end" : "adma write end");
    if ((done & SDHCI_INT_ERROR) || !(done & SDHCI_INT_DATA_END)) {
      sdAbortData (read ? "adma end" : "adma write end");
      return false;
    }
    sdClearInterrupts ();
    if (read) {
      sdDmaInvalidate (bound, static_cast<size_t> (count) * 512);
      if (bound != data) memcpy (data, bound, static_cast<size_t> (count) * 512);
    }
    return true;
  }
  const uint32_t ready = read ? SDHCI_INT_DATA_AVAIL : SDHCI_INT_SPACE_AVAIL;
  for (uint32_t block = 0; block < count; ++block) {
    uint32_t seen = sdWaitBits (ready, 2000, read ? "block read" : "block write");
    if ((seen & SDHCI_INT_ERROR) || !(seen & ready)) {
      sdAbortData (read ? "block read" : "block write");
      return false;
    }
    sdWrite32 (SDHCI_INT_STATUS, ready);   // write-1-to-clear just this buffer-ready bit
    if (read) fifoRead (data + static_cast<size_t> (block) * 512, 128);
    else fifoWrite (data + static_cast<size_t> (block) * 512, 128);
  }
  uint32_t done = sdWaitInterrupt (SDHCI_INT_DATA_END, 10000, read ? "transfer end" : "write end");
  if ((done & SDHCI_INT_ERROR) || !(done & SDHCI_INT_DATA_END)) {
    sdAbortData (read ? "transfer end" : "write end");
    return false;
  }
  sdClearInterrupts ();
  return true;
}

static bool sdReadBlocks (uint64_t lba, uint32_t count, uint8_t *data)
{
  sdStackMark (3);
  while (count) {
    // 128 blocks = 64 KiB, the largest single ADMA2 descriptor (len 0). The hash path asks for
    // 1 MiB in 61440-byte pieces, so each piece is now one transfer instead of four: measured
    // 7.3 MiB/s at 16 KiB (docs/STATE.md, 2026-10-30).
    uint32_t limit = gSdDmaRequest == 1 ? 128 : 512;
    uint32_t chunk = count > limit ? limit : count;
    if (!sdTransfer (true, lba, chunk, data)) return false;
    lba += chunk;
    data += static_cast<size_t> (chunk) * 512;
    count -= chunk;
  }
  return true;
}

static bool sdWriteBlock (uint64_t lba, const uint8_t *data)
{
  return sdTransfer (false, lba, 1, const_cast<uint8_t *> (data));
}

// CMD6 (SWITCH_FUNC), group 1 only. `set` = 1 switches, 0 only checks. Support bits are status[13] (bit0 default/SDR12,
// bit1 high-speed/SDR25, bit2 SDR50, bit3 SDR104, bit4 DDR50); the selected function is status[16]'s low nibble, NOT
// another support bit. Linux sd.c @ c339f401:361,380-413 (mmc_sd_switch_hs, sd_set_bus_speed_mode).
// Sent at the host's current clock; the caller raises it only when the card selected the function.
static bool sdSwitchFunction (bool set, unsigned function, uint8_t status[64])
{
  for (unsigned i = 0; i < 64; ++i) status[i] = 0;
  sdWrite16 (SDHCI_BLOCK_SIZE, 64);
  sdWrite16 (SDHCI_BLOCK_COUNT, 1);
  sdWrite16 (SDHCI_TRANSFER_MODE, SDHCI_TRNS_BLK_CNT_EN | SDHCI_TRNS_READ);
  uint32_t command = sdCommand (6, (set ? 0x80FFFFF0U : 0x00FFFFF0U) | (function & 15U), SDHCI_CMD_RESP_SHORT, true, true, true, 1000, nullptr);
  if (command & SDHCI_INT_ERROR) {
    sdWrite16 (SDHCI_BLOCK_COUNT, 0);
    sdLogf ("SWITCH: CMD6 rejected (0x%08x)", command);
    return false;
  }
  uint32_t ready = sdWaitBits (SDHCI_INT_DATA_AVAIL, 2000, "switch status");
  if ((ready & SDHCI_INT_ERROR) || !(ready & SDHCI_INT_DATA_AVAIL)) {
    sdAbortData ("switch status");
    return false;
  }
  sdWrite32 (SDHCI_INT_STATUS, SDHCI_INT_DATA_AVAIL);
  fifoRead (status, 16);
  uint32_t done = sdWaitInterrupt (SDHCI_INT_DATA_END, 2000, "switch end");
  sdClearInterrupts ();
  if ((done & SDHCI_INT_ERROR) || !(done & SDHCI_INT_DATA_END)) {
    sdAbortData ("switch end");
    return false;
  }
  return true;
}

static bool sdSwitchHighSpeed (void)
{
  uint8_t status[64];
  if (!sdSwitchFunction (true, 1, status)) return false;
  sdLogf ("SWITCH: CMD6 group1 high-speed support %u selected %u", (status[13] >> 1) & 1U, status[16] & 15U);
  return (status[13] & 2U) && (status[16] & 15U) == 1U;
}

// ACMD51: the 8-byte SCR (SD Physical Layer spec 3.x, table 5-17). Read only.
static bool sdReadScr (uint32_t scr[2])
{
  uint8_t bytes[8] = {};
  uint32_t status = sdCommand (55, gSd.rca, SDHCI_CMD_RESP_SHORT, true, true, false, 500, nullptr);
  if (status & SDHCI_INT_ERROR) return false;
  sdWrite16 (SDHCI_BLOCK_SIZE, 8);
  sdWrite16 (SDHCI_BLOCK_COUNT, 1);
  sdWrite16 (SDHCI_TRANSFER_MODE, SDHCI_TRNS_BLK_CNT_EN | SDHCI_TRNS_READ);
  status = sdCommand (51, 0, SDHCI_CMD_RESP_SHORT, true, true, true, 500, nullptr);
  if (status & SDHCI_INT_ERROR) {
    sdWrite16 (SDHCI_BLOCK_COUNT, 0);
    return false;
  }
  uint32_t ready = sdWaitBits (SDHCI_INT_DATA_AVAIL, 2000, "scr");
  if ((ready & SDHCI_INT_ERROR) || !(ready & SDHCI_INT_DATA_AVAIL)) {
    sdAbortData ("scr");
    return false;
  }
  sdWrite32 (SDHCI_INT_STATUS, SDHCI_INT_DATA_AVAIL);
  fifoRead (bytes, 2);
  uint32_t done = sdWaitInterrupt (SDHCI_INT_DATA_END, 2000, "scr end");
  sdClearInterrupts ();
  if ((done & SDHCI_INT_ERROR) || !(done & SDHCI_INT_DATA_END)) {
    sdAbortData ("scr end");
    return false;
  }
  scr[0] = (uint32_t (bytes[0]) << 24) | (uint32_t (bytes[1]) << 16) | (uint32_t (bytes[2]) << 8) | bytes[3];
  scr[1] = (uint32_t (bytes[4]) << 24) | (uint32_t (bytes[5]) << 16) | (uint32_t (bytes[6]) << 8) | bytes[7];
  return true;
}

// ---------------------------------------------------------------- card identification

// ---- Qualcomm power-control handshake (sdhci_msm_handle_pwr_irq, sdhci-msm.c:1594-1690), done by polling.
// A write of POWER_CONTROL, of HOST_CONTROL2 bit 3 or a RESET_ALL while powered makes the core latch
// PWRCTL_STATUS and wait for PWRCTL_CTL. The regulators are the firmware's, so the acknowledgement is only the
// success bits Linux writes after its (here already satisfied) regulator work.
static uint32_t sdMsmPowerService (unsigned waitMs, const char *why)
{
  uint32_t handled = 0;
  unsigned idle = 0;
  for (unsigned guard = 0; guard < 4096; ++guard) {
    uint32_t status = sd32 (SDHCI_MSM_PWRCTL_STATUS) & 0xF;
    if (status) {
      sdWrite32 (SDHCI_MSM_PWRCTL_CLEAR, status);
      for (unsigned retry = 0; retry < 10 && (sd32 (SDHCI_MSM_PWRCTL_STATUS) & status); ++retry) {
        sdWrite32 (SDHCI_MSM_PWRCTL_CLEAR, status);
        IODelay (10);
      }
      uint32_t ack = 0;
      if (status & (SDHCI_MSM_PWRCTL_BUS_ON | SDHCI_MSM_PWRCTL_BUS_OFF)) ack |= SDHCI_MSM_PWRCTL_BUS_SUCCESS | SDHCI_MSM_PWRCTL_IO_SUCCESS;
      if (status & (SDHCI_MSM_PWRCTL_IO_LOW | SDHCI_MSM_PWRCTL_IO_HIGH)) {
        if (gSdIoArmed && gSdSignalHook) {
          // sdhci_msm_handle_pwr_irq (sdhci-msm.c:1745-1770): the driver moves vqmmc, then acknowledges, then follows the
          // real rail with CORE_IO_PAD_PWR_SWITCH. Without a confirmed rail the pad is left alone and IO_FAIL is reported.
          bool low = (status & SDHCI_MSM_PWRCTL_IO_LOW) != 0;
          bool moved = gSdSignalHook (low);
          ack |= moved ? SDHCI_MSM_PWRCTL_IO_SUCCESS : SDHCI_MSM_PWRCTL_IO_FAIL;
          if (moved) {
            uint32_t vendor = sd32 (SDHCI_MSM_VENDOR_SPEC) | SDHCI_MSM_IO_PAD_PWR_SWITCH_EN;
            vendor = low ? (vendor | SDHCI_MSM_IO_PAD_PWR_SWITCH) : (vendor & ~SDHCI_MSM_IO_PAD_PWR_SWITCH);
            sdWrite32 (SDHCI_MSM_VENDOR_SPEC, vendor);
          }
        } else {
          ack |= SDHCI_MSM_PWRCTL_IO_SUCCESS;
        }
      }
      sdWrite32 (SDHCI_MSM_PWRCTL_CTL, ack);
      sdLogf ("PWRCTL: %s status 0x%x ack 0x%x", why, status, ack);
      handled |= status;
      idle = 0;
      continue;
    }
    if (idle++ >= waitMs) break;
    IODelay (1000);
  }
  return handled;
}

// msm_hc_select_default (sdhci-msm.c:779-807): host clocking for the non-UHS modes this driver uses.
static void sdMsmSelectDefault (void)
{
  sdWrite32 (SDHCI_MSM_VENDOR_SPEC3, sd32 (SDHCI_MSM_VENDOR_SPEC3) & ~SDHCI_MSM_PWRSAVE_DLL);
  uint32_t config = sd32 (SDHCI_MSM_VENDOR_SPEC);
  config = (config & ~SDHCI_MSM_HC_MCLK_SEL_MASK) | SDHCI_MSM_HC_MCLK_SEL_DFLT;
  sdWrite32 (SDHCI_MSM_VENDOR_SPEC, config);
  config &= ~(SDHCI_MSM_HC_SELECT_IN_EN | SDHCI_MSM_HC_SELECT_IN_MASK);
  sdWrite32 (SDHCI_MSM_VENDOR_SPEC, config);
}

// Brings the host to "ready to talk at 400 kHz". `fullReset` chooses between adopting what the firmware left
// (command and data reset only: power, signalling voltage and vendor state untouched) and a RESET_ALL that
// restores the saved signalling bit and re-applies power through the handshake.
static bool sdHostSetup (bool fullReset)
{
  uint16_t control2 = sd16 (SDHCI_HOST_CONTROL2);
  uint8_t power = sd8 (SDHCI_POWER_CONTROL);
  sdLogf ("IDENT: setup %s: power 0x%02x hc2 0x%04x present 0x%08x pwrctl 0x%08x/0x%08x/0x%08x vendor 0x%08x",
          fullReset ? "full reset" : "adopt", power, control2, sd32 (SDHCI_PRESENT_STATE),
          sd32 (SDHCI_MSM_PWRCTL_STATUS), sd32 (SDHCI_MSM_PWRCTL_MASK), sd32 (SDHCI_MSM_PWRCTL_CTL), sd32 (SDHCI_MSM_VENDOR_SPEC));
  sdWrite32 (SDHCI_MSM_PWRCTL_MASK, 0xF);
  sdMsmPowerService (0, "pending");
  if (fullReset) {
    if (!sdSoftReset (SDHCI_RESET_ALL, "controller reset")) {
      strlcpy (gSd.reason, "controller reset", sizeof (gSd.reason));
      return false;
    }
    sdMsmPowerService (50, "reset");
    if (control2 & SDHCI_CTRL_VDD_180) {
      sdWrite16 (SDHCI_HOST_CONTROL2, control2 & SDHCI_CTRL_VDD_180);
      sdMsmPowerService (50, "signalling");
    }
  } else if (!sdSoftReset (SDHCI_RESET_CMD | SDHCI_RESET_DATA, "command/data reset")) {
    strlcpy (gSd.reason, "command/data reset", sizeof (gSd.reason));
    return false;
  }
  if (!(sd8 (SDHCI_POWER_CONTROL) & SDHCI_POWER_ON)) {
    if (!(power & SDHCI_POWER_ON)) power = ((control2 & SDHCI_CTRL_VDD_180) ? SDHCI_POWER_180 : SDHCI_POWER_330) | SDHCI_POWER_ON;
    sdWrite8 (SDHCI_POWER_CONTROL, power);
    sdMsmPowerService (100, "power");
  }
  sdWrite8 (SDHCI_TIMEOUT_CONTROL, 0x0E);
  sdWrite8 (SDHCI_HOST_CONTROL, 0);
  sdWrite32 (SDHCI_INT_ENABLE, SDHCI_INT_ENABLE_MASK);
  sdWrite32 (SDHCI_SIGNAL_ENABLE, 0);
  sdMsmSelectDefault ();
  sdClearInterrupts ();
  sdLogf ("IDENT: host ready: power 0x%02x hc2 0x%04x int-enable 0x%08x vendor 0x%08x",
          sd8 (SDHCI_POWER_CONTROL), sd16 (SDHCI_HOST_CONTROL2), sd32 (SDHCI_INT_ENABLE), sd32 (SDHCI_MSM_VENDOR_SPEC));
  return true;
}

struct SdWindow {
  char name[16];
  uint64_t firstLba;
  uint64_t lastLba;
};
static bool gptRead (SdWindow *windows, unsigned *count, const char **why);


// ---------------------------------------------------------------- UHS-I stage (opt-in: gSdUhsMode)

static bool sdUhsBusy (void)      // DAT[3:0] all low: the card is holding the lines (sdhci_card_busy)
{
  return !(sd32 (SDHCI_PRESENT_STATE) & SDHCI_DATA_LVL_MASK);
}

static bool sdUhsLinesIdle (void)  // at least one DAT line is high again
{
  return (sd32 (SDHCI_PRESENT_STATE) & SDHCI_DATA_LVL_MASK) != 0;
}

// Host side of the bus configuration: width, high-speed bit and the UHS timing field of HOST_CONTROL2, clock gated while
// they change (sdhci_set_ios, sdhci.c). The DLL is held in reset/power-down: at or below 100 MHz Linux does the same
// (sdhci_msm_set_uhs_signaling), the tuning DLL is only for SDR104/HS200/HS400.
static bool sdSetBusMode (unsigned uhsField, bool highSpeed, const SdccClockPlan &plan)
{
  sdWrite16 (SDHCI_CLOCK_CONTROL, SDHCI_CLOCK_INT_EN | SDHCI_CLOCK_INT_STABLE);       // card clock off
  uint32_t dll = sd32 (SDHCI_MSM_DLL_CONFIG) | SDHCI_MSM_DLL_RST;
  sdWrite32 (SDHCI_MSM_DLL_CONFIG, dll);
  sdWrite32 (SDHCI_MSM_DLL_CONFIG, dll | SDHCI_MSM_DLL_PDN);
  sdWrite8 (SDHCI_HOST_CONTROL, (gSd.fourBit ? SDHCI_CTRL_4BITBUS : 0) | (highSpeed ? SDHCI_CTRL_HISPD : 0));
  uint16_t control2 = sd16 (SDHCI_HOST_CONTROL2);
  sdWrite16 (SDHCI_HOST_CONTROL2, static_cast<uint16_t> ((control2 & ~SDHCI_CTRL_UHS_MASK) | uhsField));
  return sdSetClock (plan);
}

// Same bytes at the new speed as at the old one, over the places a stale or marginal link shows first: the GPT, the
// head of the OS window and of the SystemOS window. Every verified-root read is hashed again later; this step exists to
// decide the speed, not to be the integrity check.
struct SdProbeRegion { uint64_t lba; uint32_t sectors; };
static uint8_t gUhsReference[3][32 * 512];
static uint8_t gUhsNow[32 * 512];
static SdProbeRegion gUhsRegion[3];

static bool sdUhsSelectRegions (void)
{
  SdWindow windows[4] = {};
  unsigned count = 0;
  const char *why = "";
  if (!gptRead (windows, &count, &why)) {
    sdLogf ("UHS: GPT unreadable at the baseline speed (%s)", why);
    return false;
  }
  gUhsRegion[0] = { 0, 32 };
  unsigned used = 1;
  for (unsigned i = 0; i < count && used < 3; ++i) gUhsRegion[used++] = { windows[i].firstLba, 32 };
  for (; used < 3; ++used) gUhsRegion[used] = gUhsRegion[0];
  return true;
}

static bool sdUhsCaptureReference (void)
{
  for (unsigned i = 0; i < 3; ++i) {
    if (!sdReadBlocks (gUhsRegion[i].lba, gUhsRegion[i].sectors, gUhsReference[i])) return false;
  }
  return true;
}

static bool sdUhsVerifySpeed (const char *label)
{
  for (unsigned round = 0; round < 3; ++round) {
    for (unsigned i = 0; i < 3; ++i) {
      if (!sdReadBlocks (gUhsRegion[i].lba, gUhsRegion[i].sectors, gUhsNow) ||
          memcmp (gUhsNow, gUhsReference[i], gUhsRegion[i].sectors * 512)) {
        sdLogf ("UHS: %s read check failed (region %u, round %u)", label, i, round);
        return false;
      }
    }
  }
  return true;
}

// CMD11 and the 1.8 V switch (mmc_set_uhs_voltage, core.c; sdhci_msm_start_signal_voltage_switch). Returns:
//   0  the card refused or never accepted CMD11 -> still a normal 3.3 V card, carry on without UHS
//   1  the card and the host are at 1.8 V
//  -1  CMD11 was answered and something later failed: the card cannot be brought back without a power cycle
static int sdVoltageSwitch (void)
{
  uint32_t response[4] = {};
  uint32_t status = sdCommand (11, 0, SDHCI_CMD_RESP_SHORT, true, true, false, 500, response);
  if ((status & SDHCI_INT_ERROR) || (response[3] & 0xFFF9A000U)) {
    sdLogf ("UHS: CMD11 not accepted (0x%08x r1 0x%08x); staying at 3.3 V", status, response[3]);
    return 0;
  }
  gSd.uhsCommitted = true;
  IODelay (1500);
  if (!sdUhsBusy ()) {
    sdLogf ("UHS: card did not pull DAT[3:0] low after CMD11 (present 0x%08x)", sd32 (SDHCI_PRESENT_STATE));
    return -1;
  }
  sdWrite16 (SDHCI_CLOCK_CONTROL, SDHCI_CLOCK_INT_EN | SDHCI_CLOCK_INT_STABLE);       // spec: clock gated for 5 ms
  uint32_t vendor = sd32 (SDHCI_MSM_VENDOR_SPEC) | SDHCI_MSM_IO_PAD_PWR_SWITCH_EN;
  sdWrite32 (SDHCI_MSM_VENDOR_SPEC, vendor);
  gSdIoArmed = true;
  sdWrite16 (SDHCI_HOST_CONTROL2, sd16 (SDHCI_HOST_CONTROL2) | SDHCI_CTRL_VDD_180);
  uint32_t handled = sdMsmPowerService (50, "io-low");
  gSdIoArmed = false;
  IOSleep (10);                                                                         // Linux keeps it 10 ms, the spec says 5
  if (!(handled & SDHCI_MSM_PWRCTL_IO_LOW) || !(sd16 (SDHCI_HOST_CONTROL2) & SDHCI_CTRL_VDD_180)) {
    sdLogf ("UHS: signalling switch not acknowledged (handled 0x%x hc2 0x%04x)", handled, sd16 (SDHCI_HOST_CONTROL2));
    return -1;
  }
  sdWrite16 (SDHCI_CLOCK_CONTROL, SDHCI_CLOCK_INT_EN | SDHCI_CLOCK_INT_STABLE | SDHCI_CLOCK_CARD_EN);
  IODelay (1500);
  if (!sdUhsLinesIdle ()) {
    sdLogf ("UHS: card still holds DAT[3:0] low after the clock restarted (present 0x%08x)", sd32 (SDHCI_PRESENT_STATE));
    return -1;
  }
  gSd.signal18 = true;
  sdLogf ("UHS: signalling at 1.8 V (hc2 0x%04x vendor 0x%08x)", sd16 (SDHCI_HOST_CONTROL2), sd32 (SDHCI_MSM_VENDOR_SPEC));
  return 1;
}

// Chooses the speed after a successful 1.8 V switch, always from the safest step up and always verified by re-reading:
// SDR12 25 MHz (baseline) -> SDR25 50 MHz -> SDR50 100 MHz. A step that fails to select or to read back falls back to
// the previous one. SDR50 is skipped when the controller asks for tuning (needs the DLL; not implemented).
static bool sdUhsSpeedSelect (void)
{
  uint8_t status[64];
  strlcpy (gSd.speed, "SDR12", sizeof (gSd.speed));
  if (!sdSetBusMode (SDHCI_CTRL_UHS_SDR12, false, gSdccClock25m)) return false;
  if (!sdUhsSelectRegions () || !sdUhsCaptureReference ()) return false;
  if (!sdSwitchFunction (false, 1, status)) return false;
  unsigned support = status[13];
  uint32_t caps1 = sd32 (SDHCI_CAPABILITIES_1);
  sdLogf ("UHS: card group1 support 0x%02x (SDR25 %u SDR50 %u SDR104 %u DDR50 %u); host caps1 0x%08x%s", support,
          (support >> 1) & 1U, (support >> 2) & 1U, (support >> 3) & 1U, (support >> 4) & 1U, caps1,
          (caps1 & SDHCI_USE_SDR50_TUNING) ? " (SDR50 needs tuning)" : "");
  if (!(support & 2U)) return true;                                              // stays SDR12 at 25 MHz
  if (!sdSwitchFunction (true, 1, status) || (status[16] & 15U) != 1U ||
      !sdSetBusMode (SDHCI_CTRL_UHS_SDR25, true, gSdccClock50m) || !sdUhsVerifySpeed ("SDR25 50 MHz")) {
    sdSwitchFunction (true, 0, status);
    sdSetBusMode (SDHCI_CTRL_UHS_SDR12, false, gSdccClock25m);
    sdLogf ("UHS: SDR25 refused; back to SDR12 25 MHz");
    return sdUhsVerifySpeed ("SDR12 25 MHz");
  }
  strlcpy (gSd.speed, "SDR25", sizeof (gSd.speed));
  if (!(support & 4U) || !(caps1 & SDHCI_SUPPORT_SDR50) || (caps1 & SDHCI_USE_SDR50_TUNING)) {
    sdLogf ("UHS: SDR25 50 MHz verified; SDR50 not used");
    return true;
  }
  if (sdSwitchFunction (true, 2, status) && (status[16] & 15U) == 2U &&
      sdSetBusMode (SDHCI_CTRL_UHS_SDR50, true, gSdccClock100m) && sdUhsVerifySpeed ("SDR50 100 MHz")) {
    strlcpy (gSd.speed, "SDR50", sizeof (gSd.speed));
    sdLogf ("UHS: SDR50 100 MHz verified by three reads of three regions");
    return true;
  }
  sdLogf ("UHS: SDR50 failed; back to SDR25 50 MHz");
  sdSetBusMode (SDHCI_CTRL_UHS_SDR25, true, gSdccClock50m);
  sdSwitchFunction (true, 1, status);
  if (sdUhsVerifySpeed ("SDR25 50 MHz")) return true;
  sdSwitchFunction (true, 0, status);
  sdSetBusMode (SDHCI_CTRL_UHS_SDR12, false, gSdccClock25m);
  strlcpy (gSd.speed, "SDR12", sizeof (gSd.speed));
  return sdUhsVerifySpeed ("SDR12 25 MHz");
}

// Mode 1: log what the card and the host offer, change nothing.
static void sdUhsProbe (void)
{
  if (sdReadScr (gSd.scr)) {
    // SCR bits 63:32 are scr[0]: SD_SPEC [27:24], SD_BUS_WIDTHS [19:16], SD_SPEC3 [15], SD_SPEC4 [10], CMD_SUPPORT [1:0].
    sdLogf ("PROBE: SCR %08x%08x SD_SPEC %u SD_SPEC3 %u SD_SPEC4 %u bus widths 0x%x cmd_support 0x%x", gSd.scr[0], gSd.scr[1],
            (gSd.scr[0] >> 24) & 15U, (gSd.scr[0] >> 15) & 1U, (gSd.scr[0] >> 10) & 1U, (gSd.scr[0] >> 16) & 15U, gSd.scr[0] & 3U);
  } else {
    sdLogf ("PROBE: SCR read failed");
  }
  uint8_t status[64];
  if (sdSwitchFunction (false, 0, status)) {
    sdLogf ("PROBE: CMD6 check: group1 support 0x%02x%02x (SDR12 %u SDR25 %u SDR50 %u SDR104 %u DDR50 %u), max current %u mA",
            status[12], status[13], status[13] & 1U, (status[13] >> 1) & 1U, (status[13] >> 2) & 1U, (status[13] >> 3) & 1U,
            (status[13] >> 4) & 1U, (status[0] << 8) | status[1]);
  } else {
    sdLogf ("PROBE: CMD6 check failed");
  }
  sdLogf ("PROBE: host caps 0x%08x/0x%08x (1.8 V %u, SDR50 %u, SDR104 %u, DDR50 %u, SDR50 tuning %u) hc2 0x%04x",
          sd32 (SDHCI_CAPABILITIES), sd32 (SDHCI_CAPABILITIES_1), !!(sd32 (SDHCI_CAPABILITIES) & SDHCI_CAN_VDD_180),
          !!(sd32 (SDHCI_CAPABILITIES_1) & SDHCI_SUPPORT_SDR50), !!(sd32 (SDHCI_CAPABILITIES_1) & SDHCI_SUPPORT_SDR104),
          !!(sd32 (SDHCI_CAPABILITIES_1) & SDHCI_SUPPORT_DDR50), !!(sd32 (SDHCI_CAPABILITIES_1) & SDHCI_USE_SDR50_TUNING),
          sd16 (SDHCI_HOST_CONTROL2));
}

static bool sdCardInitMode (bool fullReset)
{
  uint32_t response[4] = {};
  gSd.identified = false;
  gSd.reason[0] = 0;
  // The branch can report "halted" only because its root generator is still parked; the root is programmed below, so a
  // halted branch is checked again after the clock plan, not refused here.
  bool branchRunning = gccEnableSdcc2 (gSd.gcc);
  sdLogf ("IDENT: GCC branch 0x%08x (%s), AHB branch 0x%08x, root cmd 0x%08x cfg 0x%08x", gccSdcc2Branch (gSd.gcc),
          branchRunning ? "running" : "halted before programming", gccSdcc2Ahb (gSd.gcc), gccSdcc2Cmd (gSd.gcc), gccSdcc2Cfg (gSd.gcc));
  if (!sdHostSetup (fullReset)) return false;
  if (!sdSetClock (gSdccClock400k)) {
    strlcpy (gSd.reason, "no 400 kHz init clock", sizeof (gSd.reason));
    return false;
  }
  if (!branchRunning && !gccEnableSdcc2 (gSd.gcc)) {
    strlcpy (gSd.reason, "SDCC2 branch halted", sizeof (gSd.reason));
    sdLogf ("IDENT: SDCC2 branch still halted after the 400 kHz root plan (0x%08x)", gccSdcc2Branch (gSd.gcc));
    return false;
  }
  sdLogf ("IDENT: present 0x%08x caps 0x%08x/0x%08x version 0x%04x msm version 0x%08x status 0x%08x",
          sd32 (SDHCI_PRESENT_STATE), sd32 (SDHCI_CAPABILITIES), sd32 (SDHCI_CAPABILITIES_1),
          sd16 (SDHCI_HOST_VERSION), sd32 (SDHCI_MSM_MCI_VERSION), sd32 (SDHCI_MSM_MCI_STATUS));

  // CMD0: idle. A working command path returns the response bit even with no response body.
  uint32_t status = sdCommand (0, 0, SDHCI_CMD_RESP_NONE, false, false, false, 1000, nullptr);
  if (!(status & SDHCI_INT_RESPONSE) || (status & SDHCI_INT_ERROR)) {
    strlcpy (gSd.reason, "CMD0 did not complete", sizeof (gSd.reason));
    sdLogf ("IDENT: CMD0 failed (status 0x%08x present 0x%08x pwrctl 0x%08x)", status,
            sd32 (SDHCI_PRESENT_STATE), sd32 (SDHCI_MSM_PWRCTL_STATUS));
    return false;
  }
  // CMD8: interface condition. A timeout is an SD v1 card, which has no CMD8.
  bool v2 = false;
  status = sdCommand (8, 0x1AA, SDHCI_CMD_RESP_SHORT, true, true, false, 500, response);
  if (!(status & SDHCI_INT_ERROR) && (response[3] & 0xFF) == 0xAA) {
    v2 = true;
    sdLogf ("IDENT: CMD8 accepted (0x%08x)", response[3]);
  } else {
    sdLogf ("IDENT: CMD8 not accepted (0x%08x); SD v1 path", status);
  }
  // ACMD41 until the card leaves idle. S18R (bit 24) is asked only in UHS mode 2, with a platform that can move the rail
  // and a host that advertises 1.8 V: a card that answers S18A then waits for CMD11 and nothing else is changed for it.
  bool wantUhs = v2 && gSdUhsMode >= 2 && gSdSignalHook && (sd32 (SDHCI_CAPABILITIES) & SDHCI_CAN_VDD_180);
  sdLogf ("IDENT: UHS mode %u, rail hook %s, host 1.8 V %s -> S18R %s", gSdUhsMode, gSdSignalHook ? "present" : "absent",
          (sd32 (SDHCI_CAPABILITIES) & SDHCI_CAN_VDD_180) ? "yes" : "no", wantUhs ? "requested" : "not requested");
  uint32_t ocr = 0;
  bool ready = false;
  for (unsigned attempt = 0; attempt < 100; ++attempt) {
    status = sdCommand (55, 0, SDHCI_CMD_RESP_SHORT, true, true, false, 500, nullptr);
    if (status & SDHCI_INT_ERROR) {
      strlcpy (gSd.reason, "CMD55 failed", sizeof (gSd.reason));
      return false;
    }
    status = sdCommand (41, 0x40FF8000U | (wantUhs ? (1U << 24) : 0U), SDHCI_CMD_RESP_SHORT, false, false, false, 500, response);
    if (!(status & SDHCI_INT_ERROR)) {
      ocr = response[3];
      if (ocr & 0x80000000U) {
        ready = true;
        break;
      }
    }
    IOSleep (20);
  }
  if (!ready) {
    strlcpy (gSd.reason, "ACMD41 never ready", sizeof (gSd.reason));
    return false;
  }
  gSd.ocr = ocr;
  gSd.highCapacity = (ocr & 0x40000000U) != 0;
  if (wantUhs && (ocr & (1U << 24))) {
    int switched = sdVoltageSwitch ();
    if (switched < 0) {
      strlcpy (gSd.reason, "1.8 V switch failed; the card needs a power cycle", sizeof (gSd.reason));
      return false;
    }
  }
  sdLogf ("IDENT: OCR 0x%08x (%s, %s)", ocr, gSd.highCapacity ? "SDHC/SDXC" : "SDSC",
          v2 ? "v2" : "v1");
  // CMD2: CID.
  status = sdCommand (2, 0, SDHCI_CMD_RESP_LONG, true, false, false, 1000, response);
  if (status & SDHCI_INT_ERROR) {
    strlcpy (gSd.reason, "CMD2 failed", sizeof (gSd.reason));
    return false;
  }
  sdLogf ("IDENT: CID %08x%08x%08x%08x", response[0], response[1], response[2], response[3]);
  // CMD3: relative address.
  status = sdCommand (3, 0, SDHCI_CMD_RESP_SHORT, true, true, false, 1000, response);
  if (status & SDHCI_INT_ERROR) {
    strlcpy (gSd.reason, "CMD3 failed", sizeof (gSd.reason));
    return false;
  }
  gSd.rca = response[3] & 0xFFFF0000U;
  sdLogf ("IDENT: RCA 0x%04x", gSd.rca >> 16);
  // CMD9: CSD, which carries the capacity.
  status = sdCommand (9, gSd.rca, SDHCI_CMD_RESP_LONG, true, false, false, 1000, response);
  if (status & SDHCI_INT_ERROR) {
    strlcpy (gSd.reason, "CMD9 failed", sizeof (gSd.reason));
    return false;
  }
  for (unsigned i = 0; i < 4; ++i) gSd.csd[i] = response[i];
  unsigned structure = responseByte (response, 0) >> 6;
  uint64_t capacityBlocks = 0;
  if (structure == 1) {
    uint32_t csize = ((responseByte (response, 7) & 0x3F) << 16) |
                     (responseByte (response, 8) << 8) | responseByte (response, 9);
    capacityBlocks = (static_cast<uint64_t> (csize) + 1) * 1024;
  } else if (structure == 0) {
    uint32_t readBlLen = responseByte (response, 5) & 0x0F;
    uint32_t csize = ((responseByte (response, 6) & 0x03) << 10) |
                     (responseByte (response, 7) << 2) | (responseByte (response, 8) >> 6);
    uint32_t csizeMult = ((responseByte (response, 9) & 0x03) << 1) | (responseByte (response, 10) >> 7);
    uint64_t bytes = (static_cast<uint64_t> (csize) + 1) << (csizeMult + 2 + readBlLen);
    capacityBlocks = bytes / 512;
  } else {
    strlcpy (gSd.reason, "unknown CSD structure", sizeof (gSd.reason));
    return false;
  }
  gSd.blocks = capacityBlocks;
  sdLogf ("IDENT: CSD %08x%08x%08x%08x structure %u blocks %llu (%llu MiB)",
          response[0], response[1], response[2], response[3], structure, capacityBlocks,
          capacityBlocks / 2048);
  // CMD7: select.
  status = sdCommand (7, gSd.rca, SDHCI_CMD_RESP_SHORT_BUSY, true, true, false, 1000, nullptr);
  if (status & SDHCI_INT_ERROR) {
    strlcpy (gSd.reason, "CMD7 failed", sizeof (gSd.reason));
    return false;
  }
  // CMD16: 512-byte blocks.
  status = sdCommand (16, 512, SDHCI_CMD_RESP_SHORT, true, true, false, 500, nullptr);
  if (status & SDHCI_INT_ERROR) {
    strlcpy (gSd.reason, "CMD16 failed", sizeof (gSd.reason));
    return false;
  }
  // ACMD6: four data lines, then the host side of the same change.
  status = sdCommand (55, gSd.rca, SDHCI_CMD_RESP_SHORT, true, true, false, 500, nullptr);
  if (!(status & SDHCI_INT_ERROR)) {
    status = sdCommand (6, 2, SDHCI_CMD_RESP_SHORT, true, true, false, 500, nullptr);
  }
  if (status & SDHCI_INT_ERROR) {
    sdLogf ("IDENT: four-bit bus not set (0x%08x); staying 1-bit", status);
  } else {
    sdWrite8 (SDHCI_HOST_CONTROL, SDHCI_CTRL_4BITBUS);
    gSd.fourBit = true;
  }
  // A card-status read proves the selected card answers as itself.
  status = sdCommand (13, gSd.rca, SDHCI_CMD_RESP_SHORT, true, true, false, 500, response);
  sdLogf ("IDENT: status %08x (CMD13 0x%08x)", response[3], status);
  gSd.identified = true;
  if (gSd.signal18) {
    // 1.8 V already: the UHS ladder decides the speed (SDR12 -> SDR25 -> SDR50), each step verified by re-reading.
    if (!sdUhsSpeedSelect ()) {
      gSd.identified = false;
      strlcpy (gSd.reason, "no verified speed at 1.8 V; the card needs a power cycle", sizeof (gSd.reason));
      return false;
    }
    sdLogf ("CLOCK: UHS %s, %u Hz, 1.8 V", gSd.speed, gSd.clockHz);
    return true;
  }
  if (gSdUhsMode >= 1) sdUhsProbe ();
  // Service clock: 25 MHz is the SD default speed and inside every card's identification-free range.
  if (!sdSetClock (gSdccClock25m)) {
    sdLogf ("IDENT: staying at 400 kHz");
    return true;
  }
  // High-speed: CMD6 at 25 MHz, then the 50 MHz root plan and the host's high-speed bit. The GPT
  // header at LBA 1 is read back at the new clock; any failure returns to 25 MHz instead of risking
  // the root mount on a marginal link.
  if (!sdSwitchHighSpeed ()) return true;
  if (!sdSetClock (gSdccClock50m)) {
    sdSetClock (gSdccClock25m);
    sdLogf ("CLOCK: 50 MHz plan failed; staying at 25 MHz");
    return true;
  }
  sdWrite8 (SDHCI_HOST_CONTROL, (gSd.fourBit ? SDHCI_CTRL_4BITBUS : 0) | SDHCI_CTRL_HISPD);
  {
    uint8_t header[512];
    if (sdReadBlocks (1, 1, header) && !memcmp (header, "EFI PART", 8)) {
      sdLogf ("CLOCK: 50 MHz high-speed verified by GPT header read");
    } else {
      sdWrite8 (SDHCI_HOST_CONTROL, gSd.fourBit ? SDHCI_CTRL_4BITBUS : 0);
      sdSetClock (gSdccClock25m);
      sdLogf ("CLOCK: 50 MHz readback failed; back to 25 MHz");
    }
  }
  return true;
}

// Tries the least invasive path first. A failure of the adopted state (clock gated, power dropped at ExitBootServices,
// stale transfer) falls back to a full controller reset with the power handshake.
static bool sdCardInit (void)
{
  if (sdCardInitMode (false)) return true;
  char first[96];
  strlcpy (first, gSd.reason, sizeof (first));
  if (gSd.uhsCommitted) {
    sdLogf ("INIT: adopt failed after CMD11 (%s); no retry, the card needs a power cycle", first);
    return false;
  }
  sdLogf ("INIT: adopt failed (%s); retrying with a full controller reset", first);
  if (sdCardInitMode (true)) return true;
  sdLogf ("INIT: full reset failed too (%s)", gSd.reason);
  return false;
}

// ---------------------------------------------------------------- partition table

static bool gptRead (SdWindow *windows, unsigned *count, const char **why)
{
  uint8_t header[512];
  if (!sdReadBlocks (1, 1, header)) {
    *why = "GPT header read";
    return false;
  }
  if (memcmp (header, "EFI PART", 8) != 0) {
    *why = "no GPT signature";
    return false;
  }
  uint64_t entryLba = 0;
  for (unsigned i = 0; i < 8; ++i) entryLba |= static_cast<uint64_t> (header[72 + i]) << (8 * i);
  uint32_t entryCount = 0, entrySize = 0;
  for (unsigned i = 0; i < 4; ++i) {
    entryCount |= static_cast<uint32_t> (header[80 + i]) << (8 * i);
    entrySize |= static_cast<uint32_t> (header[84 + i]) << (8 * i);
  }
  uint64_t tableBytes = static_cast<uint64_t> (entryCount) * entrySize;
  if (!entryCount || entryCount > 1024 || entrySize < 128 || entrySize > 4096 || tableBytes > 1024 * 1024) {
    *why = "GPT entry geometry";
    return false;
  }
  static uint8_t table[64 * 1024];
  if (tableBytes > sizeof (table)) {
    *why = "GPT entries too large";
    return false;
  }
  if (!sdReadBlocks (entryLba, static_cast<uint32_t> ((tableBytes + 511) / 512), table)) {
    *why = "GPT entries read";
    return false;
  }
  *count = 0;
  for (uint32_t index = 0; index < entryCount; ++index) {
    const uint8_t *entry = table + static_cast<size_t> (index) * entrySize;
    bool used = false;
    for (unsigned i = 0; i < 16; ++i) used |= entry[i] != 0;
    if (!used) continue;
    char name[72];
    unsigned length = 0;
    for (unsigned i = 0; i < 36 && length + 1 < sizeof (name); ++i) {
      uint16_t unit = static_cast<uint16_t> (entry[56 + i * 2] | (entry[57 + i * 2] << 8));
      if (!unit) break;
      name[length++] = unit < 0x80 ? static_cast<char> (unit) : '?';
    }
    name[length] = 0;
    uint64_t first = 0, last = 0;
    for (unsigned i = 0; i < 8; ++i) {
      first |= static_cast<uint64_t> (entry[32 + i]) << (8 * i);
      last |= static_cast<uint64_t> (entry[40 + i]) << (8 * i);
    }
    sdLogf ("GPT: entry %u name '%s' first %llu last %llu", index, name, first, last);
    if (!strcmp (name, "OS") || !strcmp (name, "SystemOS")) {
      if (*count < 4) {
        strlcpy (windows[*count].name, name, sizeof (windows[0].name));
        windows[*count].firstLba = first;
        windows[*count].lastLba = last;
        *count = *count + 1;
      }
    }
  }
  if (*count != 2) {
    *why = "need the OS and SystemOS partitions";
    return false;
  }
  return true;
}

// ---------------------------------------------------------------- boot log persistence

static bool sdFlushLog (void)
{
  if (!gLogCardReady || gLogBusy || !gSd.identified || !gSd.lock) return false;
  IOLockLock (gSd.lock);
  if (!gLogCardReady || gLogBusy || !gSd.identified) {
    IOLockUnlock (gSd.lock);
    return false;
  }
  gLogBusy = true;
  memcpy (gLog, SD_LOG_MAGIC, 8);
  uint32_t length = static_cast<uint32_t> (gLogUsed);
  memcpy (gLog + 8, &gLogSeq, 4);
  memcpy (gLog + 12, &length, 4);
  uint64_t blocks = gSd.blocks;
  uint32_t capacity = static_cast<uint32_t> (blocks);
  memcpy (gLog + 16, &capacity, 4);
  bool ok = true;
  for (unsigned sector = 0; sector < SD_LOG_SECTORS; ++sector) {
    if (!sdWriteBlock (gLogFirstLba + sector, reinterpret_cast<const uint8_t *> (gLog) + sector * 512)) {
      ok = false;
      break;
    }
  }
  gLogBusy = false;
  IOLockUnlock (gSd.lock);
  return ok;
}

// Increments the sequence of the log left by the previous boot so several boots can be told apart.
static void sdLogAdoptPrevious (void)
{
  static uint8_t previous[SD_LOG_BYTES];
  if (!gSd.identified || !gSd.blocks) return;
  if (!sdReadBlocks (gLogFirstLba, SD_LOG_SECTORS, previous)) return;
  if (memcmp (previous, SD_LOG_MAGIC, 8) != 0) {
    gLogSeq = 1;
    return;
  }
  uint32_t seq = 0;
  memcpy (&seq, previous + 8, 4);
  gLogSeq = seq + 1;
}



#endif
