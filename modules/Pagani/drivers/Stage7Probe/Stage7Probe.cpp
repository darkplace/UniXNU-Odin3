// SPDX-License-Identifier: BSD-2-Clause-Patent
// Adapted from UniXNU-sheng's LegacyDisplayProbe and KeyStoreProbe for SM8750.
#include <IOKit/IOPlatformExpert.h>
#include <IOKit/IODeviceMemory.h>
#include <IOKit/IODeviceTreeSupport.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOUserClient.h>
#include <libkern/c++/OSData.h>
#include <libkern/c++/OSSymbol.h>
#include <libkern/c++/OSKext.h>
#include <pexpert/pexpert.h>
#include <mach/kmod.h>
#include <libkern/libkern.h>
#include "../BootProgress/PeripheralStatus.h"
#include "GpuSurvey.h"
extern "C" int boot_progress_gpu_note(unsigned,const char *,uint64_t,uint64_t);
extern "C" kmod_info_t g_kernel_kmod_info;
extern "C" kern_return_t OSKextLoadKextWithIdentifier(const char *);
extern "C" int boot_progress_display_handoff(void);
extern "C" void boot_progress_display_failed(void);
extern "C" void boot_progress_display_status(const char *,uint64_t);
extern "C" void boot_progress_aks_failed(unsigned);
extern "C" void native_legacy_display_static_init(void) __asm__("__GLOBAL__sub_I_NativeLegacyDisplay.cpp");
extern "C" void native_keyevents_static_init(void) __asm__("__GLOBAL__sub_I_NativeKeyStoreEvents.cpp");
extern "C" IOService *native_legacy_display_allocate(void),*native_keyevents_allocate(void);
extern "C" void pagani_input_static_init(void) __asm__("__GLOBAL__sub_I_PaganiInput.cpp");
extern "C" void pagani_panel_static_init(void) __asm__("__GLOBAL__sub_I_PaganiPanel.cpp");
extern "C" IOService *pagani_input_allocate(bool),*pagani_panel_allocate(void);
extern "C" void odin_touch_static_init(void) __asm__("__GLOBAL__sub_I_OdinTouch.cpp");
extern "C" IOService *odin_touch_allocate(unsigned);
extern "C" void odin_fan_static_init(void) __asm__("__GLOBAL__sub_I_OdinFan.cpp");
extern "C" IOService *odin_fan_allocate(unsigned);
static IOService *pendingDisplay;
static bool displayInitialized,ephemeralState;
static uint64_t frames,clients,aksCalls,aksOpens;

