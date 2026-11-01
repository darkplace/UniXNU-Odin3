// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <cassert>
#include <cstdarg>
#include <cstdint>
#include <cstring>
#include <cstdio>
#include <map>
#include <vector>
#include "SdStore.h"
#include "PersistOverlay.h"
#include "SdhciRegs.h"
struct IOLock {};
static IOLock lock;
static void IOLockLock(IOLock *) {}
static void IOLockUnlock(IOLock *) {}
static void IODelay(unsigned) {}
static struct {IOLock *lock;bool identified;uint64_t blocks;uint32_t rca;} gSd={&lock,true,65536,0x10000};
static uint64_t gLogFirstLba=65536-8192;
struct SdWriteWindow{uint64_t first=0,end=0;};
static SdWriteWindow gSdWriteWindow[2];
static unsigned mode=0,reset=0,writes=0,logs=0,flushes=0;
static char lastLog[256];
volatile uint64_t native_sd_write_bytes=0,native_sd_write_us=0;
static void sdLogf(const char *f,...) __attribute__((format(printf,1,2)));
static void sdLogf(const char *f,...) {va_list a;va_start(a,f);vsnprintf(lastLog,sizeof lastLog,f,a);va_end(a);++logs;}
static bool sdFlushLog() {++flushes;return true;}
static bool failWrite=false,failReadback=false,cardBusy=false,failCmd=false;
static char identity[37]="00112233-4455-6677-8899-aabbccddeeff";
static std::map<uint64_t,std::vector<uint8_t>> disk;
extern "C" {uint8_t native_ram_block_seed_sha256[32]={1};}
static bool PE_parse_boot_argn(const char *n,void *v,unsigned bytes) {
    if(!strcmp(n,"odin-data-guid")){memset(v,0,bytes);memcpy(v,identity,sizeof identity);return true;}
    if(!strcmp(n,"sheng-data-persist")){memcpy(v,&mode,bytes);return true;}
    if(!strcmp(n,"sheng-data-reset")){memcpy(v,&reset,bytes);return true;}return false;
}
static bool sdReadBlocks(uint64_t b,uint32_t n,uint8_t *out) {
    if(b>=gSd.blocks || n>gSd.blocks-b)return false;
    for(unsigned i=0;i<n;++i) {
        auto p=disk.find(b+i);if(p==disk.end())memset(out+i*512,0,512);else memcpy(out+i*512,p->second.data(),512);
    }
    if(failReadback && b>=4096 && writes)out[0]^=1;
    return true;
}
static bool sdTransfer(bool read,uint64_t b,uint32_t n,uint8_t *p) {
    assert(!read);++writes;
    // The real sdTransfer() fence: a write outside the pinned Data window never reaches the card.
    assert(gSdWriteWindow[1].end>gSdWriteWindow[1].first && b>=gSdWriteWindow[1].first && b+n<=gSdWriteWindow[1].end);
    if(failWrite)return false;
    for(unsigned i=0;i<n;++i)disk[b+i]=std::vector<uint8_t>(p+i*512,p+(i+1)*512);
    return true;
}
static uint32_t sdCommand(unsigned command,uint32_t,unsigned,bool,bool,bool,unsigned,uint32_t r[4]) {
    assert(command==13);r[3]=cardBusy?0xe00:0x900;return failCmd?0:SDHCI_INT_RESPONSE;
}
#include "StoreImplementation.h"
static void put32(uint8_t *p,uint32_t v){for(unsigned i=0;i<4;++i)p[i]=uint8_t(v>>(8*i));}
static void put64(uint8_t *p,uint64_t v){put32(p,uint32_t(v));put32(p+4,uint32_t(v>>32));}
static void install(bool overlap=false,bool duplicate=false) {
    disk.clear();dataRegion={};dataBlocks=0;dataPoisoned=false;gStoreWrites=0;gSdWriteWindow[1]={};writes=0;mode=2;reset=0;
    failWrite=failReadback=cardBusy=failCmd=false;
    uint8_t table[16384]={};
    const uint8_t basic[16]={0xa2,0xa0,0xd0,0xeb,0xe5,0xb9,0x33,0x44,0x87,0xc0,0x68,0xb6,0xb7,0x26,0x99,0xc7};
    memcpy(table,basic,16);assert(odin_store::guid(identity,table+16));put64(table+32,4096);put64(table+40,8191);
    const char *name="UniXNUData";for(unsigned i=0;name[i];++i)table[56+i*2]=uint8_t(name[i]);
    if(overlap || duplicate){memcpy(table+128,table,128);if(!duplicate)table[128+16]^=1;}
    for(unsigned i=0;i<32;++i) {
        disk[2+i]=std::vector<uint8_t>(table+i*512,table+(i+1)*512);
        disk[65503+i]=disk[2+i];
    }
    for(unsigned backup=0;backup<2;++backup) {
        uint8_t h[512]={};memcpy(h,"EFI PART",8);put32(h+8,0x10000);put32(h+12,92);
        put64(h+24,backup?65535:1);put64(h+32,backup?1:65535);put64(h+40,34);put64(h+48,65502);
        h[56]=1;put64(h+72,backup?65503:2);put32(h+80,128);put32(h+84,128);
        put32(h+88,odin_store::crc(table,sizeof table));put32(h+16,odin_store::crc(h,92));
        disk[backup?65535:1]=std::vector<uint8_t>(h,h+512);
    }
}
static bool open(){uint64_t first=0;const char *why="";return native_ufs_store_open(130,&first,&why);}
int main() {
    uint8_t id[16];assert(odin_store::guid(identity,id));assert(id[0]==0x33 && id[4]==0x55 && id[6]==0x77);
    assert(!odin_store::guid("00112233445566778899aabbccddeeff",id));assert(!odin_store::guid("00000000-0000-0000-0000-000000000000",id));
    install();mode=0;assert(!open() && !writes);mode=1;assert(open() && !writes && gSdWriteWindow[1].end==gSdWriteWindow[1].first); // mode 1 never opens a write window
    uint8_t p[4096];memset(p,0x5a,sizeof p);assert(!native_ufs_store_write(1,1,p) && !writes);
    install();assert(!native_ufs_store_gpt_check() && open());assert(dataRegion.first==4096);
    assert(gSdWriteWindow[1].first==4096 && gSdWriteWindow[1].end==4096+130*8); // exactly the pinned partition prefix
    assert(native_ufs_store_write(2,1,p));uint8_t out[4096];assert(native_ufs_store_read(2,1,out) && !memcmp(p,out,4096));
    assert(native_ufs_store_sync() && !native_ufs_store_gpt_check());
    assert(!native_ufs_store_write(130,1,p));unsigned before=writes;assert(!native_ufs_store_write(1,1,p) && writes==before);
    install();disk[1][16]^=1;assert(!open() && !writes);
    install();disk[65503][0]^=1;assert(!open() && !writes);
    install(true);assert(!open() && !writes);
    install(false,true);assert(!open() && !writes);
    install();identity[0]='9';assert(!open());identity[0]='0';
    install();disk[4096]=std::vector<uint8_t>(512,0x33);assert(!open() && !writes);
    // The card log is the only visible channel: open, first write, every failure and the poisoned state are reported.
    install();logs=flushes=0;assert(open() && logs==1 && flushes==1 && strstr(lastLog,"write window OPEN"));
    assert(native_ufs_store_write(2,1,p) && logs==2 && strstr(lastLog,"1 writes ok"));assert(native_ufs_store_write(3,1,p) && logs==2);
    install();mode=1;logs=flushes=0;assert(open() && strstr(lastLog,"closed (inspection)"));
    install();identity[0]='9';logs=0;assert(!open() && logs==1 && strstr(lastLog,"refused"));identity[0]='0';
    install();assert(open());failWrite=true;logs=0;assert(!native_ufs_store_write(2,1,p) && gSdWriteWindow[1].end==gSdWriteWindow[1].first && logs==1 && strstr(lastLog,"FAILED"));failWrite=false;before=writes;
    assert(!native_ufs_store_write(2,1,p) && writes==before);
    install();assert(open());failReadback=true;assert(!native_ufs_store_write(2,1,p) && dataPoisoned);
    install();assert(open());cardBusy=true;assert(!native_ufs_store_write(2,1,p) && dataPoisoned);
    install();assert(open());failCmd=true;assert(!native_ufs_store_sync() && dataPoisoned);
    install();assert(open());disk[65535][16]^=1;assert(native_ufs_store_gpt_check() && dataPoisoned && gSdWriteWindow[1].end==gSdWriteWindow[1].first);
    // The exact NativeRamBlock overlay format survives a simulated reboot of
    // this actual store adapter; seed mismatch refuses instead of erasing it.
    install();assert(open());
    struct Ops {
        bool read(uint64_t b,uint32_t n,void *d){return native_ufs_store_read(b,n,d);}
        bool write(uint64_t b,uint32_t n,const void *s){return native_ufs_store_write(b,n,s);}
        bool sync(){return native_ufs_store_sync();}
    } ops;
    uint8_t bitmap[4096];persist::Overlay overlay;
    assert(overlay.open(ops,128,native_ram_block_seed_sha256,false,bitmap)==persist::Open::Fresh);
    assert(overlay.writePage(ops,13,p));
    dataBlocks=0;dataRegion={};dataPoisoned=false;assert(open());
    persist::Overlay resumed;uint8_t bitmap2[4096];
    assert(resumed.open(ops,128,native_ram_block_seed_sha256,false,bitmap2)==persist::Open::Resumed && resumed.has(13));
    assert(ops.read(persist::pageBlock(128,13),1,out) && !memcmp(p,out,sizeof p));
    dataBlocks=0;native_ram_block_seed_sha256[0]=2;assert(!open());reset=1;assert(open());
    native_ram_block_seed_sha256[0]=1;
    puts("PASS: actual SD store ABI, GPT copies/CRCs/GUID/overlap, modes0/1, range, readback, busy, timeout, poison; host only");
}
