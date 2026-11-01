// SPDX-License-Identifier: BSD-2-Clause-Patent
#pragma once
#include <stdint.h>
#include <string.h>

// A dedicated microSD partition, not free space guessed from a root image.
// This parser never writes. Caller serializes I/O and supplies two 16 KiB tables.
namespace odin_store {
constexpr unsigned kTableBytes = 128 * 128;
inline uint32_t le32(const uint8_t *p) {
    uint32_t v=0;for(unsigned i=0;i<4;++i)v|=uint32_t(p[i])<<(8*i);return v;
}
inline uint64_t le64(const uint8_t *p) {
    return uint64_t(le32(p))|(uint64_t(le32(p+4))<<32);
}
inline uint32_t crc(const uint8_t *p,unsigned bytes) {
    uint32_t v=~0u;for(unsigned i=0;i<bytes;++i){v^=p[i];for(unsigned b=0;b<8;++b)v=(v>>1)^(0xedb88320u&(0u-(v&1)));}return ~v;
}
inline bool nonzero(const uint8_t *p,unsigned bytes) {
    for(unsigned i=0;i<bytes;++i)if(p[i])return true;
    return false;
}
inline bool guid(const char *s,uint8_t out[16]) {
    if(!s || strlen(s)!=36)return false;
    uint8_t network[16];unsigned n=0,half=0;
    for(unsigned i=0;i<36;++i) {
        if(i==8 || i==13 || i==18 || i==23){if(s[i]!='-')return false;continue;}
        unsigned v=s[i]>='0'&&s[i]<='9'?unsigned(s[i]-'0'):
            s[i]>='a'&&s[i]<='f'?unsigned(s[i]-'a'+10):
            s[i]>='A'&&s[i]<='F'?unsigned(s[i]-'A'+10):16;
        if(v==16)return false;
        if(!half)network[n]=uint8_t(v<<4);else network[n++]|=uint8_t(v);
        half^=1;
    }
    const unsigned order[16]={3,2,1,0,5,4,7,6,8,9,10,11,12,13,14,15};
    for(unsigned i=0;i<16;++i)out[i]=network[order[i]];
    return nonzero(out,16);
}
struct Region {
    uint64_t first=0,sectors=0;
    uint32_t primaryCrc=0,backupCrc=0,entriesCrc=0;
    bool same(const Region &r) const {
        return first==r.first && sectors==r.sectors && primaryCrc==r.primaryCrc &&
            backupCrc==r.backupCrc && entriesCrc==r.entriesCrc;
    }
};
inline bool header(uint8_t h[512],uint64_t self,uint64_t other,uint64_t sectors) {
    unsigned size=le32(h+12);
    if(memcmp(h,"EFI PART",8) || le32(h+8)!=0x10000 || size<92 || size>512 || le32(h+20) ||
       le64(h+24)!=self || le64(h+32)!=other || le64(h+40)<34 ||
       le64(h+40)>le64(h+48) || le64(h+48)>=sectors-33 ||
       !le32(h+80) || le32(h+80)>128 || le32(h+84)!=128)return false;
    uint32_t saved=le32(h+16);memset(h+16,0,4);uint32_t actual=crc(h,size);
    for(unsigned i=0;i<4;++i)h[16+i]=uint8_t(saved>>(8*i));
    return saved==actual;
}
inline bool name(const uint8_t *entry,const char *text) {
    unsigned i=0;for(;text[i] && i<36;++i)if(entry[56+2*i]!=uint8_t(text[i]) || entry[57+2*i])return false;
    return i<36 && !entry[56+2*i] && !entry[57+2*i];
}
template<class IO> bool locate(IO &io,uint64_t sectors,uint64_t logFirst,const uint8_t expected[16],
    Region &out,uint8_t *table,uint8_t *backup,const char **why) {
    out={};*why="invalid Data GPT";
    if(sectors<128 || !nonzero(expected,16) || !table || !backup)return false;
    uint8_t p[512],b[512];
    if(!io.read(1,1,p) || !io.read(sectors-1,1,b) || !header(p,1,sectors-1,sectors) ||
       !header(b,sectors-1,1,sectors))return false;
    if(!nonzero(p+56,16) || memcmp(p+40,b+40,32) || memcmp(p+80,b+80,12))return false;
    unsigned count=le32(p+80),bytes=count*128,blocks=(bytes+511)/512;
    uint64_t pt=le64(p+72),bt=le64(b+72),firstUsable=le64(p+40),lastUsable=le64(p+48);
    if(pt<2 || pt>=firstUsable || blocks>firstUsable-pt || bt<=lastUsable ||
       bt>=sectors-1 || blocks>sectors-1-bt ||
       !io.read(pt,blocks,table) || !io.read(bt,blocks,backup) || memcmp(table,backup,bytes) ||
       crc(table,bytes)!=le32(p+88))return false;
    const uint8_t basic[16]={0xa2,0xa0,0xd0,0xeb,0xe5,0xb9,0x33,0x44,0x87,0xc0,0x68,0xb6,0xb7,0x26,0x99,0xc7};
    unsigned matches=0;
    for(unsigned i=0;i<count;++i) {
        const uint8_t *e=table+i*128;if(!nonzero(e,16))continue;
        uint64_t lo=le64(e+32),hi=le64(e+40);
        if(!nonzero(e+16,16) || lo<firstUsable || hi<lo || hi>lastUsable)return false;
        for(unsigned j=0;j<i;++j) {
            const uint8_t *q=table+j*128;if(!nonzero(q,16))continue;
            if(!memcmp(e+16,q+16,16) || (lo<=le64(q+40) && le64(q+32)<=hi))return false;
        }
        if(name(e,"UniXNUData")) {
            ++matches;
            if(memcmp(e,basic,16) || memcmp(e+16,expected,16) || le64(e+48) ||
               (lo&7) || ((hi-lo+1)&7) || !logFirst || hi>=logFirst)return false;
            out.first=lo;out.sectors=hi-lo+1;
        }
    }
    if(matches!=1){*why="need one GUID-pinned UniXNUData partition";out={};return false;}
    out.primaryCrc=le32(p+16);out.backupCrc=le32(b+16);out.entriesCrc=le32(p+88);
    *why="";return true;
}
inline bool range(const Region &r,uint64_t windowBlocks,uint64_t rel,uint32_t blocks,uint64_t *sector) {
    if(!blocks || !windowBlocks || windowBlocks>r.sectors/8 || rel>=windowBlocks || blocks>windowBlocks-rel)return false;
    *sector=r.first+rel*8;return true;
}
// Do not let the generic overlay's automatic stale-seed reset silently erase
// a different Data volume. An existing overlay requires the exact seed/size,
// unless the operator explicitly selected sheng-data-reset=1/2.
inline bool seedCompatible(const uint8_t h[4096],uint64_t blocks,const uint8_t seed[32],bool reset) {
    if(!nonzero(h,4096))return true;
    if(memcmp(h,"SHNGDATA",8))return false;
    if(reset)return true;
    if(le32(h+8)!=1 || le32(h+12)!=64 || le32(h+60)!=crc(h,60))return false;
    uint64_t pages=le64(h+16);
    return pages && pages<blocks && 1+(pages+32767)/32768+pages==blocks && !memcmp(h+24,seed,32);
}
} // namespace odin_store
