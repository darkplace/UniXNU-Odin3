// SPDX-License-Identifier: BSD-2-Clause-Patent
// OdinSd - the Odin 3's microSD slot as the verified read-only root block device of XNU.
//
// Host side (SDCC2, SDHCI v5 at 0x08804000, GCC root generator, PIO, card boot log) is the SdhciMsm driver of
// devices/UniXNU-odin3, unchanged. The publication side follows the kernel's storage-provider contract
// (https://unixnu.com/porting/ufs, "Completion criteria"; https://unixnu.com/concepts/security, "Verified root"):
//   * the card's GPT is parsed and the "OS" and "SystemOS" partitions are published as separate
//     IOBlockStorageDevice windows of 512-byte blocks (the OS image carries its own GPT/APFS inside);
//   * every read is checked against the owner-signed policy whose signed image size equals the window
//     (NativeVirtioBlock.cpp configureVerification/readSectors, same vr_* calls) and nothing is returned
//     unless all 4 KiB blocks verify;
//   * native_ufs_block_authenticates() answers the root-authorization query of NativeVirtioBlock
//     (native_verified_block_root_authorized) for these windows, as NativeUfs does for UFS windows.
//
// The class is named NativeUfs because the kernel's personality table (iokit/KernelConfigTables.cpp, flag
// XNU_NATIVE_UFS) instantiates that class on IOPlatformDevice nodes; the probe accepts only the sdhci node.
#include <IOKit/IOService.h>
#include <IOKit/IOPlatformExpert.h>
#include <IOKit/IODeviceMemory.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/storage/IOBlockStorageDevice.h>
#include <libkern/c++/OSData.h>
#include <libkern/c++/OSDictionary.h>
#include <libkern/c++/OSNumber.h>
#include <libkern/c++/OSString.h>
#include <libkern/libkern.h>
#include <kern/debug.h>
#include <pexpert/pexpert.h>
#include <sys/param.h>
#include <sys/types.h>
#include <vm/vm_kern.h>
#include <vm/vm_map.h>
#include <libkern/crypto/sha2.h>
#include "verified_root.h"
#include "VerifiedBlockPolicy.h"

#include <kern/clock.h>
#include "Sm8750Platform/Rpmh.h"
#include "SdHost.h"
#include "OdinRuns.h"
#include "SdStore.h"

// ---------------------------------------------------------------- verified windows

extern "C" {
extern const struct vr_crypto vr_xnu_crypto;
extern const uint8_t verified_root_key[];
// Counters shown by the boot-progress page when the owner cache stage is set (BootProgress.c:745-749).
volatile uint64_t native_ufs_block_reads = 0;
volatile uint64_t native_ufs_block_failures = 0;
volatile uint64_t native_sd_verified_blocks = 0;
volatile uint32_t native_sd_integrity_failures = 0;
volatile uint32_t native_sd_ambiguous_policies = 0;
volatile uint32_t native_sd_windows_verified = 0;
}

class NativeUfs;

class OdinSdWindow : public IOBlockStorageDevice {
  OSDeclareDefaultStructors (OdinSdWindow)
  NativeUfs *owner = nullptr;
  uint64_t absolute = 0;
  uint64_t bytes = 0;
  char label[32] = {};
  bool online = false;
  struct vr_view verified = {};
  vm_offset_t verifiedStorage = 0;
  vm_size_t verifiedStorageBytes = 0;
  unsigned char *snapshot = nullptr;
  IOLock *snapshotGate = nullptr;
  static constexpr unsigned kTransfer = 65536;
  bool configureVerification ();
  IOReturn readVerified (uint64_t sector, uint32_t length);
public:
  bool configure (NativeUfs *host, const char *name, uint64_t absoluteLba, uint64_t blockCount);
  bool authenticatesVolume (const char *uuid, uint8_t *manifestDigest = nullptr) const;
  bool start (IOService *provider) override;
  void free () override;
  IOReturn doEjectMedia () override { return kIOReturnUnsupported; }
  IOReturn doFormatMedia (UInt64) override { return kIOReturnNotWritable; }
  UInt32 doGetFormatCapacities (UInt64 *, UInt32) const override { return 0; }
  char *getVendorString () override { return const_cast<char *> ("OdinSd"); }
  char *getProductString () override { return label; }
  char *getRevisionString () override { return const_cast<char *> ("1.0"); }
  char *getAdditionalDeviceInfoString () override { return const_cast<char *> ("microSD partition, verified read-only"); }
  IOReturn reportBlockSize (UInt64 *value) override { *value = 512; return kIOReturnSuccess; }
  IOReturn reportEjectability (bool *value) override { *value = false; return kIOReturnSuccess; }
  IOReturn reportMaxValidBlock (UInt64 *value) override { *value = bytes / 512 - 1; return kIOReturnSuccess; }
  IOReturn reportMediaState (bool *present, bool *changed) override {
    *present = online; if (changed) *changed = false; return kIOReturnSuccess;
  }
  IOReturn reportRemovability (bool *value) override { *value = false; return kIOReturnSuccess; }
  IOReturn reportWriteProtection (bool *value) override { *value = true; return kIOReturnSuccess; }
  IOReturn doSynchronizeCache () override { return online ? kIOReturnSuccess : kIOReturnNotReady; }
  IOReturn doSynchronize (UInt64, UInt64, IOOptionBits) override { return doSynchronizeCache (); }
  IOReturn getWriteCacheState (bool *value) override { *value = false; return kIOReturnSuccess; }
  IOReturn setWriteCacheState (bool) override { return kIOReturnUnsupported; }
  IOReturn doAsyncReadWrite (IOMemoryDescriptor *, UInt64, UInt64, IOStorageAttributes *, IOStorageCompletion *) override;
};

