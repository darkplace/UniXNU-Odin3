// SPDX-License-Identifier: BSD-2-Clause-Patent
// Odin-only MAX31760 fan provider on qupv3_se3_i2c (0x50), GPIO182 enable. Opt-in: odin-fan=0..3.
// Retained-state path like OdinTouch: no clock vote, no regulator (BOB1/L15B) control, GENI firmware
// must already be I2C. Sequence and constants: official max31760_fan.ko (see FanMax31760.h).
#include <IOKit/IOService.h>
#include <IOKit/IODeviceMemory.h>
#include <IOKit/IOWorkLoop.h>
#include <IOKit/IOTimerEventSource.h>
#include <pexpert/pexpert.h>
#include "FanMax31760.h"
#include "GeniI2c.h"
#include "../BootProgress/PeripheralStatus.h"

struct OdinFanMap {
    IOMemoryMap *mapping=nullptr;
    bool map(uint64_t physical,uint64_t size) {
        IODeviceMemory *memory=IODeviceMemory::withRange(physical,size);
        mapping=memory?memory->map(kIOMapAnywhere|kIOMapInhibitCache):nullptr;
        OSSafeReleaseNULL(memory);return mapping && mapping->getVirtualAddress();
    }
    uint32_t read(uint32_t offset) const {return *(volatile uint32_t *)(mapping->getVirtualAddress()+offset);}
    void write(uint32_t offset,uint32_t value) const {
        *(volatile uint32_t *)(mapping->getVirtualAddress()+offset)=value;__asm__ volatile("dsb sy" ::: "memory");
    }
    void release() {OSSafeReleaseNULL(mapping);}
};

