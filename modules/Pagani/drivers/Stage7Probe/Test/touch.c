// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../FocalTech.h"
#include "../GeniI2c.h"
static void point(uint8_t raw[63],unsigned slot,unsigned id,unsigned event,unsigned x,unsigned y) {
    uint8_t *p=raw+3+slot*6;
    p[0]=(uint8_t)((event<<6)|(x>>8));p[1]=(uint8_t)x;
    p[2]=(uint8_t)((id<<4)|(y>>8));p[3]=(uint8_t)y;p[4]=1;p[5]=0x10;
}
static void clear(uint8_t raw[63],unsigned count) {memset(raw,255,63);raw[0]=raw[1]=0;raw[2]=(uint8_t)count;}
typedef struct { uint32_t reg[1024];unsigned commands,writes,reads,abort;uint8_t data[63];unsigned size,pos;int nack,timeout; } model;
static uint32_t read32(void *c,uint32_t at) {
    model *m=c;++m->reads;
    if (at==SE_GENI_M_IRQ_STATUS) {
        if (m->abort) return GENI_M_CMD_ABORT_DONE;
        if (m->timeout) return 0;
        return m->nack?(1u<<10):GENI_M_CMD_DONE;
    }
    if (at==SE_GENI_RX_FIFO_STATUS && m->commands && (m->reg[SE_GENI_M_CMD0/4]>>27)==2) {
        unsigned left=m->size-m->pos;
        return ((left+3)/4)|(left?GENI_RX_LAST|((left%4)<<28):0);
    }
    if (at==SE_GENI_RX_FIFOn) {
        uint32_t v=0;for (unsigned b=0;b<4 && m->pos<m->size;++b) v|=(uint32_t)m->data[m->pos++]<<(8*b);
        return v;
    }
    return m->reg[at/4];
}
static void write32(void *c,uint32_t at,uint32_t value) {
    model *m=c;++m->writes;m->reg[at/4]=value;
    if (at==SE_GENI_M_CMD0) ++m->commands;
    if (at==SE_GENI_M_CMD_CTRL_REG) {assert(value==GENI_M_CMD_ABORT);++m->abort;}
}
static void delay(void *c,unsigned n) {(void)c;assert(n==10);}
static odin_i2c bus(model *m) {
    odin_i2c b={.io={m,read32,write32,delay},.limit=4};
    m->reg[GENI_FW_REVISION_RO/4]=3<<8;m->reg[SE_HW_PARAM_0/4]=(32u<<24)|(16u<<16);
    assert(!odin_i2c_adopt(&b) && !m->writes);return b;
}
int main(void) {
    uint8_t raw[63],report[62];odin_ft_frame frame={0},old={0};
    clear(raw,1);point(raw,0,9,0,0,0);
    assert(!odin_ft_decode(raw,63,1,&frame));assert(frame.count==1 && frame.contact[9].x==1919 && !frame.contact[9].y);
    odin_ft_report(&frame,&old,report);assert(report[0]==3 && report[1]==3 && report[2]==9 && report[3]==255 && report[4]==127 && report[61]==1);
    old=frame;clear(raw,1);point(raw,0,9,2,1079,1919);
    assert(!odin_ft_decode(raw,63,1,&frame) && !frame.contact[9].x && frame.contact[9].y==1079);
    odin_ft_report(&frame,&old,report);assert(report[2]==9 && report[5]==255 && report[6]==127);
    assert(!odin_ft_decode(raw,63,3,&frame) && frame.contact[9].x==1919 && !frame.contact[9].y);
    old=frame;clear(raw,0);assert(!odin_ft_decode(raw,63,1,&frame));
    odin_ft_report(&frame,&old,report);assert(report[1]==2 && report[2]==9 && report[61]==1);
    odin_ft_report(&frame,&frame,report);assert(!report[61]);
    clear(raw,10);for (unsigned i=0;i<10;++i) point(raw,i,i,0,100*i,150*i);
    assert(!odin_ft_decode(raw,63,1,&frame) && frame.count==10);
    old=frame;clear(raw,1);point(raw,0,9,2,100,200);assert(!odin_ft_decode(raw,63,1,&frame));
    odin_ft_report(&frame,&old,report);assert(report[61]==10 && report[55]==3 && report[56]==9);
    // Bad packets cannot publish partial contact sets.
    old=frame;clear(raw,2);point(raw,0,1,0,10,20);point(raw,1,1,2,30,40);
    assert(odin_ft_decode(raw,63,1,&frame)<0 && !memcmp(&old,&frame,sizeof old));
    point(raw,1,2,2,1080,0);assert(odin_ft_decode(raw,63,1,&frame)==-34);
    point(raw,1,2,3,0,0);assert(odin_ft_decode(raw,63,1,&frame)<0);
    clear(raw,11);assert(odin_ft_decode(raw,63,1,&frame)<0);
    clear(raw,1);assert(odin_ft_decode(raw,63,1,&frame)<0); // truncated contact list
    clear(raw,0);raw[0]=0x40;assert(odin_ft_decode(raw,63,1,&frame)<0);
    for (unsigned n=0;n<63;++n) assert(odin_ft_decode(raw,n,1,&frame)==-22);
    // FIFO adoption refuses SPI, GPI-only, active or dirty state without writes.
    for (unsigned mode=0;mode<5;++mode) {
        model m={0};odin_i2c b=bus(&m);
        unsigned reg=mode==0?GENI_FW_REVISION_RO:mode==1?GENI_IF_DISABLE_RO:mode==2?SE_GENI_DMA_MODE_EN:mode==3?SE_GSI_EVENT_EN:SE_GENI_STATUS;
        m.reg[reg/4]=1;assert(odin_i2c_adopt(&b)<0 && !m.writes);
    }
    for (unsigned n=1;n<=63;++n) { // tails, repeated start and full frame
        model m={0};odin_i2c b=bus(&m);m.size=n;
        for (unsigned i=0;i<n;++i) m.data[i]=(uint8_t)(i^0x5a);
        memset(raw,0,63);assert(!odin_i2c_read_reg(&b,0,raw,n) && m.commands==2 && !memcmp(raw,m.data,n));
        assert(m.reg[SE_GENI_M_CMD0/4]==((2u<<27)|(0x38<<9)));
    }
    for (unsigned mode=0;mode<2;++mode) {
        model m={0};odin_i2c b=bus(&m);m.nack=!mode;m.timeout=mode;
        assert(odin_i2c_read_reg(&b,0,raw,1)==(mode?-110:-5) && b.poisoned && m.abort==1);
        unsigned writes=m.writes;assert(odin_i2c_read_reg(&b,0,raw,1)<0 && m.writes==writes);
    }
    puts("PASS: FT coherent frames, 10 IDs/down/move/up, inverse rotation, HID ranges/releases, FIFO guards/NACK/timeout/poison/tails");
}