class NativeUfs : public IOService {
  OSDeclareDefaultStructors (NativeUfs)
  OSSharedPtr<IOMemoryMap> fMap;
  OSSharedPtr<IOMemoryMap> fGccMap;
  OdinSdWindow *fWindow[4] = {};
  unsigned fWindows = 0;
  bool fPublished = false;
public:
  IOService *probe (IOService *provider, SInt32 *score) override;
  bool start (IOService *provider) override;
  void stop (IOService *provider) override;
  // Reads `count` sectors at absolute `lba` into `out` (CPU memory), one lock hold.
  bool readRaw (uint64_t lba, uint32_t count, uint8_t *out);
};

OSDefineMetaClassAndStructors (NativeUfs, IOService)
OSDefineMetaClassAndStructors (OdinSdWindow, IOBlockStorageDevice)

static uintptr_t mapPhys (uint64_t physical, uint64_t length, OSSharedPtr<IOMemoryMap> &keep)
{
  IODeviceMemory *memory = IODeviceMemory::withRange (physical, length);
  if (!memory) return 0;
  keep = OSSharedPtr<IOMemoryMap> (memory->map (kIOMapAnywhere | kIOMapInhibitCache), OSNoRetain);
  memory->release ();
  return keep ? static_cast<uintptr_t> (keep->getVirtualAddress ()) : 0;
}


// ---------------------------------------------------------------- UHS rail (odin-sd-uhs)
//
// The card's I/O rail is vqmmc = L8B (ayn cq8725s common dtsi: vreg_l8b_1p8, 1.8-3.008 V; Linux moves it from the PWRCTL
// IO_LOW/IO_HIGH handshake). XNU has no regulator driver, so the same RPMh request the touch driver uses on the 13T
// (Sm8750Platform/Rpmh.h) is the only way: apps DRV-2 at 0x16520000, command DB at 0x81c60000 (upstream sm8750.dtsi
// apps_rsc / aop-cmd-db), resource name "ldob8". Nothing here runs unless odin-sd-uhs >= 1; the rail is written only in
// mode 2, only after the probe boot printed the VRM address and only when odin-sd-vqmmc names that exact address.
static constexpr uint64_t kApps2Rsc = 0x16520000ULL, kApps2RscBytes = 0x10000ULL;
static constexpr uint64_t kCmdDb = 0x81c60000ULL, kCmdDbBytes = 0x20000ULL;
static OSSharedPtr<IOMemoryMap> gRscMap, gCmdDbMap;
static struct peripheral_io gRsc, gCmdDb;
static uint32_t gVqmmcVrm;

static uint32_t railRead (void *context, uint32_t reg)
{
  return *reinterpret_cast<volatile uint32_t *> (reinterpret_cast<uintptr_t> (context) + reg);
}
static void railWrite (void *context, uint32_t reg, uint32_t value)
{
  *reinterpret_cast<volatile uint32_t *> (reinterpret_cast<uintptr_t> (context) + reg) = value;
  __asm__ volatile ("dsb sy" ::: "memory");
}
static void railDelay (void *, unsigned microseconds) { IODelay (microseconds); }