static void progress(const char *detail,uint64_t value) {boot_progress_display_status(detail,value);}
static bool enabled(const char *name) {
    unsigned value=0;return PE_parse_boot_argn(name,&value,sizeof value) && value==1;
}
static int gpuRead32(void *,uint64_t physical,uint32_t *value) {
    IODeviceMemory *memory=IODeviceMemory::withRange(physical & ~0xfffULL,0x1000);
    IOMemoryMap *map=memory?memory->map(kIOMapInhibitCache):nullptr;
    if (!map || !map->getVirtualAddress()) {OSSafeReleaseNULL(map);OSSafeReleaseNULL(memory);return -1;}
    *value=*(volatile uint32_t *)(map->getVirtualAddress()+(physical & 0xfff));
    __asm__ volatile("dsb sy" ::: "memory");
    OSSafeReleaseNULL(map);OSSafeReleaseNULL(memory);
    return 0; // A bus exception is NOT caught here. The pre-access row identifies the attempted access.
}
static int gpuScm(void *,uint32_t fn,uint64_t arginfo,uint64_t arg0,uint64_t reply[4]) {
    register uint64_t x0 __asm__("x0")=fn,x1 __asm__("x1")=arginfo,x2 __asm__("x2")=arg0,x3 __asm__("x3")=0;
    register uint64_t x4 __asm__("x4")=0,x5 __asm__("x5")=0,x6 __asm__("x6")=0,x7 __asm__("x7")=0;
    __asm__ volatile("smc #0" : "+r"(x0),"+r"(x1),"+r"(x2),"+r"(x3),"+r"(x4),"+r"(x5),"+r"(x6),"+r"(x7)
                     : : "x8","x9","x10","x11","x12","x13","x14","x15","x16","x17","cc","memory");
    reply[0]=x0;reply[1]=x1;reply[2]=x2;reply[3]=x3;
    return 0; // Raw return, including interrupted/busy/unsupported; no retry or secure operation.
}
static int gpuBefore(void *,unsigned index,const char *name,uint64_t address) {
    char text[64];snprintf(text,sizeof text,"BEFORE %.28s",name);
    return boot_progress_gpu_note(index,text,address,0);
}
static void gpuAfter(void *,unsigned index,const gs_row *row) {
    char text[64];snprintf(text,sizeof text,"%s %.28s",row->result==GS_OK?"READ":(row->result==GS_SKIPPED_GATE?"SKIP":"ERROR"),row->name);
    boot_progress_gpu_note(index,text,row->value,row->result==GS_OK?row->status:row->result);
}
static void gpuSurvey(void) {
    if (!enabled("odin-gpu-survey")) return;
    static gs_result result;
    const gs_io io={nullptr,gpuRead32,gpuScm,gpuBefore,gpuAfter};
    gs_run(&io,&result);
    boot_progress_gpu_note(23,result.cancelled?"CANCELLED: no checkpoint":"DONE: gate A/B (not ownership)",
                          ((uint64_t)(result.gate_a==1)<<32)|(result.gate_b==1),result.accesses);
}
static bool registerClass(void (*initializer)(void)) {
    void *handle=OSMetaClass::preModLoad(g_kernel_kmod_info.name);
    if(!handle)return false;
    initializer();return OSMetaClass::postModLoad(handle)==kIOReturnSuccess;
}
static void startPeripheral(IOService *service,const char *label,unsigned device) {
    IOService *provider=IOService::getResourceService();
    bool attached=service && provider && service->init() && service->attach(provider);
    if (!attached) boot_progress_peripheral(device,"service attach",kIOReturnNoResources,0,0);
    bool started=attached && service->start(provider);
    progress(label,started);
    if (!started) {if (attached) service->detach(provider);OSSafeReleaseNULL(service);}
    // A started peripheral retains its service lifetime until reboot, like the display provider.
}
static void startPeripherals(bool hidReady) {
    unsigned odinTouch=0;PE_parse_boot_argn("odin-touch",&odinTouch,sizeof odinTouch);
    if (odinTouch && odinTouch<=4 && (odinTouch<4 || hidReady)) {
        IORegistryEntry *node=IORegistryEntry::fromPath("/arm-io/touch",gIODTPlane);
        OSData *compatible=node?OSDynamicCast(OSData,node->getProperty("compatible")):nullptr;
        const char expected[]="ayn,odin3-ft3519";
        bool matches=compatible && compatible->getLength()==sizeof expected &&
            !memcmp(compatible->getBytesNoCopy(),expected,sizeof expected);
        OSSafeReleaseNULL(node);
        if (matches && registerClass(odin_touch_static_init))
            startPeripheral(odin_touch_allocate(odinTouch),"Odin touch stage",PAGANI_TOUCH);
    }
    // odin-fan=0 (default) off, 1 inspect only, 2 power+init fan at 0, 3 fixed odin-fan-pct (default 50).
    unsigned odinFan=0;PE_parse_boot_argn("odin-fan",&odinFan,sizeof odinFan);
    if (odinFan && odinFan<=3) {
        IORegistryEntry *node=IORegistryEntry::fromPath("/arm-io/touch",gIODTPlane);
        OSData *compatible=node?OSDynamicCast(OSData,node->getProperty("compatible")):nullptr;
        const char expected[]="ayn,odin3-ft3519"; // Odin board marker only; the fan itself needs no ADT node
        bool board=compatible && compatible->getLength()==sizeof expected &&
            !memcmp(compatible->getBytesNoCopy(),expected,sizeof expected);
        OSSafeReleaseNULL(node);
        if (board && registerClass(odin_fan_static_init))
            startPeripheral(odin_fan_allocate(odinFan),"Odin fan stage",PAGANI_PANEL);
    }
    // Odin uses firmware scanout only. Do not adopt PKX110 GPIOs, rails,
    // touch or DSI controls even if an inherited argument enables them.
    if (enabled("odin-gop-only")) {
        progress("Odin GOP; PKX110 peripherals blocked",1);
        return;
    }
    boot_progress_peripheral_enable(enabled("pagani-peripheral-status"));
    boot_progress_platform_enable(enabled("pagani-platform-status"));
    for (unsigned i=0;i<PAGANI_SPI;++i) boot_progress_peripheral(i,"disabled",0,0,0);
    if (enabled("pagani-touch") || enabled("pagani-buttons")) {
        bool ready=hidReady && registerClass(pagani_input_static_init);
        const char *stage=hidReady?"HID class":"HID resource";
        if (enabled("pagani-buttons")) {
            boot_progress_peripheral(PAGANI_KEYS,stage,ready?0:kIOReturnNotReady,0,0);
            if (ready) startPeripheral(pagani_input_allocate(false),"physical buttons started",PAGANI_KEYS);
        }
        if (enabled("pagani-touch")) {
            boot_progress_peripheral(PAGANI_TOUCH,stage,ready?0:kIOReturnNotReady,0,0);
            if (ready) startPeripheral(pagani_input_allocate(true),"S3910 touch started",PAGANI_TOUCH);
        }
    }
    if (enabled("pagani-panel")) {
        kern_return_t loaded=OSKextLoadKextWithIdentifier("com.apple.driver.AppleARMPlatform");
        progress("AppleARMPlatform load",(uint32_t)loaded);
        bool ready=!loaded && registerClass(pagani_panel_static_init);
        boot_progress_peripheral(PAGANI_PANEL,loaded?"platform load":"panel class",loaded?loaded:(ready?0:kIOReturnNotReady),0,0);
        if (ready) startPeripheral(pagani_panel_allocate(),"DSI panel control started",PAGANI_PANEL);
    }
}
static bool console(PE_Video &video) {
    return !PE_current_console(&video) && video.v_baseAddr && video.v_width && video.v_height &&
        video.v_width<=16384 && video.v_height<=16384 && video.v_depth==32 && !video.v_rotate &&
        video.v_rowBytes>=video.v_width*4 && video.v_rowBytes<=UINT64_MAX/video.v_height;
}
extern "C" {
__attribute__((noreturn)) void native_legacy_backend_guard(unsigned slot,void *object,void *caller) {
    panic("SM8750 unsupported display slot %u object %p caller %p",slot,object,caller);
}
bool native_legacy_gop_mapping_observed(IOService *,IOMemoryMap *mapping,uint32_t width,uint32_t height) {
    progress("GOP mapping ready",((uint64_t)width<<32)|height);
    return mapping && mapping->getVirtualAddress();
}
void native_legacy_registration_observed(IOService *) {progress("display registered",1);}
void native_legacy_swap_stage(unsigned) {}
void native_legacy_vsync_observed(unsigned event,unsigned hz) {if(event==4)progress("software display refresh Hz",hz);}
void native_legacy_client_enter(task_t,uint32_t type) {progress("display client enter",type);}
void native_legacy_client_observed(IOReturn status,IOUserClient *) {progress("display client",status?(uint32_t)status:++clients);}
void native_legacy_pm_observed(unsigned stage,IOReturn status,unsigned long) {if(status)progress("display PM error",((uint64_t)stage<<32)|(uint32_t)status);}
void native_legacy_frame_observed(unsigned,uint64_t,uint64_t,uint64_t,uint64_t,uint64_t) {}
void native_legacy_frame_colors_observed(const uint32_t *,const uint64_t *,uint64_t) {}
void boot_progress_display_geometry(unsigned,unsigned,uint64_t,unsigned,unsigned,unsigned,unsigned,uint64_t);
void native_legacy_geometry_observed(unsigned pw,unsigned ph,uint64_t pitch,
    unsigned sw,unsigned sh,unsigned iw,unsigned ih,uint64_t sourcePitch) {
    boot_progress_display_geometry(pw,ph,pitch,sw,sh,iw,ih,sourcePitch);
    unsigned calibrate=0;PE_parse_boot_argn("odin-fb-calibrate",&calibrate,sizeof calibrate);
    if(calibrate==1) {
        boot_progress_peripheral_enable(1);
        boot_progress_peripheral(PAGANI_PANEL,"raw GOP square",0,(pw<<16)|ph,pitch);
    }
}
void native_legacy_swap_validation_observed(bool accepted,unsigned reason,const void *,const void *) {if(!accepted)progress("display swap rejected",reason);}
// A refused swap, as the validator read it (layout in NativeLegacyDisplay.cpp). One line on the page, always the latest: S = surface size,
// s = layer-0 source rectangle size, d = destination rectangle size, R = layer-0 rotation word, r = reason. The value packs the request's
// size words (816/820 as 16 bits each), the nonzero x/y offsets of the two rectangles and which of the three options are set.
void native_legacy_swap_digest(unsigned reason,const uint32_t v[18]) {
    char text[64];
    snprintf(text,sizeof text,"r%u S%ux%u s%ux%u d%ux%u R%u",reason,v[16],v[17],v[2],v[3],v[6],v[7],v[8]);
    const uint64_t offsets=(v[0]|v[1]|v[4]|v[5])&0xffff,options=(v[9]?1:0)|(v[10]?2:0)|(v[11]?4:0);
    progress(text,((uint64_t)(v[12]&0xffff)<<48)|((uint64_t)(v[13]&0xffff)<<32)|(offsets<<16)|options);
}
void native_legacy_surface_rejected(uint64_t,uint64_t,uint32_t format,uint32_t,uint64_t,uint64_t,uint64_t) {progress("surface format rejected",format);}
bool native_legacy_prepare_scanout(void) {return boot_progress_display_handoff()!=0;}
void native_legacy_transfer_observed(IOReturn status,const void *,uint64_t) {
    if(status) {
        // A failed first copy must not permanently hide the diagnostic console.
        if(!frames)boot_progress_display_failed();
        progress("display copy failed",(uint32_t)status);
    } else {
        ++frames;progress("system frame copied",frames);
        boot_progress_peripheral_frame();
    }
}
void native_legacy_copy_timing_observed(uint64_t,uint64_t) {}
void native_legacy_retirement_observed(IOService *) {}
// shortcut: CPU scanout only until the SM8750 GPU and its synchronization are implemented.
bool native_legacy_gpu_offer(void *,IOMemoryDescriptor *,uint64_t,uint32_t,uint32_t) {return false;}
void native_legacy_gpu_fallback_observed(int) {}
void native_keyevents_open_refused(void) {progress("key state open refused",0);}
bool native_aks_state_enabled(void) {return ephemeralState;}
uint32_t native_aks_lock_state(void) {return 0;}
void native_aks_call(uint32_t selector,int32_t result) {++aksCalls;if(result){boot_progress_aks_failed(selector);progress("key operation unavailable",selector);}}
void native_aks_opened(void) {++aksOpens;}
}
static void startKeyState(void) {
    if(!enabled("offline-key-events") || !enabled("sheng-aks"))return;
    IORegistryEntry *chosen=IORegistryEntry::fromPath("/chosen",gIODTPlane);
    OSData *data=chosen?OSDynamicCast(OSData,chosen->getProperty("ephemeral-storage")):nullptr;
    uint32_t ephemeral=0;
    if(data && data->getLength()==sizeof ephemeral)memcpy(&ephemeral,data->getBytesNoCopy(),sizeof ephemeral);
    OSSafeReleaseNULL(chosen);
    if(ephemeral!=1)return;
    const OSSymbol *name=OSSymbol::withCString("AppleKeyStore");
    bool absent=name && !OSMetaClass::getMetaClassWithName(name);
    OSSafeReleaseNULL(name);
    IOService *provider=IOService::getResourceService();
    if(!absent || !provider || !registerClass(native_keyevents_static_init))return;
    IOService *service=native_keyevents_allocate();
    bool attached=service && service->init() && service->attach(provider);
    // Publish state queries only after the volatile-storage guard passed.
    ephemeralState=true;
    if(attached && service->start(provider)){progress("volatile key state registered",1);return;}
    ephemeralState=false;
    if(attached)service->detach(provider);
    OSSafeReleaseNULL(service);
    progress("volatile key state failed",0);
}
extern "C" void native_legacy_display_probe(void) {
    if(displayInitialized)return;
    displayInitialized=true;
    progress("display class registration enter",0);
    if(!registerClass(native_legacy_display_static_init)){progress("display class failed",0);return;}
    PE_Video video={};
    if(!console(video)){progress("GOP console unavailable",0);return;}
    progress("display allocation enter",0);
    IOService *display=native_legacy_display_allocate();
    progress("display init enter",display!=nullptr);
    if(display && display->init())pendingDisplay=display;
    else OSSafeReleaseNULL(display);
    progress("display class initialized",pendingDisplay!=nullptr);
}
extern "C" void native_legacy_display_late_probe(void) {
    gpuSurvey(); // One-shot, before display handoff; independent of the PKX110 peripheral guard.
    progress("key state start enter",0);
    startKeyState();
    // Load original event dispatch before attaching physical HID devices.
    progress("IOHIDFamily load enter",0);
    progress("IOHIDFamily load",(uint32_t)OSKextLoadKextWithIdentifier("com.apple.iokit.IOHIDFamily"));
    progress("IOHIDEventDriver load enter",0);
    progress("IOHIDEventDriver matching",(uint32_t)OSKext::loadKextWithIdentifier(
        "com.apple.iokit.IOHIDEventDriver",true,false,kOSKextExcludeNone,kOSKextExcludeNone,nullptr));
    OSDictionary *hidMatching=IOService::serviceMatching("IOHIDResource");
    IOService *hidResource=hidMatching?IOService::waitForMatchingService(hidMatching,5000000000ULL):nullptr;
    progress("IOHIDResource ready",hidResource!=nullptr);
    bool hidReady=hidResource!=nullptr;
    OSSafeReleaseNULL(hidResource);OSSafeReleaseNULL(hidMatching);
    IOService *display=pendingDisplay;
    pendingDisplay=nullptr;
    if(!display){progress("no initialized display",0);return;}
    progress("IOSurface load enter",0);
    kern_return_t loaded=OSKextLoadKextWithIdentifier("com.apple.iokit.IOSurface");
    // Match the service CLASS, not IOName. Q2 with this exact IPSW showed
    // IOSurfaceRoot registered while the IOCoreSurfaceRoot name wait failed.
    OSDictionary *matching=IOService::serviceMatching("IOSurfaceRoot");
    IOService *surface=matching?IOService::waitForMatchingService(matching,5000000000ULL):nullptr;
    if (!surface) {
        OSSafeReleaseNULL(matching);
        matching=IOService::serviceMatching("IOCoreSurfaceRoot");
        surface=matching?IOService::waitForMatchingService(matching,5000000000ULL):nullptr;
    }
    bool surfaceReady=!loaded && surface;
    OSSafeReleaseNULL(surface);OSSafeReleaseNULL(matching);
    progress("IOSurface ready",surfaceReady);
    PE_Video video={};
    if(!surfaceReady || !console(video)){display->release();return;}
    IOPlatformDevice *nub=new IOPlatformDevice;
    IOService *parent=IOService::getResourceService();
    IORegistryEntry *dtParent=IORegistryEntry::fromPath("/arm-io",gIODTPlane);
    IODeviceMemory *memory=IODeviceMemory::withRange(video.v_baseAddr,video.v_rowBytes*video.v_height);
    OSArray *ranges=OSArray::withCapacity(1);
    const char type[]="display-subsystem";
    OSData *typeData=OSData::withBytes(type,sizeof type);
    bool nubAttached=false,nubStarted=false,dtAttached=false,attached=false,started=false;
    if(nub && parent && dtParent && memory && ranges && typeData && nub->init() && ranges->setObject(memory)) {
        nub->setDeviceMemory(ranges);nub->setName("NativeGOPFramebuffer");
        nubAttached=nub->attach(parent);
        nubStarted=nubAttached && nub->start(parent);
        if(nubStarted && nub->setProperty("device_type",typeData)) {
            nub->setName("gop-display",gIODTPlane);
            dtAttached=nub->attachToParent(dtParent,gIODTPlane);
            SInt32 score=0;
            progress("GOP display probe enter",dtAttached);
            attached=dtAttached && display->probe(nub,&score)==display && display->attach(nub);
            progress("GOP display start enter",attached);
            started=attached && display->start(nub);
        }
    }
    progress("GOP display started",started);
    if (started) startPeripherals(hidReady);
    OSSafeReleaseNULL(ranges);OSSafeReleaseNULL(memory);OSSafeReleaseNULL(typeData);
    if(!started) {
        if(attached)display->detach(nub);
        if(dtAttached)nub->detachFromParent(dtParent,gIODTPlane);
        if(nubStarted)nub->stop(parent);
        if(nubAttached)nub->detach(parent);
        OSSafeReleaseNULL(nub);OSSafeReleaseNULL(display);
    }
    // On success the service and its real framebuffer provider live until reboot.
    OSSafeReleaseNULL(dtParent);
}
