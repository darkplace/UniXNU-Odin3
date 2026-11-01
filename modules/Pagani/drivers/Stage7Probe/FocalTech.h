// SPDX-License-Identifier: BSD-2-Clause-Patent
// Register/report facts: CQ8725S focaltech_core.{c,h}, focaltech_common.h;
// independent bounded implementation. Provenance: docs/builds/odin3-touch-20261029.md.
#ifndef ODIN_FOCALTECH_H
#define ODIN_FOCALTECH_H
#include <stdint.h>
#include <stddef.h>
#define ODIN_FT_CONTACTS 10
#define ODIN_FT_FRAME_BYTES 63
#define ODIN_FT_ID_HIGH 0xa3
#define ODIN_FT_ID_LOW 0x9f
#define ODIN_FT_FW_VERSION 0xa6
#define ODIN_FT_WIDTH 1080u
#define ODIN_FT_HEIGHT 1920u
typedef struct { uint16_t x,y; uint8_t active; } odin_ft_contact;
typedef struct { odin_ft_contact contact[ODIN_FT_CONTACTS]; unsigned count; } odin_ft_frame;

// Input is the complete register window 0x00..0x3e, NOT the vendor driver's
// prefixed buffer (which starts its read at register 0x01). No raw-matrix processing.
static inline int odin_ft_decode(const uint8_t *raw,size_t size,unsigned rotate,odin_ft_frame *out) {
    if (!raw || !out || size!=ODIN_FT_FRAME_BYTES || (rotate!=1 && rotate!=3)) return -22;
    if ((raw[0]&0x70) || (raw[2]&15)>ODIN_FT_CONTACTS) return -71;
    odin_ft_frame next={{{0,0,0}},0}; unsigned seen=0;
    for (unsigned n=0;n<ODIN_FT_CONTACTS;++n) {
        const uint8_t *p=raw+3+6*n;
        unsigned id=p[2]>>4,event=p[0]>>6;
        if (id>=ODIN_FT_CONTACTS) break; // Vendor terminator; zero count may have no records.
        if ((seen&(1u<<id)) || event==3) return -71;
        seen|=1u<<id;
        if (event==1) continue; // Explicit UP; vanished IDs also release in the HID snapshot.
        unsigned px=((p[0]&15u)<<8)|p[1],py=((p[2]&15u)<<8)|p[3];
        if (px>=ODIN_FT_WIDTH || py>=ODIN_FT_HEIGHT) return -34;
        next.contact[id].x=(uint16_t)(rotate==1?ODIN_FT_HEIGHT-1-py:py);
        next.contact[id].y=(uint16_t)(rotate==1?px:ODIN_FT_WIDTH-1-px);
        next.contact[id].active=1; ++next.count;
    }
    if (next.count!=(raw[2]&15u)) return -71;
    *out=next; return 0; // Atomic frame at the caller's work-loop boundary; no partial publication.
}

// Same 62-byte layout and normalised logical range as NativeHidInput 5f3a37c.
// Pack by hardware ID (not by previous report slot). The union of old/new IDs
// fits ten entries, including vanished fingers' explicit UPs.
static inline void odin_ft_report(const odin_ft_frame *now,const odin_ft_frame *previous,uint8_t report[62]) {
    for (unsigned i=0;i<62;++i) report[i]=0;
    report[0]=3; unsigned entries=0;
    for (unsigned id=0;id<ODIN_FT_CONTACTS;++id) {
        const odin_ft_contact *c=&now->contact[id];
        const odin_ft_contact *old=&previous->contact[id];
        if (!c->active && !old->active) continue;
        uint8_t *p=report+1+entries*6; p[1]=(uint8_t)id;
        unsigned x=c->active?c->x:old->x,y=c->active?c->y:old->y;
        x=x*32767u/(ODIN_FT_HEIGHT-1);y=y*32767u/(ODIN_FT_WIDTH-1);
        p[0]=(uint8_t)(c->active?3:2); // confidence remains true through UP.
        p[2]=(uint8_t)x;p[3]=(uint8_t)(x>>8);p[4]=(uint8_t)y;p[5]=(uint8_t)(y>>8);
        ++entries;
    }
    report[61]=(uint8_t)entries;
}
#endif