// Read-only: map the command DB and look "ldob8"/"ldob9" up. The RSC window is mapped but not touched here.
static bool railResolve (uint32_t *vqmmc, uint32_t *vmmc)
{
  uintptr_t db = mapPhys (kCmdDb, kCmdDbBytes, gCmdDbMap);
  uintptr_t rsc = mapPhys (kApps2Rsc, kApps2RscBytes, gRscMap);
  if (!db || !rsc) {
    sdLogf ("RAIL: cannot map cmd-db 0x%llx / apps DRV-2 0x%llx", kCmdDb, kApps2Rsc);
    return false;
  }
  gCmdDb = { reinterpret_cast<void *> (db), railRead, railWrite, railDelay };
  gRsc = { reinterpret_cast<void *> (rsc), railRead, railWrite, railDelay };
  uint32_t a = 0, b = 0;
  int rcA = rpmh_vrm_address (&gCmdDb, static_cast<unsigned> (kCmdDbBytes), "ldob8", &a);
  int rcB = rpmh_vrm_address (&gCmdDb, static_cast<unsigned> (kCmdDbBytes), "ldob9", &b);
  sdLogf ("RAIL: cmd-db magic 0x%08x ldob8 (vqmmc) rc %d addr 0x%05x, ldob9 (vmmc) rc %d addr 0x%05x",
          railRead (gCmdDb.context, 4), rcA, rcA ? 0U : a, rcB, rcB ? 0U : b);
  if (rcA) return false;
  if (vqmmc) *vqmmc = a;
  if (vmmc) *vmmc = rcB ? 0 : b;
  return true;
}

// Called only by sdVoltageSwitch() while a PWRCTL IO_LOW request is pending (SdHost.h). Never returns to 3.3 V.
static bool railSignalHook (bool low18)
{
  if (!low18 || !gVqmmcVrm) return false;
  struct rpmh_result result = {};
  int rc = sm8750_rpmh_vrm_enable (&gRsc, gVqmmcVrm, 1800, &result);
  sdLogf ("RAIL: ldob8 0x%05x -> 1800 mV rc %d (rsc hw 0x%08x tcs %u status %u cmd %08x %08x %08x)", gVqmmcVrm, rc,
          result.hardware, result.tcs, result.status, result.command[0], result.command[1], result.command[2]);
  return rc == 0;
}

bool NativeUfs::readRaw (uint64_t lba, uint32_t count, uint8_t *out)
{
  if (!gSd.lock || !out) return false;
  IOLockLock (gSd.lock);
  bool ok = sdReadBlocks (lba, count, out);
  IOLockUnlock (gSd.lock);
  if (ok) __atomic_fetch_add (&native_ufs_block_reads, 1, __ATOMIC_RELAXED);
  else __atomic_fetch_add (&native_ufs_block_failures, 1, __ATOMIC_RELAXED);
  return ok;
}

bool OdinSdWindow::configure (NativeUfs *host, const char *name, uint64_t absoluteLba, uint64_t blockCount)
{
  if (!host || !name || !blockCount || blockCount > UINT64_MAX / 512) return false;
  owner = host;
  absolute = absoluteLba;
  bytes = blockCount * 512;
  strlcpy (label, name, sizeof (label));
  return true;
}

// Select the one owner-signed policy whose signed image size equals this window (NativeVirtioBlock.cpp).
bool OdinSdWindow::configureVerification ()
{
  const uint8_t *manifest = nullptr;
  uint64_t manifestBytes = 0;
  if (!verified_root_policy_count || verified_root_policy_count > 16) {
    sdLogf ("VERIFY: %s: policy count %llu invalid", label, verified_root_policy_count);
    return false;
  }
  for (uint64_t index = 0; index < verified_root_policy_count; ++index) {
    const NativeVerifiedBlockPolicy &policy = verified_root_policies[index];
    if (!policy.manifest || policy.manifestBytes < VR_HEADER_BYTES + VR_SIGNATURE_BYTES) return false;
    uint64_t imageBytes = 0;
    for (unsigned i = 0; i < 8; ++i) imageBytes |= static_cast<uint64_t> (policy.manifest[24 + i]) << (8 * i);
    if (imageBytes != bytes) continue;
    if (manifest) {
      __atomic_fetch_add (&native_sd_ambiguous_policies, 1, __ATOMIC_RELAXED);
      sdLogf ("VERIFY: %s: ambiguous policies", label);
      return false;
    }
    manifest = policy.manifest;
    manifestBytes = policy.manifestBytes;
  }
  if (!manifest) {
    sdLogf ("VERIFY: %s: no policy for %llu bytes", label, bytes);
    return false;
  }
  vm_size_t tableBytes = round_page (static_cast<vm_size_t> (manifestBytes));
  if (tableBytes < manifestBytes || tableBytes > UINT64_MAX - 4096) return false;
  verifiedStorageBytes = tableBytes + 4096;
  if (kmem_alloc (kernel_map, &verifiedStorage, verifiedStorageBytes,
                  static_cast<kma_flags_t> (KMA_KOBJECT | KMA_DATA), VM_KERN_MEMORY_SECURITY) != KERN_SUCCESS) return false;
  memcpy (reinterpret_cast<void *> (verifiedStorage), manifest, manifestBytes);
  uint8_t *key = reinterpret_cast<uint8_t *> (verifiedStorage + tableBytes);
  memcpy (key, verified_root_key, VR_KEY_BYTES);
  if (vm_map_protect (kernel_map, verifiedStorage, verifiedStorage + verifiedStorageBytes,
                      VM_PROT_READ, TRUE) != KERN_SUCCESS) return false;
  if (vr_open (reinterpret_cast<const uint8_t *> (verifiedStorage), manifestBytes, key, 1,
               &vr_xnu_crypto, &verified) != VR_OK || verified.image_bytes != bytes) {
    sdLogf ("VERIFY: %s: signed policy rejected", label);
    return false;
  }
  snapshot = static_cast<unsigned char *> (IOMalloc (kTransfer));
  if (!snapshot) return false;
  __atomic_fetch_add (&native_sd_windows_verified, 1, __ATOMIC_RELAXED);
  sdLogf ("VERIFY: %s: policy accepted, %llu blocks of 4 KiB", label, static_cast<uint64_t> (verified.block_count));
  return true;
}

