// SPDX-License-Identifier: BSD-2-Clause-Patent
// Bounded FIFO-only I2C adapter using QcomGeniSpi's common register/IO definitions.
// Values/command semantics: AYN Linux i2c-qcom-geni.c, pinned in the touch report.
// No microcode, clock, GPIO, DMA, SMMU or sensor firmware programming here.
#ifndef ODIN_GENI_I2C_H
#define ODIN_GENI_I2C_H
#include "FocalTech.h"
#include <GeniSpi.h>
#define ODIN_I2C_TX_LEN 0x26c
#define ODIN_I2C_RX_LEN 0x270
#define ODIN_I2C_STOP_STRETCH (1u<<2)
#define ODIN_I2C_ERRORS (GENI_M_ERRORS | (1u<<10) | (1u<<12) | (1u<<13) | GENI_M_CMD_ABORT_DONE)
typedef struct { struct geni_io io; unsigned limit,depth; int poisoned; uint32_t irq,status; } odin_i2c;

static inline int odin_i2c_adopt(odin_i2c *s) {
    uint32_t fw=s->io.rd(s->io.ctx,GENI_FW_REVISION_RO);
    s->status=s->io.rd(s->io.ctx,SE_GENI_STATUS);
    // GENI protocol I2C = 3 (geni-se.h); retain only idle firmware FIFO mode.
    if (((fw>>8)&255)!=3 || (s->status&0x1001)) return -71;
    if ((s->io.rd(s->io.ctx,GENI_IF_DISABLE_RO)&1) || s->io.rd(s->io.ctx,SE_GENI_DMA_MODE_EN) ||
        s->io.rd(s->io.ctx,SE_GSI_EVENT_EN)) return -95;
    uint32_t hw=s->io.rd(s->io.ctx,SE_HW_PARAM_0);
    s->depth=(hw>>16)&0x3f;
    if (!s->depth || ((hw>>24)&0x3f)!=32 || s->io.rd(s->io.ctx,SE_GENI_TX_FIFO_STATUS) ||
        (s->io.rd(s->io.ctx,SE_GENI_RX_FIFO_STATUS)&GENI_RX_WC_MASK)) return -16;
    return 0;
}

static inline int odin_i2c_fail(odin_i2c *s,int error) {
    s->poisoned=1; // No later transaction, even if abort appears to succeed.
    s->io.wr(s->io.ctx,SE_GENI_M_CMD_CTRL_REG,GENI_M_CMD_ABORT);
    for (unsigned i=0;i<s->limit;++i) {
        uint32_t v=s->io.rd(s->io.ctx,SE_GENI_M_IRQ_STATUS);
        if (v&GENI_M_CMD_ABORT_DONE) {s->io.wr(s->io.ctx,SE_GENI_M_IRQ_CLEAR,v);break;}
        s->io.delay_us(s->io.ctx,10);
    }
    return error;
}

static inline void odin_i2c_configure(odin_i2c *s) {
    // geni_se_config_packing(8,4,true,true,true): byte 0 in FIFO bits 7:0.
    uint32_t a=0xfeu|(0x1feu<<10),b=0x2feu|(0x3ffu<<10);
    s->io.wr(s->io.ctx,SE_GENI_TX_PACKING_CFG0,a);s->io.wr(s->io.ctx,SE_GENI_TX_PACKING_CFG1,b);
    s->io.wr(s->io.ctx,SE_GENI_RX_PACKING_CFG0,a);s->io.wr(s->io.ctx,SE_GENI_RX_PACKING_CFG1,b);
    s->io.wr(s->io.ctx,SE_GENI_BYTE_GRAN,0);
    s->io.wr(s->io.ctx,SE_GENI_M_IRQ_EN,ODIN_I2C_ERRORS|GENI_M_CMD_DONE|GENI_M_RX_FIFO_LAST|GENI_M_RX_FIFO_WATERMARK|GENI_M_TX_FIFO_WATERMARK);
    s->io.wr(s->io.ctx,SE_GENI_RX_WATERMARK_REG,1);
    // GIC SPI 357 is not unmasked; the SE status sources above remain generated.
    s->io.wr(s->io.ctx,SE_IRQ_EN,0);
}

