// SPDX-License-Identifier: BSD-2-Clause-Patent
// Odin-only staged FT3519/I2C4 provider. No OnePlus power, reset or bus constants.
// Retained-state path: refuse unclocked pins/engine, GPI-only firmware, or unknown silicon.
#include "OriginalHidDeviceABI.h"
#include <IOKit/IODeviceMemory.h>
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/IOWorkLoop.h>
#include <IOKit/IOTimerEventSource.h>
#include <IOKit/IODeviceTreeSupport.h>
#include <libkern/c++/OSString.h>
#include <pexpert/pexpert.h>
#include "FocalTech.h"
#include "GeniI2c.h"
#include "../PaganiInput/S3910.h" // contact limit used by the ABI-qualified HID event helper only
#include "../PaganiInput/Reports.h"
#include "../PaganiInput/HidEvents.h"
#include "../BootProgress/PeripheralStatus.h"

struct OdinTouchMap {
    IOMemoryMap *mapping=nullptr;
    bool map(uint64_t physical,uint64_t size) {
        IODeviceMemory *memory=IODeviceMemory::withRange(physical,size);
        mapping=memory?memory->map(kIOMapAnywhere|kIOMapInhibitCache):nullptr;
        OSSafeReleaseNULL(memory);return mapping && mapping->getVirtualAddress();
    }
    uint32_t read(uint32_t offset) const {return *(volatile uint32_t *)(mapping->getVirtualAddress()+offset);}
    void release() {OSSafeReleaseNULL(mapping);}
};