bool OdinSdWindow::authenticatesVolume (const char *uuid, uint8_t *manifestDigest) const
{
  if (!uuid || !online || !verifiedStorage || !snapshot || !verified.hashes) return false;
  static const char hex[] = "0123456789ABCDEF";
  char expected[37];
  unsigned at = 0;
  for (unsigned i = 0; i < 16; ++i) {
    if (i == 4 || i == 6 || i == 8 || i == 10) expected[at++] = '-';
    expected[at++] = hex[verified.volume_uuid[i] >> 4];
    expected[at++] = hex[verified.volume_uuid[i] & 15];
  }
  expected[at] = 0;
  if (strcmp (uuid, expected)) return false;
  if (manifestDigest) {
    const uint64_t envelope = VR_HEADER_BYTES + VR_SIGNATURE_BYTES + verified.block_count * 32;
    SHA384_CTX context;
    SHA384_Init (&context);
    SHA384_Update (&context, reinterpret_cast<const void *> (verifiedStorage), envelope);
    SHA384_Final (manifestDigest, &context);
  }
  return true;
}

// Read `length` bytes from window sector `sector` into `snapshot` (CPU-owned memory: PIO, no DMA, so there is
// nothing to race the hash), verifying every covered 4 KiB block. Returns the bytes at snapshot[0..length).
IOReturn OdinSdWindow::readVerified (uint64_t sector, uint32_t length)
{
  if (!snapshot || !online || !length || length > kTransfer - VR_BLOCK_BYTES || (length & 511) ||
      sector >= bytes / 512 || length / 512 > bytes / 512 - sector) return kIOReturnBadArgument;
  sdStackMark (1);
  uint64_t begin = sector * 512, end = begin + length;
  uint64_t alignedBegin = begin & ~static_cast<uint64_t> (VR_BLOCK_BYTES - 1);
  uint64_t padding = (VR_BLOCK_BYTES - (end & (VR_BLOCK_BYTES - 1))) & (VR_BLOCK_BYTES - 1);
  if (padding > bytes - end) padding = bytes - end;
  uint32_t span = static_cast<uint32_t> (end + padding - alignedBegin);
  if (span > kTransfer) return kIOReturnBadArgument;
  if (!owner->readRaw (absolute + alignedBegin / 512, span / 512, snapshot)) return kIOReturnIOError;
  for (uint32_t offset = 0; offset < span; offset += VR_BLOCK_BYTES) {
    uint32_t piece = span - offset < VR_BLOCK_BYTES ? span - offset : VR_BLOCK_BYTES;
    if (vr_verify_block (&verified, alignedBegin / VR_BLOCK_BYTES + offset / VR_BLOCK_BYTES,
                         snapshot + offset, piece, &vr_xnu_crypto) != VR_OK) {
      __atomic_fetch_add (&native_sd_integrity_failures, 1, __ATOMIC_RELAXED);
      return kIOReturnNotPrivileged;
    }
    __atomic_fetch_add (&native_sd_verified_blocks, 1, __ATOMIC_RELAXED);
    sdStackMark (2);
  }
  memmove (snapshot, snapshot + (begin - alignedBegin), length);
  return kIOReturnSuccess;
}

bool OdinSdWindow::start (IOService *provider)
{
  if (!owner || !IOBlockStorageDevice::start (provider)) return false;
  snapshotGate = IOLockAlloc ();
  if (!snapshotGate) return false;
  if (!configureVerification ()) return false;
  online = true;
  registerService ();
  return true;
}

