// SPDX-License-Identifier: BSD-2-Clause-Patent
// Pure MAX31760 protocol, curve and page-line formatting for the Odin 3 fan.
// Every constant below was read from the official AYN max31760_fan.ko (EDL Odin3_20260616,
// SHA-256 5afae3bc…, docs/builds/odin3-thermal-20261029.md) or its DTBO entry 107.
#ifndef ODIN_FAN_MAX31760_H
#define ODIN_FAN_MAX31760_H
#include <stddef.h>
#include <stdint.h>
#define ODIN_FAN_ADDR 0x50u          // DTBO max31760@50 on qupv3_se3_i2c (i2c@a8c000)
#define ODIN_FAN_SE3_BASE 0xa8c000u  // qupv3_1 SE3, oficial-vb.dts:15969
#define ODIN_FAN_GCC_S3_CBCR 0x18290u // gcc-sm8750.c @c339f401 gcc_qupv3_wrap1_s3_clk halt_reg
#define ODIN_FAN_SDA_GPIO 44u        // qupv3_se3_i2c_sda_active, qup1_se3_l0 (oficial-vb.dts:3279)
#define ODIN_FAN_SCL_GPIO 45u
#define ODIN_FAN_PWR_GPIO 182u       // maxim,pwr-en-gpio <&tlmm 0xb6 1>
#define ODIN_FAN_REG_CR1 0x00u       // probe writes 0x19
#define ODIN_FAN_REG_CR2 0x01u       // probe writes 0x11
#define ODIN_FAN_REG_CR3 0x02u       // fan-num 1 -> 0x31, fan-num 2 -> 0x33
#define ODIN_FAN_REG_PWM 0x50u       // set_cur_state / pwm_duty
#define ODIN_FAN_REG_TACH_HI 0x52u   // speed_tc1_show reads 0x52 then 0x53
#define ODIN_FAN_REG_TACH_LO 0x53u
#define ODIN_FAN_CR1 0x19u
#define ODIN_FAN_CR2 0x11u
#define ODIN_FAN_CR3_ONE 0x31u
#define ODIN_FAN_MAX_STATE 100u
typedef struct { void *ctx; int (*wr)(void *,uint8_t,uint8_t); int (*rd)(void *,uint8_t,uint8_t *); } odin_fan_bus;
typedef struct { int rc; const char *step; uint8_t cr1,cr2,cr3,pwm,tach_hi,tach_lo; unsigned state,rpm; } odin_fan_report;

// max31760_set_cur_state: ((255*state)>>2 & 0x3fff)*0x147b>>17, state <= 100.
static inline int odin_fan_duty(unsigned state) {
    if (state>ODIN_FAN_MAX_STATE) return -22;
    return (int)(((((255u*state)>>2)&0x3fffu)*0x147bu)>>17);
}
// speed_tc1_show: (6000000 / (reg52<<8|reg53)) / 2; 0 when the counter reads 0.
static inline unsigned odin_fan_rpm(uint8_t hi,uint8_t lo) {
    unsigned count=((unsigned)hi<<8)|lo;
    return count?(6000000u/count)/2:0;
}
// Official curve: one trip, 95 C (95000 mC), hysteresis 1 C, cooling state 50; sensor TSENS1 idx1.
static inline unsigned odin_fan_curve(int milli_c,unsigned previous) {
    if (milli_c>=95000) return 50;
    if (previous && milli_c>95000-1000) return previous;
    return 0;
}
// odin-fan: 0 off (default), 1 read-only inspect, 2 power+init, fan at 0, 3 power+init, fixed percent.
static inline unsigned odin_fan_mode(unsigned arg) {return arg>3?0:arg;}
static inline unsigned odin_fan_percent(unsigned arg) {return arg>=1 && arg<=ODIN_FAN_MAX_STATE?arg:50;}

static inline int odin_fan_read(const odin_fan_bus *b,odin_fan_report *r) {
    int rc;
    if ((rc=b->rd(b->ctx,ODIN_FAN_REG_CR1,&r->cr1))) {r->step="read CR1";return rc;}
    if ((rc=b->rd(b->ctx,ODIN_FAN_REG_CR2,&r->cr2))) {r->step="read CR2";return rc;}
    if ((rc=b->rd(b->ctx,ODIN_FAN_REG_CR3,&r->cr3))) {r->step="read CR3";return rc;}
    if ((rc=b->rd(b->ctx,ODIN_FAN_REG_PWM,&r->pwm))) {r->step="read PWM";return rc;}
    if ((rc=b->rd(b->ctx,ODIN_FAN_REG_TACH_HI,&r->tach_hi))) {r->step="read TACH";return rc;}
    if ((rc=b->rd(b->ctx,ODIN_FAN_REG_TACH_LO,&r->tach_lo))) {r->step="read TACH";return rc;}
    r->rpm=odin_fan_rpm(r->tach_hi,r->tach_lo);r->step="readback";return 0;
}
// Same order as the official probe/resume once power is up: CR1, CR2, CR3, then PWM.
// A read of CR1 first doubles as the ACK probe; nothing is written if it fails.
static inline int odin_fan_start(const odin_fan_bus *b,unsigned state,odin_fan_report *r) {
    int rc,duty=odin_fan_duty(state);
    uint8_t first;
    r->rc=0;r->state=state;r->step="invalid state";
    if (duty<0) return r->rc=duty;
    if ((rc=b->rd(b->ctx,ODIN_FAN_REG_CR1,&first))) {r->step="ACK probe";return r->rc=rc;}
    if ((rc=b->wr(b->ctx,ODIN_FAN_REG_CR1,ODIN_FAN_CR1))) {r->step="write CR1";return r->rc=rc;}
    if ((rc=b->wr(b->ctx,ODIN_FAN_REG_CR2,ODIN_FAN_CR2))) {r->step="write CR2";return r->rc=rc;}
    if ((rc=b->wr(b->ctx,ODIN_FAN_REG_CR3,ODIN_FAN_CR3_ONE))) {r->step="write CR3";return r->rc=rc;}
    if ((rc=b->wr(b->ctx,ODIN_FAN_REG_PWM,(uint8_t)duty))) {r->step="write PWM";return r->rc=rc;}
    return r->rc=odin_fan_read(b,r);
}
// Page lines, at most 79 characters (BootProgress row width is 80 including NUL).
static inline void odin_fan_line_inspect(char *o,size_t n,unsigned mode,uint32_t clk,uint32_t sda,uint32_t scl,uint32_t gpio,unsigned level) {
    snprintf(o,n,"FAN: m=%u clk=%08x pin=%03x/%03x pwr=%03x/%x",mode,clk,sda&0xfff,scl&0xfff,gpio&0xfff,level&1);
}
static inline void odin_fan_line_probe(char *o,size_t n,const odin_fan_report *r) {
    snprintf(o,n,"FAN: %.14s rc=%d cr=%02x/%02x/%02x pwm=%02x",r->step?r->step:"?",r->rc,r->cr1,r->cr2,r->cr3,r->pwm);
}
static inline void odin_fan_line_run(char *o,size_t n,unsigned state,const odin_fan_report *r,unsigned tick,int err) {
    snprintf(o,n,"FAN: set=%u%% pwm=%02x tach=%02x%02x rpm=%u err=%d t=%u",state,r->pwm,r->tach_hi,r->tach_lo,r->rpm,err,tick);
}
#endif