static inline int odin_i2c_message(odin_i2c *s,unsigned addr,const uint8_t *tx,uint8_t *rx,size_t size,int held) {
    if (s->poisoned || !s->limit || addr>127 || !size || size>ODIN_FT_FRAME_BYTES || (!!tx==!!rx)) return -22;
    void *c=s->io.ctx;
    s->io.wr(c,SE_GENI_M_IRQ_CLEAR,0xffffffffu);
    s->io.wr(c,tx?ODIN_I2C_TX_LEN:ODIN_I2C_RX_LEN,(uint32_t)size);
    s->io.wr(c,SE_GENI_M_CMD0,((tx?1u:2u)<<27)|(addr<<9)|(held?ODIN_I2C_STOP_STRETCH:0));
    size_t moved=0;
    for (unsigned i=0;i<s->limit;++i) {
        uint32_t irq=s->io.rd(c,SE_GENI_M_IRQ_STATUS);s->irq=irq;
        s->status=s->io.rd(c,SE_GENI_STATUS);
        if (irq&ODIN_I2C_ERRORS) return odin_i2c_fail(s,-5);
        if (tx && moved<size) {
            unsigned used=s->io.rd(c,SE_GENI_TX_FIFO_STATUS)&0xfffffff;
            if (used>s->depth) return odin_i2c_fail(s,-71);
            for (unsigned free=s->depth-used;free && moved<size;--free) {
                uint32_t word=0;
                for (unsigned b=0;b<4 && moved<size;++b) word|=(uint32_t)tx[moved++]<<(8*b);
                s->io.wr(c,SE_GENI_TX_FIFOn,word);
            }
        }
        if (rx) {
            uint32_t state=s->io.rd(c,SE_GENI_RX_FIFO_STATUS);
            unsigned words=state&GENI_RX_WC_MASK;
            if (words>s->depth || words>(size-moved+3)/4) return odin_i2c_fail(s,-71);
            for (unsigned w=0;w<words;++w) {
                uint32_t word=s->io.rd(c,SE_GENI_RX_FIFOn);
                unsigned bytes=4;
                if (w+1==words && (state&GENI_RX_LAST)) {
                    unsigned valid=(state>>GENI_RX_LAST_VALID_SHIFT)&GENI_RX_LAST_VALID_MASK;
                    if (valid>4) return odin_i2c_fail(s,-71);
                    if (valid) bytes=valid;
                }
                if (bytes>size-moved) return odin_i2c_fail(s,-71);
                for (unsigned b=0;b<bytes;++b) rx[moved++]=(uint8_t)(word>>(8*b));
            }
        }
        if (irq) s->io.wr(c,SE_GENI_M_IRQ_CLEAR,irq);
        if (irq&GENI_M_CMD_DONE) return moved==size?0:odin_i2c_fail(s,-71);
        s->io.delay_us(c,10);
    }
    return odin_i2c_fail(s,-110);
}

// Register access at an arbitrary 7-bit address (SMBus byte semantics: repeated start for reads).
static inline int odin_i2c_read_reg_at(odin_i2c *s,unsigned addr,uint8_t reg,uint8_t *out,size_t size) {
    int rc=odin_i2c_message(s,addr,&reg,0,1,1);
    return rc?rc:odin_i2c_message(s,addr,0,out,size,0);
}
static inline int odin_i2c_write_reg_at(odin_i2c *s,unsigned addr,uint8_t reg,uint8_t value) {
    const uint8_t frame[2]={reg,value};
    return odin_i2c_message(s,addr,frame,0,2,0);
}
static inline int odin_i2c_read_reg(odin_i2c *s,uint8_t reg,uint8_t *out,size_t size) {
    return odin_i2c_read_reg_at(s,0x38,reg,out,size);
}
#endif