void OdinSdWindow::free ()
{
  online = false;
  if (snapshotGate) { IOLockFree (snapshotGate); snapshotGate = nullptr; }
  if (snapshot) { IOFree (snapshot, kTransfer); snapshot = nullptr; }
  if (verifiedStorage) { kmem_free (kernel_map, verifiedStorage, verifiedStorageBytes); verifiedStorage = 0; }
  IOBlockStorageDevice::free ();
}

IOReturn OdinSdWindow::doAsyncReadWrite (IOMemoryDescriptor *buffer, UInt64 block, UInt64 count,
                                         IOStorageAttributes *, IOStorageCompletion *completion)
{
  sdStackMark (0);
  if (!buffer || !completion || !completion->action || !count ||
      block >= bytes / 512 || count > bytes / 512 - block || buffer->getLength () < count * 512) {
    return kIOReturnBadArgument;
  }
  if (buffer->getDirection () != kIODirectionIn) return kIOReturnNotWritable;
  if (!online || !snapshotGate) return kIOReturnNotReady;
  uint64_t actual = 0;
  IOReturn result = kIOReturnSuccess;
  constexpr uint32_t chunk = kTransfer - VR_BLOCK_BYTES;
  // The verified snapshot must remain CPU-owned through verification AND the
  // descriptor copy. The transport lock alone only serialized sdReadBlocks.
  IOLockLock (snapshotGate);
  while (actual < count * 512) {
    uint32_t length = static_cast<uint32_t> (count * 512 - actual > chunk ? chunk : count * 512 - actual);
    result = readVerified (block + actual / 512, length);
    if (result) break;
    if (buffer->writeBytes (actual, snapshot, length) != length) { result = kIOReturnIOError; break; }
    actual += length;
  }
  IOLockUnlock (snapshotGate);
  completion->action (completion->target, completion->parameter, result, actual);
  return kIOReturnSuccess;
}

// Same answer NativeUfs gives for its windows: -1 when `provider` is not one of ours, else 1/0 for the signed UUID.
extern "C" int native_ufs_block_authenticates (IOService *provider, const char *uuid, uint8_t *manifestDigest)
{
  OdinSdWindow *window = OSDynamicCast (OdinSdWindow, provider);
  if (!window) return -1;
  return window->authenticatesVolume (uuid, manifestDigest) ? 1 : 0;
}

