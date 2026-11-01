// SPDX-License-Identifier: BSD-2-Clause-Patent
// Host test of the MAX31760 protocol/curve against a fake chip and the real I2C helpers' signatures.
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../FanMax31760.h"
typedef struct { uint8_t reg[256]; int present,log_n,fail_at; struct {int w;uint8_t r,v;} log[32]; } chip;
static int rd(void *c,uint8_t r,uint8_t *v) {chip *k=c; if (!k->present) return -6; *v=k->reg[r]; return 0;}
static int wr(void *c,uint8_t r,uint8_t v) {
    chip *k=c; if (!k->present) return -6;
    if (k->fail_at==k->log_n) return -5;
    k->log[k->log_n].w=1;k->log[k->log_n].r=r;k->log[k->log_n++].v=v;k->reg[r]=v;return 0;
}
int main(void) {
    // Official formula vs integer percent: 0->0, 50->127, 100->255, monotonic, rejects >100.
    assert(odin_fan_duty(0)==0 && odin_fan_duty(50)==127 && odin_fan_duty(100)==255 && odin_fan_duty(101)<0);
    for (unsigned s=1;s<=100;++s) {assert(odin_fan_duty(s)>=odin_fan_duty(s-1)); assert(abs(odin_fan_duty(s)-(int)(255*s/100))<=1);}
    assert(odin_fan_rpm(0,0)==0 && odin_fan_rpm(0x02,0x00)==(6000000u/512)/2 && odin_fan_rpm(0xff,0xff)==45);
    assert(odin_fan_mode(0)==0 && odin_fan_mode(3)==3 && odin_fan_mode(4)==0 && odin_fan_mode(0xffffffffu)==0);
    assert(odin_fan_percent(0)==50 && odin_fan_percent(101)==50 && odin_fan_percent(75)==75);
    // Curve: single official trip with 1 C hysteresis, state 50.
    assert(odin_fan_curve(94999,0)==0 && odin_fan_curve(95000,0)==50 && odin_fan_curve(94500,50)==50 &&
           odin_fan_curve(94000,50)==0 && odin_fan_curve(-1000,50)==0);
    chip k={.present=1,.fail_at=-1};k.reg[ODIN_FAN_REG_TACH_HI]=0x01;k.reg[ODIN_FAN_REG_TACH_LO]=0x00;
    odin_fan_bus bus={&k,wr,rd};odin_fan_report r={0};
    assert(!odin_fan_start(&bus,50,&r) && r.rc==0);
    assert(k.log_n==4 && k.log[0].r==0 && k.log[0].v==0x19 && k.log[1].r==1 && k.log[1].v==0x11 &&
           k.log[2].r==2 && k.log[2].v==0x31 && k.log[3].r==0x50 && k.log[3].v==127);
    assert(r.cr1==0x19 && r.cr2==0x11 && r.cr3==0x31 && r.pwm==127 && r.rpm==(6000000u/256)/2 && !strcmp(r.step,"readback"));
    // No ACK: nothing written, step names the cause.
    chip dead={.present=0,.fail_at=-1};odin_fan_bus nb={&dead,wr,rd};memset(&r,0,sizeof r);
    assert(odin_fan_start(&nb,50,&r)==-6 && !dead.log_n && !strcmp(r.step,"ACK probe"));
    // Write fault midway stops the sequence before PWM.
    chip bad={.present=1,.fail_at=2};odin_fan_bus bb={&bad,wr,rd};
    assert(odin_fan_start(&bb,50,&r)==-5 && bad.log_n==2 && !strcmp(r.step,"write CR3") && bad.reg[0x50]==0);
    assert(odin_fan_start(&bus,101,&r)<0);
    char line[160];
    odin_fan_line_inspect(line,sizeof line,2,0x80000000u,0x1234,0x1236,0x2200,1);assert(strlen(line)<=79);
    odin_fan_line_probe(line,sizeof line,&r);assert(strlen(line)<=79);
    r.rpm=99999;odin_fan_line_run(line,sizeof line,100,&r,119,-110);assert(strlen(line)<=79);
    puts("PASS: MAX31760 protocol, curve, lines");return 0;
}