class OdinTouch : public IOHIDDevice {
    OSDeclareDefaultStructors(OdinTouch)
    IOWorkLoop *loop=nullptr;
    IOTimerEventSource *timer=nullptr;
    IOService *events=nullptr;
    bool added=false,started=false,running=false;
    unsigned stage=0,rotation=0;
    uint32_t expected=0,identity=0,residentEvidence=0;
    uint64_t reports=0;
    OdinTouchMap gcc,se,reset,irq,sda,scl;
    odin_i2c bus={};
    odin_ft_frame previous={};
    odin_ft_frame pending={};
    bool pendingReport=false,failed=false;
    static uint32_t read(void *c,uint32_t at) {return static_cast<OdinTouch *>(c)->se.read(at);}
    static void write(void *c,uint32_t at,uint32_t value) {
        auto self=static_cast<OdinTouch *>(c);
        *(volatile uint32_t *)(self->se.mapping->getVirtualAddress()+at)=value;
        __asm__ volatile("dsb sy" ::: "memory");
    }
    static void delay(void *,unsigned us) {IODelay(us);}
    void note(const char *text,int result=0,uint32_t value=0) {
        boot_progress_peripheral_enable(1);
        boot_progress_peripheral(PAGANI_TOUCH,text,result,value,reports);
        IOLog("Odin touch: %s rc=%d value=%x reports=%llu\n",text,result,value,reports);
    }
    int inspect() {
        // gcc-sm8750.c: retained QUP1 S4 branch at 0x184e4. No clock vote.
        if (!gcc.map(0x100000,0x60000)) return -12;
        uint32_t clk=gcc.read(0x184e4);note("S4 CBCR",0,clk);
        if (clk==0xffffffffu || (clk&(1u<<31))) return -19;
        const unsigned branches[]={0x2315c,0x23148,0x23140,0x23144};
        for (unsigned at:branches) {
            uint32_t v=gcc.read(at);note("QUP1 CBCR",0,v);
            if (v==0xffffffffu || (v&(1u<<31))) return -19;
        }
        // pinctrl-sm8750.c and Odin DTS: data/clock 48/49, reset 161, IRQ 162.
        if (!sda.map(0xf100000+48*0x1000,0x1000) || !scl.map(0xf100000+49*0x1000,0x1000) ||
            !reset.map(0xf100000+161*0x1000,0x1000) || !irq.map(0xf100000+162*0x1000,0x1000)) return -12;
        note("pins SDA/SCL",0,((sda.read(0)&0xffff)<<16)|(scl.read(0)&0xffff));
        note("reset/irq levels",0,((reset.read(4)&3)<<16)|(irq.read(4)&3));
        // qup1_se4 is function 1; reset must already be deasserted. Do not disturb shared display rails.
        if (((sda.read(0)>>2)&15)!=1 || ((scl.read(0)>>2)&15)!=1 || !(reset.read(4)&1) ||
            ((reset.read(0)>>2)&15) || ((irq.read(0)>>2)&15) || (irq.read(0)&(1u<<9))) return -71;
        if (!se.map(0xa90000,0x4000)) return -12;
        note("GENI firmware",0,se.read(GENI_FW_REVISION_RO));
        bus.io={this,read,write,delay};bus.limit=2000;
        return odin_i2c_adopt(&bus);
    }
    int identify() {
        uint8_t hi=0,lo=0,version=0,mode=0;
        odin_i2c_configure(&bus);
        note("read chip ID before scan");
        int rc=odin_i2c_read_reg(&bus,ODIN_FT_ID_HIGH,&hi,1);
        if (!rc) rc=odin_i2c_read_reg(&bus,ODIN_FT_ID_LOW,&lo,1);
        if (!rc) rc=odin_i2c_read_reg(&bus,ODIN_FT_FW_VERSION,&version,1);
        if (!rc) rc=odin_i2c_read_reg(&bus,0,&mode,1);
        identity=((uint32_t)hi<<8)|lo;
        residentEvidence=(identity<<16)|((uint32_t)version<<8)|mode;
        note("ID + resident FW",rc,residentEvidence);
        setProperty("ChipID",identity,32);setProperty("FirmwareVersion",version,32);
        // The DT's ic-type is NOT the register pair. No guessed 0x3519/0x5452 allowlist.
        if (!rc && (!identity || identity==0xffff || (mode&0x70))) rc=-71;
        return rc;
    }
    bool send(const odin_ft_frame &frame) {
        if (!events) {
            events=pagani_hid::findDriver(this,4);
            if (!events) return false;
            if (*reinterpret_cast<uintptr_t **>(events)!=pagani_original_hid_driver_vtable ||
                pagani_hid::number(events,"PrimaryUsagePage")!=13 || pagani_hid::number(events,"PrimaryUsage")!=4 ||
                !pagani_hid::installDispatch(events)) {OSSafeReleaseNULL(events);return false;}
        }
        uint8_t report[62];odin_ft_report(&frame,&previous,report);
        auto buffer=IOBufferMemoryDescriptor::withBytes(report,sizeof report,kIODirectionNone);
        IOReturn rc=buffer?handleReport(buffer,0,0):kIOReturnNoMemory;
        OSSafeReleaseNULL(buffer);
        if (!rc) {previous=frame;++reports;}
        note("HID frame",rc,frame.count);return rc==kIOReturnSuccess;
    }
    static void tick(OSObject *owner,IOTimerEventSource *) {
        auto self=static_cast<OdinTouch *>(owner);
        if (!__atomic_load_n(&self->running,__ATOMIC_ACQUIRE)) return;
        // Poll ATTN without installing a TLMM irq provider. No false GIC mapping for GPIO162.
        if (!self->failed && !(self->irq.read(4)&1)) {
            uint8_t raw[ODIN_FT_FRAME_BYTES];odin_ft_frame frame={};
            int rc=odin_i2c_read_reg(&self->bus,0,raw,sizeof raw);
            if (!rc) rc=odin_ft_decode(raw,sizeof raw,self->rotation,&frame);
            if (rc) {
                self->note("STOP: frame/bus error",rc,self->bus.irq);
                self->pending={};self->pendingReport=true;self->failed=true;
            } else {
                self->pending=frame;self->pendingReport=true;
            }
        }
        // Retry the latest complete snapshot if the original HID event driver is
        // still matching; do not depend on another edge to retry a failed lift.
        if (self->pendingReport && self->send(self->pending)) self->pendingReport=false;
        if (self->failed && !self->pendingReport) {__atomic_store_n(&self->running,false,__ATOMIC_RELEASE);return;}
        self->timer->setTimeoutMS(8);
    }
    void cleanup() {
        __atomic_store_n(&running,false,__ATOMIC_RELEASE);
        if (timer) {timer->cancelTimeout();if (added) loop->removeEventSource(timer);}
        added=false;OSSafeReleaseNULL(timer);OSSafeReleaseNULL(loop);OSSafeReleaseNULL(events);
        gcc.release();se.release();reset.release();irq.release();sda.release();scl.release();
    }
public:
    void select(unsigned requested) {stage=requested;}
    IOWorkLoop *getWorkLoop() const override {return loop;}
    OSString *newTransportString() const override {return OSString::withCString("I2C");}
    OSString *newProductString() const override {return OSString::withCString("Odin3 FocalTech");}
    IOReturn newReportDescriptor(IOMemoryDescriptor **out) const override {
        if (!out) return kIOReturnBadArgument;
        *out=IOBufferMemoryDescriptor::withBytes(touchDescriptor,sizeof touchDescriptor,kIODirectionNone);
        return *out?kIOReturnSuccess:kIOReturnNoMemory;
    }
    bool start(IOService *provider) override {
        if (!stage || stage>4) return false;
        int rc=inspect();note("retained resources",rc);
        if (rc) {cleanup();return false;}
        if (stage<3) {note("STOP before chip ID",0,stage);cleanup();return false;}
        rc=identify();
        if (rc || stage==3) {note("ID/FW STOP no HID",rc,residentEvidence);cleanup();return false;}
        PE_parse_boot_argn("odin-touch-id",&expected,sizeof expected);
        PE_parse_boot_argn("sheng-fb-rotate",&rotation,sizeof rotation);
        if (!expected || identity!=expected || (rotation!=1 && rotation!=3)) {
            note("STOP: require measured ID",-19,identity);cleanup();return false;
        }
        loop=IOWorkLoop::workLoop();
        pagani_hid::edgeX=24*65536u/1920;pagani_hid::edgeY=24*65536u/1080;
        if (!loop || !pagani_hid::installPersonality("I2C","Odin3 FocalTech")) {cleanup();return false;}
        setProperty("Built-In",true);setProperty("DisplayIntegrated",true);setProperty("HIDDefaultBehavior",true);
        setProperty("DigitizerCollectionDispatch",true);
        started=IOHIDDevice::start(provider);
        if (!started) {cleanup();return false;}
        timer=IOTimerEventSource::timerEventSource(this,tick);
        added=timer && loop->addEventSource(timer)==kIOReturnSuccess;
        if (!added) {IOHIDDevice::stop(provider);started=false;cleanup();return false;}
        note("scan resident firmware",0,identity);__atomic_store_n(&running,true,__ATOMIC_RELEASE);timer->setTimeoutMS(1);return true;
    }
    void stop(IOService *provider) override {
        __atomic_store_n(&running,false,__ATOMIC_RELEASE);if (timer) timer->cancelTimeout();
        odin_ft_frame empty={};if (started) send(empty);
        if (started) IOHIDDevice::stop(provider);
        started=false;cleanup();
    }
    void free() override {cleanup();IOHIDDevice::free();}
};
OSDefineMetaClassAndStructors(OdinTouch,IOHIDDevice)
extern "C" IOService *odin_touch_allocate(unsigned stage) {
    auto service=new OdinTouch;if (service) service->select(stage);return service;
}