// ABI names retained for NativeRamBlock; these functions use SDCC2 ONLY, never UFS.
// Default off. A dedicated name, type AND boot-argument partition GUID are required.
static odin_store::Region dataRegion;
static uint64_t dataBlocks;
static bool dataPoisoned;
static uint64_t gStoreWrites;
// A poisoned store loses its write window too: after any doubt nothing reaches the card through this path.
static void dataPoison() { dataPoisoned=true; gSdWriteWindow[1]={}; }
static uint8_t dataTables[2][odin_store::kTableBytes]; // protected by gSd.lock
extern "C" uint8_t native_ram_block_seed_sha256[32];
struct SdStoreIO {
  bool read(uint64_t lba,uint32_t count,void *dst) { return sdReadBlocks(lba,count,static_cast<uint8_t *>(dst)); }
};
extern "C" unsigned native_ufs_store_mode(void) {
  unsigned mode=0;PE_parse_boot_argn("sheng-data-persist",&mode,sizeof mode);return mode<=2?mode:0;
}
static bool dataLocate(odin_store::Region &region,const char **why) {
  char text[38]={};uint8_t expected[16]={};
  if(!PE_parse_boot_argn("odin-data-guid",text,sizeof text-1) || !odin_store::guid(text,expected)) {
    *why="require canonical odin-data-guid";return false;
  }
  SdStoreIO io;
  return odin_store::locate(io,gSd.blocks,gLogFirstLba,expected,region,dataTables[0],dataTables[1],why);
}
extern "C" int native_ufs_store_open(uint64_t blocks,uint64_t *firstLba,const char **why) {
  const char *reason="store unavailable";bool ok=false;
  if(gSd.lock && gSd.identified && native_ufs_store_mode()) {
    IOLockLock(gSd.lock);
    odin_store::Region found;
    if(!dataBlocks && !dataPoisoned && dataLocate(found,&reason)) {
      if(!blocks || blocks>found.sectors/8)reason="Data partition too small";
      else {
        // Static, not on the stack: this is the Data-mount path and the 16 KiB stack was already
        // nearly gone when it ran (change mount-panic). Safe: gSd.lock is held around this call.
        static uint8_t header[4096];unsigned reset=0;PE_parse_boot_argn("sheng-data-reset",&reset,sizeof reset);
        if(native_ufs_store_mode()==2 && (!sdReadBlocks(found.first,8,header) ||
            !odin_store::seedCompatible(header,blocks,native_ram_block_seed_sha256,reset==1 || reset==2)))
          reason="foreign/damaged/stale Data header; explicit reset required";
        else {
          dataRegion=found;dataBlocks=blocks;if(firstLba)*firstLba=found.first/8;ok=true;
          // Only mode 2 may write, and only inside the pinned, GUID-checked Data partition (never p1-p3, never a GPT).
          if(native_ufs_store_mode()==2)gSdWriteWindow[1]={found.first,found.first+blocks*8};
        }
      }
    }
    IOLockUnlock(gSd.lock);
    // Outcome on the card log (read it with scripts/h1/read-sd-log.sh): the store has no other visible channel.
    if(ok)sdLogf("STORE: mode %u open, Data partition LBA %llu, %llu sectors, overlay %llu blocks, write window %s",
                 native_ufs_store_mode(),(unsigned long long)dataRegion.first,(unsigned long long)dataRegion.sectors,(unsigned long long)dataBlocks,
                 native_ufs_store_mode()==2?"OPEN":"closed (inspection)");
    else sdLogf("STORE: mode %u refused: %s",native_ufs_store_mode(),reason);
    sdFlushLog();
  }
  if(why)*why=reason;
  return ok;
}
extern "C" int native_ufs_store_read(uint64_t rel,uint32_t blocks,void *dst) {
  sdStackMark(8);
  if(!gSd.lock || !dst)return 0;
  IOLockLock(gSd.lock);uint64_t sector=0;
  bool ok=odin_store::range(dataRegion,dataBlocks,rel,blocks,&sector) &&
    blocks<=UINT32_MAX/8 && sdReadBlocks(sector,blocks*8,static_cast<uint8_t *>(dst));
  IOLockUnlock(gSd.lock);return ok;
}
// SD has no UFS FUA bit. Wait for the card to leave programming state, then
// verify every 4 KiB by reading it back. This does NOT claim power-loss atomicity.
static bool dataReady() {
  for(unsigned i=0;i<5000;++i) {
    uint32_t r[4]={};uint32_t s=sdCommand(13,gSd.rca,SDHCI_CMD_RESP_SHORT,true,true,false,500,r);
    // include/linux/mmc/mmc.h @ c339f401: R1_STATUS, plus locked and SD AKE error.
    if((s&SDHCI_INT_ERROR) || !(s&SDHCI_INT_RESPONSE) || (r[3]&(0xfff9a000u|(1u<<25)|(1u<<3))))return false;
    if((r[3]&(1u<<8)) && ((r[3]>>9)&15)==4)return true;
    IODelay(1000);
  }
  return false;
}
extern "C" int native_ufs_store_write(uint64_t rel,uint32_t blocks,const void *src) {
  sdStackMark(9);   // this frame is 0x10b0 B: it holds readback[4096]
  if(!gSd.lock || !src || native_ufs_store_mode()!=2)return 0;
  IOLockLock(gSd.lock);uint64_t sector=0;
  bool ok=!dataPoisoned && odin_store::range(dataRegion,dataBlocks,rel,blocks,&sector);
  // Static, not on the stack: this frame was 0x10b0 B and the mount chain ran out of its 16 KiB
  // stack inside the scheduler (change mount-panic). Safe because gSd.lock is held from the line
  // above to the unlock below, and the transport is PIO.
  static uint8_t readback[4096];const uint8_t *p=static_cast<const uint8_t *>(src);
  for(uint32_t i=0;ok && i<blocks;++i,p+=4096,sector+=8) {
    ok=sdTransfer(false,sector,8,const_cast<uint8_t *>(p)) && dataReady() &&
      sdReadBlocks(sector,8,readback) && !memcmp(p,readback,sizeof readback);
  }
  if(!ok)dataPoison();
  uint64_t done=ok?++gStoreWrites:gStoreWrites;
  IOLockUnlock(gSd.lock);
  // First write, every 2048th, and any failure reach the card log; the rest would only slow the store down.
  if(!ok){sdLogf("STORE: write of %u block(s) at overlay block %llu FAILED after %llu writes; store poisoned, no further writes",blocks,(unsigned long long)rel,(unsigned long long)done);sdFlushLog();}
  else if(done==1 || !(done&2047)){sdLogf("STORE: %llu writes ok, %llu KiB, %llu ms inside the transport",(unsigned long long)done,(unsigned long long)(native_sd_write_bytes>>10),(unsigned long long)(native_sd_write_us/1000));sdFlushLog();}
  return ok;
}
extern "C" int native_ufs_store_sync(void) {
  if(!gSd.lock || native_ufs_store_mode()!=2)return 0;
  IOLockLock(gSd.lock);bool ok=dataBlocks && !dataPoisoned && dataReady();
  if(!ok)dataPoison();
  IOLockUnlock(gSd.lock);
  if(!ok){sdLogf("STORE: sync FAILED (card not ready / error status); store poisoned");sdFlushLog();}
  return ok;
}
extern "C" unsigned native_ufs_store_gpt_check(void) {
  if(!gSd.lock || !gSd.identified)return 1;
  IOLockLock(gSd.lock);odin_store::Region found;const char *why="";
  bool ok=dataLocate(found,&why) && (!dataBlocks || dataRegion.same(found));
  if(!ok && dataBlocks)dataPoison();
  IOLockUnlock(gSd.lock);
  if(!ok && dataBlocks){sdLogf("STORE: GPT check FAILED (partition moved or tables diverged); store poisoned");sdFlushLog();}
  return ok?0:1;
}