class OdinFan : public IOService {
    OSDeclareDefaultStructors(OdinFan)
    IOWorkLoop *loop=nullptr;
    IOTimerEventSource *timer=nullptr;
    bool added=false,running=false,powered=false;
    unsigned mode=0,state=0,ticks=0;
    int lastError=0;
    OdinFanMap gcc,se,pins;
    odin_i2c bus={};
    odin_fan_report report={};
    char text[96];
    static uint32_t read(void *c,uint32_t at) {return static_cast<OdinFan *>(c)->se.read(at);}
    static void write(void *c,uint32_t at,uint32_t value) {static_cast<OdinFan *>(c)->se.write(at,value);}
    static void delay(void *,unsigned us) {IODelay(us);}
    static int busRead(void *c,uint8_t reg,uint8_t *out) {return odin_i2c_read_reg_at(&static_cast<OdinFan *>(c)->bus,ODIN_FAN_ADDR,reg,out,1);}
    static int busWrite(void *c,uint8_t reg,uint8_t value) {return odin_i2c_write_reg_at(&static_cast<OdinFan *>(c)->bus,ODIN_FAN_ADDR,reg,value);}
    void line(unsigned index) {
        boot_progress_fan(index,text);IOLog("Odin %s\n",text);
        boot_progress_peripheral_enable(1);
    }
    uint32_t pin(unsigned gpio,unsigned reg) {return pins.read(gpio*0x1000+reg);}
    void power(bool on) {
        // TLMM: function 0 (GPIO) in ctl[5:2], OE ctl[9], output level io[1]. Level before OE so the pin never glitches low.
        uint32_t ctl=pin(ODIN_FAN_PWR_GPIO,0);
        uint32_t io=pin(ODIN_FAN_PWR_GPIO,4);
        pins.write(ODIN_FAN_PWR_GPIO*0x1000+4,on?(io|2u):(io&~2u));
        pins.write(ODIN_FAN_PWR_GPIO*0x1000,(ctl&~(15u<<2))|(1u<<9));
        powered=on;
    }
    int inspect() {
        if (!gcc.map(0x100000,0x60000)) return -12;
        uint32_t clk=gcc.read(ODIN_FAN_GCC_S3_CBCR);
        // CLK_OFF set (or an all-ones read): touching the engine would hang the bus. No vote is cast here.
        bool clocked=clk!=0xffffffffu && !(clk&(1u<<31));
        const unsigned common[]={0x2315c,0x23148,0x23140,0x23144};
        for (unsigned at:common) {uint32_t v=gcc.read(at);if (v==0xffffffffu || (v&(1u<<31))) clocked=false;}
        if (!pins.map(0xf100000,0xb7000)) return -12;
        uint32_t sda=pin(ODIN_FAN_SDA_GPIO,0),scl=pin(ODIN_FAN_SCL_GPIO,0),gpio=pin(ODIN_FAN_PWR_GPIO,0);
        odin_fan_line_inspect(text,sizeof text,mode,clk,sda,scl,gpio,(pin(ODIN_FAN_PWR_GPIO,4)>>1)&1);
        line(0);
        if (!clocked) return -19;
        if (((sda>>2)&15)!=1 || ((scl>>2)&15)!=1) return -71; // qup1_se3 function must already be muxed
        if (!se.map(ODIN_FAN_SE3_BASE,0x4000)) return -12;
        bus.io={this,read,write,delay};bus.limit=2000;
        return odin_i2c_adopt(&bus);
    }
    void tick() {
        odin_fan_report now=report;
        const odin_fan_bus io={this,busWrite,busRead};
        int rc=odin_fan_read(&io,&now);
        if (!rc) {report=now;lastError=0;} else lastError=rc;
        odin_fan_line_run(text,sizeof text,state,&report,ticks,lastError);line(2);
    }
    static void poll(OSObject *owner,IOTimerEventSource *) {
        auto self=static_cast<OdinFan *>(owner);
        if (!__atomic_load_n(&self->running,__ATOMIC_ACQUIRE)) return;
        // 120 samples x 2 s: enough to watch spin-up/steady tach, then stop touching the bus.
        if (++self->ticks>120 || self->bus.poisoned) {__atomic_store_n(&self->running,false,__ATOMIC_RELEASE);return;}
        self->tick();
        self->timer->setTimeoutMS(2000);
    }
    void cleanup() {
        __atomic_store_n(&running,false,__ATOMIC_RELEASE);
        if (timer) {timer->cancelTimeout();if (added) loop->removeEventSource(timer);}
        added=false;OSSafeReleaseNULL(timer);OSSafeReleaseNULL(loop);
        gcc.release();se.release();pins.release();
    }
public:
    void select(unsigned requested) {mode=odin_fan_mode(requested);}
    bool start(IOService *provider) override {
        if (!mode) return false;
        int rc=inspect();
        if (rc) {snprintf(text,sizeof text,"FAN: STOP inspect rc=%d (no write done)",rc);line(1);cleanup();return false;}
        if (mode==1) {snprintf(text,sizeof text,"FAN: inspect OK, GENI I2C idle, no write");line(1);cleanup();return false;}
        unsigned pct=0,arg=0;
        if (mode==3 && PE_parse_boot_argn("odin-fan-pct",&arg,sizeof arg)) pct=odin_fan_percent(arg);
        else if (mode==3) pct=50;
        state=pct;
        odin_i2c_configure(&bus);
        power(true);IODelay(20000); // official probe: GPIO high, 20-20.1 ms, then I2C
        const odin_fan_bus io={this,busWrite,busRead};
        rc=odin_fan_start(&io,state,&report);
        odin_fan_line_probe(text,sizeof text,&report);line(1);
        if (rc) {power(false);cleanup();return false;} // fail closed: unpower, as the official remove path does
        IOService::start(provider);
        loop=IOWorkLoop::workLoop();
        timer=loop?IOTimerEventSource::timerEventSource(this,poll):nullptr;
        added=timer && loop->addEventSource(timer)==kIOReturnSuccess;
        if (!added) {cleanup();return true;} // chip is programmed; just no periodic readback
        tick();__atomic_store_n(&running,true,__ATOMIC_RELEASE);timer->setTimeoutMS(2000);return true;
    }
    void stop(IOService *provider) override {
        if (powered && bus.limit) {busWrite(this,ODIN_FAN_REG_PWM,0);power(false);}
        cleanup();IOService::stop(provider);
    }
    void free() override {cleanup();IOService::free();}
};
OSDefineMetaClassAndStructors(OdinFan,IOService)
extern "C" IOService *odin_fan_allocate(unsigned mode) {
    auto service=new OdinFan;if (service) service->select(mode);return service;
}