IOService *NativeUfs::probe (IOService *provider, SInt32 *score)
{
  __atomic_fetch_add (&native_sd_probe_calls, 1, __ATOMIC_RELAXED);
  OSObject *property = provider->getProperty ("compatible");
  OSData *data = OSDynamicCast (OSData, property);
  OSString *string = OSDynamicCast (OSString, property);
  const char *text = nullptr;
  size_t length = 0;
  if (data) {
    text = static_cast<const char *> (data->getBytesNoCopy ());
    length = data->getLength ();
  } else if (string) {
    text = string->getCStringNoCopy ();
    length = string->getLength ();
  }
  if (!text) return nullptr;
  bool match = false;
  for (size_t at = 0; at + 17 <= length; ++at) {
    if (!memcmp (text + at, "qcom,sm8750-sdhci", 17) || !memcmp (text + at, "qcom,sdhci-msm-v5", 17)) {
      match = true;
      break;
    }
  }
  if (!match) return nullptr;
  sdLogf ("PROBE: sdhci provider accepted (%u probe calls so far)", native_sd_probe_calls);
  if (score) *score = 1000;
  return this;
}

bool NativeUfs::start (IOService *provider)
{
  unsigned disabled = 0;
  PE_parse_boot_argn ("native-sd-off", &disabled, sizeof (disabled));
  gSd.lock = IOLockAlloc ();
  if (!gSd.lock) return false;
  fMap = OSSharedPtr<IOMemoryMap> ();
  uintptr_t base = mapPhys (SDHCI_BASE, SDHCI_BASE_SIZE, fMap);
  uintptr_t gcc = mapPhys (GCC_SDCC2_WINDOW, GCC_SDCC2_WINDOW_SIZE, fGccMap);
  if (!base || !gcc) {
    sdLogf ("START: cannot map SDHCI 0x%llx / GCC 0x%llx", SDHCI_BASE, GCC_SDCC2_WINDOW);
    return false;
  }
  gSd.base = base;
  gSd.gcc = gcc;
  gSd.mapped = true;
  IOLockLock (gSd.lock);
  sdLogf ("START: SDHCI 0x%llx mapped, GCC 0x%llx mapped, driver %s", SDHCI_BASE, GCC_SDCC2_WINDOW,
          disabled ? "disabled" : "enabled");
  if (disabled) {
    IOLockUnlock (gSd.lock);
    return false;
  }
  {
    unsigned uhs = 0, pinned = 0;
    PE_parse_boot_argn ("odin-sd-uhs", &uhs, sizeof (uhs));
    PE_parse_boot_argn ("odin-sd-vqmmc", &pinned, sizeof (pinned));
    gSdUhsMode = uhs <= 2 ? uhs : 0;
    gSdSignalHook = nullptr;
    if (gSdUhsMode >= 1) {
      uint32_t vqmmc = 0, vmmc = 0;
      bool resolved = railResolve (&vqmmc, &vmmc);
      if (gSdUhsMode >= 2) {
        if (resolved && pinned == vqmmc) {
          gVqmmcVrm = vqmmc;
          gSdSignalHook = railSignalHook;
        } else {
          sdLogf ("RAIL: UHS mode 2 refused: ldob8 %s, odin-sd-vqmmc 0x%x (the probe boot must print the address first)",
                  resolved ? "resolved" : "not resolved", pinned);
        }
      }
    }
    unsigned dma = 0;
    PE_parse_boot_argn ("odin-sd-dma", &dma, sizeof (dma));
    gSdDmaRequest = dma == 1 ? 1 : 0;
    gAdmaNoted = false;
    sdLogf ("START: odin-sd-uhs %u, odin-sd-dma %u, rail hook %s", gSdUhsMode, gSdDmaRequest, gSdSignalHook ? "armed" : "off");
    // uefi-runs: the window the kernel believes it owns, and the free runs the loader appended.
    // One line per run: sdLogf truncates at 255 bytes and the high bank matters most.
    {
      extern unsigned long gPhysBase, gPhysSize;
      char runs[320];
      uint64_t run[8][2];
      unsigned count = 0;
      sdLogf ("UEFI: window base 0x%llx size 0x%llx",
              static_cast<unsigned long long> (gPhysBase), static_cast<unsigned long long> (gPhysSize));
      if (!PE_parse_boot_arg_str ("db-runs", runs, sizeof (runs))) sdLogf ("UEFI: db-runs absent");
      else if (runs[0] == 'n' && runs[1] == 'o' && runs[2] == 'n' && runs[3] == 'e' && runs[4] == 0) {
        sdLogf ("UEFI: db-runs none");
      } else if (!odin_runs::parse (runs, run, 8, &count)) {
        sdLogf ("UEFI: db-runs malformed");
      } else {
        sdLogf ("UEFI: db-runs %u%s", count, count == 8 ? " (cap reached; later runs may be missing)" : "");
        for (unsigned i = 0; i < count; i++) {
          sdLogf ("UEFI: run %u 0x%llx+0x%llx", i,
                  static_cast<unsigned long long> (run[i][0]), static_cast<unsigned long long> (run[i][1]));
        }
      }
    }
  }
  if (!sdCardInit ()) {
    sdLogf ("START: card identification failed: %s", gSd.reason);
    IOLockUnlock (gSd.lock);
    return false;
  }
  gLogFirstLba = gSd.blocks > SD_LOG_RESERVED ? gSd.blocks - SD_LOG_RESERVED : 0;
  gLogCardReady = gLogFirstLba != 0;
  if (gLogCardReady) gSdWriteWindow[0] = { gLogFirstLba, gLogFirstLba + SD_LOG_SECTORS };
  sdLogAdoptPrevious ();
  sdLogf ("INIT: card ready, clock %u Hz, %u-bit, %llu blocks, log at LBA %llu",
          gSd.clockHz, static_cast<unsigned> (gSd.fourBit ? 4 : 1), gSd.blocks, gLogFirstLba);
  const char *why = "";
  SdWindow windows[4] = {};
  unsigned count = 0;
  bool parsed = gptRead (windows, &count, &why);
  IOLockUnlock (gSd.lock);
  // The log write and the window publication run without the transport lock: registering a block
  // device lets the storage family probe it in this thread, and that probe reads through this driver.
  sdFlushLog ();
  if (!parsed) {
    sdLogf ("GPT: %s", why);
    sdFlushLog ();
    return false;
  }
  if (!IOService::start (provider)) {
    sdLogf ("START: IOService::start failed");
    sdFlushLog ();
    return false;
  }
  // SystemOS first: the root mount resolves that volume immediately after the root appears.
  const char *order[2] = { "SystemOS", "OS" };
  for (unsigned position = 0; position < 2; ++position) {
    SdWindow *found = nullptr;
    for (unsigned i = 0; i < count; ++i) {
      if (!strcmp (windows[i].name, order[position])) found = &windows[i];
    }
    if (!found) continue;
    OdinSdWindow *device = OSTypeAlloc (OdinSdWindow);
    if (!device || !device->init (static_cast<OSDictionary *> (nullptr)) ||
        !device->configure (this, found->name, found->firstLba, found->lastLba - found->firstLba + 1)) {
      sdLogf ("PUBLISH: %s allocation failed", order[position]);
      if (device) device->release ();
      continue;
    }
    if (!device->attach (this) || !device->start (this)) {
      sdLogf ("PUBLISH: %s did not start (no verified policy or storage refusal)", order[position]);
      device->detach (this);
      device->release ();
      continue;
    }
    fWindow[fWindows++] = device;
    sdLogf ("PUBLISH: %s at LBA %llu, %llu blocks (%llu MiB), start requested", found->name,
            found->firstLba, found->lastLba - found->firstLba + 1,
            ((found->lastLba - found->firstLba + 1) * 512) / (1024 * 1024));
    fPublished = true;
  }
  sdLogf ("PUBLISH: %u window(s) started", fWindows);
  sdFlushLog ();
  return fPublished;
}

void NativeUfs::stop (IOService *provider)
{
  for (unsigned i = 0; i < fWindows; ++i) {
    if (fWindow[i]) {
      fWindow[i]->detach (this);
      fWindow[i]->release ();
      fWindow[i] = nullptr;
    }
  }
  fMap.reset ();
  fGccMap.reset ();
  IOService::stop (provider);
}
