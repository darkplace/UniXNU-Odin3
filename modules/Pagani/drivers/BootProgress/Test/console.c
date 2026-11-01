// SPDX-License-Identifier: BSD-2-Clause-Patent
// Compile with -fsanitize=address,undefined; uses the actual renderer and map builder.
#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#define BOOT_PROGRESS_TEST 1
#ifndef T0SZ_BOOT
#define T0SZ_BOOT 26
#endif
typedef struct boot_args {
    uint16_t Revision, Version;
    uint64_t virtBase, physBase, memSize, topOfKernelData;
    struct { uint64_t v_baseAddr, v_display, v_rowBytes, v_width, v_height, v_depth; } Video;
} boot_args;
typedef void *thread_call_t;
enum { NSEC_PER_SEC = 1000000000, THREAD_CALL_PRIORITY_KERNEL = 1, THREAD_CALL_OPTIONS_ONCE = 1 };
struct { uint32_t timebase_frequency_hz; } gPEClockFrequencyInfo;
volatile uint32_t armdt_platform_configured, armcpu_registered, armcpu_gic_ready, armcpu_gic_boot[8];
volatile uint64_t armcpu_timer_interrupts;
volatile uint32_t armcpu_smp_requested, armcpu_smp_running, armcpu_smp_conduit, armcpu_smp_fail[8];
void early_boot_progress_vectors_done(void) {}
static uintptr_t ml_io_map_wcomb(uintptr_t base, uint64_t size) { (void)size; return base; }
static void clock_interval_to_deadline(unsigned n, unsigned scale, uint64_t *deadline) { *deadline = (uint64_t)n * scale; }
static unsigned scheduled;
static void thread_call_enter_delayed(thread_call_t call, uint64_t deadline) { (void)call; assert(deadline == NSEC_PER_SEC); scheduled++; }
static void thread_call_enter(thread_call_t call) { assert(call); }
static thread_call_t thread_call_allocate_with_options(void (*fn)(void *, void *), void *arg, int priority, int options)
{ (void)fn; (void)arg; (void)priority; (void)options; return (void *)1; }
#define BOOT_LOGO_WIDTH 4
#define BOOT_LOGO_HEIGHT 4
#define BOOT_LOGO_X 2
#define BOOT_LOGO_Y -1
static const unsigned char boot_logo[] = {0,64,128,255,255,128,64,0,0,64,128,255,255,128,64,0};
static uint64_t test_cpu;
unsigned processor_count, processor_avail_count;
unsigned long gPhysSize;
volatile uint32_t armcpu_core_timer[8], armcpu_core_sgi[8];
static int release_on_contention;
static unsigned exchange_for_test(unsigned *lock, unsigned value, int order)
{
    unsigned previous = __atomic_exchange_n(lock, value, order);
    // Let the other CPU finish immediately after this failed acquisition.
    if (previous && release_on_contention) {
        release_on_contention = 0;
        __atomic_store_n(lock, 0, __ATOMIC_RELEASE);
    }
    return previous;
}
#define __atomic_exchange_n exchange_for_test
void odin_boot_snapshot(unsigned *count, unsigned *mask) { *count = 3; *mask = 1; }
void boot_progress_process_stage(int, const char *, const char *);
// Host stand-in for the saved state: the "state" pointer value is the PC.
typedef void arm_saved_state_t;
#define get_saved_state_pc(s) ((uint64_t)(uintptr_t)(s))
#include "../BootProgress.c"
#undef __atomic_exchange_n
volatile int native_owner_cache_stage;
volatile uint64_t native_ufs_block_reads, native_ufs_block_failures;
char native_sd_diag[6][88];
volatile uint32_t native_sd_diag_count, native_sd_probe_calls, native_sd_integrity_failures, native_sd_windows_verified;
volatile uint64_t native_sd_verified_blocks;
volatile uint64_t native_sd_read_bytes, native_sd_read_us, native_sd_write_bytes, native_sd_write_us;
// Counters of the 4 KiB code-signature pass (ubc_subr.c on the device; weak in BootProgress.c).
uint64_t cs_native4k_read_ticks, cs_native4k_hash_ticks, cs_native4k_bytes;
unsigned long cs_native4k_ready_blobs;
volatile uint64_t native_data_persist_loaded;      // Data pages served by the store window (NativeRamBlock.cpp)
unsigned vm_page_free_count;
uint32_t memorystatus_available_pages;
// Card-log hooks the OdinSd driver provides on the device (weak in BootProgress.c).
static unsigned ul_lines, ul_flushes, uf_lines;
static char ul_text[512], uf_text[16][160];
// UL lines are the snapshot; UF lines are the aborts that came with it (one each, kept in a small ring for the assertions).
void native_sd_log_note(const char *text)
{
    if (!strncmp(text, "UF ", 3)) { snprintf(uf_text[uf_lines % 16], sizeof uf_text[0], "%s", text); ++uf_lines; return; }
    ++ul_lines; snprintf(ul_text, sizeof ul_text, "%s", text);
}
_Bool native_sd_log_flush(void) { ++ul_flushes; return 1; }
// Off until the card-log section: the recount stub then reports 10 % busy for domain 0 and 40 % for domain 1 on every call.
static int test_domain_on;
static uint64_t test_domain_now[2][2];
int odin_cpu_domain_usage(uint64_t out[2][2],unsigned counts[2]) {
    static const unsigned busy_step[2] = {10, 40};
    memset(out,0,4*sizeof(uint64_t));counts[0]=counts[1]=0;
    if(!test_domain_on)return 0;
    for(unsigned d=0;d<2;++d){
        test_domain_now[d][0]+=busy_step[d];test_domain_now[d][1]+=100-busy_step[d];
        out[d][0]=test_domain_now[d][0];out[d][1]=test_domain_now[d][1];counts[d]=d?8:0;
    }
    return 1;
}

static void panic_for_test(const char *format, ...)
{
    va_list args;
    va_start(args, format);
    boot_progress_panic(format, &args, 0xfffffff007123456, 9, 0x140);
    va_end(args);
}

int main(void)
{
    enum { W = 126, H = 840, PITCH = 132, GUARD = 32 };
    uint32_t *allocation = malloc((PITCH * H + 2 * GUARD) * sizeof(uint32_t));
    assert(allocation);
    for (unsigned i = 0; i < PITCH * H + 2 * GUARD; i++) allocation[i] = 0x12345678;
    boot_args args = { .virtBase = 0xfffffff007004000ULL, .physBase = 0xc0004000,
                      .memSize = 0x10000000, .Video = { (uintptr_t)(allocation + GUARD), 1, PITCH * 4, W, H, 32 } };
    early_boot_progress_init(&args);
    assert(boot_progress_gpu_note(0,"BEFORE gcc gpu_cfg_ahb",0x171004,0));
    assert(strstr(gpu_survey_rows[0],"171004") && gpu_survey_count == 1);
    busy = 1;
    assert(!boot_progress_gpu_note(1,"must refuse while busy",0,0));
    busy = 0;
    assert(!boot_progress_gpu_note(24,"bounds",0,0));
    gpu_survey_count = 0;
    assert(boot_progress_active() && logo_visible);
    early_boot_progress_vm_ready();
    unsigned previous = stage_count;
    uint64_t held = irq_save();
    assert(held != UINT64_MAX);
    test_cpu = 0x10000;
    boot_progress_stage("contended secondary");
    assert(stage_count == previous);
    release_on_contention = 1;
    assert(boot_progress_display_handoff());
    assert(!release_on_contention && !console_lock && display_owned);
    boot_progress_display_failed();
    test_cpu = 0;
    irq_restore(held);
    boot_progress_stage("uncontended boot CPU");
    assert(stage_count == previous + 1);

    unsigned logo_at = GUARD + ((H - BOOT_LOGO_HEIGHT) / 2 + BOOT_LOGO_Y) * PITCH +
                       (W - BOOT_LOGO_WIDTH) / 2 + BOOT_LOGO_X;
    assert(allocation[logo_at] == 0xff000000 && allocation[logo_at + 1] == 0xff404040 &&
           allocation[logo_at + 2] == 0xff808080 && allocation[logo_at + 3] == 0xffffffff);
    for (unsigned i = 0; i < 5000; i++) boot_progress_putc((i % 41) ? 'X' : '\n');
    assert(allocation[logo_at + 3] == 0xffffffff);
    boot_progress_stage("wrap, CR\rLF\n and invalid bytes \1\377");
    for (unsigned i = 0; i < GUARD; i++) {
        assert(allocation[i] == 0x12345678);
        assert(allocation[GUARD + PITCH * H + i] == 0x12345678);
    }
    for (unsigned y = 0; y < H; y++)
        for (unsigned x = W; x < PITCH; x++) assert(allocation[GUARD + y * PITCH + x] == 0x12345678);
    assert(!geometry(UINTPTR_MAX - 3, W, H, PITCH * 4, 32));
    assert(!geometry(4, W, H, W * 4 - 4, 32));
    assert(!geometry(4, W, H, PITCH * 4, 16));
    assert(!geometry(4, W, 100000, PITCH * 4, 32));
    assert(!geometry(5, W, H, PITCH * 4, 32));
    args.Video.v_width += 1ULL << 32;
    early_boot_progress_init(&args);
    assert(!boot_progress_active() && framebuffer_pa == 0);
    args.Video.v_width = W;
    early_boot_progress_init(&args);

    uint64_t *tables = aligned_alloc(4096, 4 * 4096);
    assert(tables);
    memset(tables, 0, 4 * 4096);
    framebuffer_pa = 0x3ffff000;
    assert(early_boot_progress_map(tables, (uintptr_t)(tables + 512), (uintptr_t)(tables + 1536)));
    assert(tables[0] == ((uintptr_t)(tables + 512) | 3));
    assert(tables[1] == ((uintptr_t)(tables + 1024) | 3));
    assert(tables[512 + 511] == (0x3fe00000ULL | (1ULL << 54) | (1ULL << 53) | 0x605));
    assert(tables[1024] == (0x40000000ULL | (1ULL << 54) | (1ULL << 53) | 0x605));
    for (unsigned i = 1536; i < 2048; i++) assert(tables[i] == 0);
    // A repeated map uses the existing tables without consuming another page.
    assert(early_boot_progress_map(tables, (uintptr_t)(tables + 1536), (uintptr_t)(tables + 1536)));
    tables[1024] ^= 4;
    assert(!early_boot_progress_map(tables, (uintptr_t)(tables + 1536), (uintptr_t)(tables + 1536)));
    memset(tables, 0, 4 * 4096);
    assert(!early_boot_progress_map(tables, (uintptr_t)(tables + 512), (uintptr_t)(tables + 512)));
    assert(tables[0] == 0);
    framebuffer_pa = (1ULL << (64 - T0SZ_BOOT)) - 4096;
    assert(!early_boot_progress_map(tables, (uintptr_t)(tables + 512), (uintptr_t)(tables + 1536)));
    framebuffer_pa = 1ULL << (64 - T0SZ_BOOT);
    assert(!early_boot_progress_map(tables, (uintptr_t)(tables + 512), (uintptr_t)(tables + 1536)));
    assert(tables[0] == 0);
    armcpu_timer_interrupts = 7;
    boot_progress_start(); poll(NULL, NULL);
    assert(poll_count == 1 && armcpu_timer_interrupts == 7);
    boot_progress_root("root device wait begin", "IOMedia Content=Apple_HFS", 60);
    assert(root_sequence == 1 && root_value == 60);
    assert(!strcmp(root_stage, "root device wait begin"));
    assert(!strcmp(root_detail, "IOMedia Content=Apple_HFS"));
    // The stack-margin suffix " k=xxxx" goes after the detail in the history; it must not overwrite it.
    assert(strstr(stages[(stage_count - 1) % HISTORY], "root device wait begin: IOMedia Content=Apple_HFS k="));
    native_owner_cache_stage = 3;
    vm_page_free_count = 1234; memorystatus_available_pages = 2345;
    native_ufs_block_reads = 5678; native_ufs_block_failures = 9;
    native_sd_read_bytes = 300ULL << 20; native_sd_read_us = 7654321;
    native_sd_write_bytes = 5ULL << 20; native_sd_write_us = 2500000;
    uint32_t root_line[LINE * PITCH];
    unsigned saved_row = row, saved_column = column;
    // The root line the last boot_progress_root drew (one row above the cursor) must survive a poll.
    unsigned root_at = (saved_row - 1) * LINE * stride * 4;
    memcpy(root_line, (void *)(framebuffer + root_at), sizeof(root_line));
    poll(NULL, NULL);
    assert(!strcmp(storage_status, "free=1234 avail=2345 SD=5678 err=9 MiB=300 ms=7654 W=5120KiB/2500ms"));
    assert(!memcmp(root_line, (void *)(framebuffer + root_at), sizeof(root_line)));
    assert(row == saved_row && column == saved_column && native_ufs_block_reads == 5678);
    boot_progress_kernel_exception(0x96000010, 0xfffffff007345678, 0x12345000);
    assert(fault_valid && fault_esr == 0x96000010 && fault_far == 0x12345000);
    console_smp = 1; console_lock = 1; // A stopped console owner cannot block panic.
    panic_for_test("Failed to mount %s: %u %llx %%", "root device", 60U, 0x123456789abcdef0ULL);
    assert(panic_state == 1 && busy);
    assert(!boot_progress_display_handoff());
    assert(!strcmp(boot_progress_panic_text, "Failed to mount root device: 60 123456789abcdef0 %"));
    test_cpu = 0x10000;
    panic_for_test("second CPU must preserve first panic");
    assert(!strcmp(boot_progress_panic_text, "Failed to mount root device: 60 123456789abcdef0 %"));
    test_cpu = 0;
    assert(allocation[GUARD] == 0xffff4040);
    assert(allocation[GUARD + W / 3] == 0xff40ff40);
    assert(allocation[GUARD + 2 * W / 3] == 0xff4080ff);
    unsigned before_scheduled = scheduled, before_stages = stage_count;
    boot_progress_putc('X'); boot_progress_stage("must not overwrite panic");
    boot_progress_root("must not replace root state", NULL, 1); poll(NULL, NULL);
    assert(scheduled == before_scheduled && stage_count == before_stages && root_sequence == 1);
    assert(poll_count == 2 && armcpu_timer_interrupts == 7);
    uintptr_t before_framebuffer = framebuffer;
    boot_progress_bind(4, W, H, PITCH * 4, 32);
    assert(framebuffer == before_framebuffer);
    panic_for_test("nested failure %d", 123);
    assert(!strcmp(boot_progress_panic_text, "Failed to mount root device: 60 123456789abcdef0 %"));
    boot_progress_kernel_exception(0x96000004, 0xabcd, 0xdead);
    assert(fault_esr == 0x96000010); // The first fault remains available.
    early_boot_progress_init(&args);
    char long_message[3000];
    memset(long_message, 'P', sizeof(long_message) - 1); long_message[sizeof(long_message) - 1] = 0;
    for (unsigned i = 0; i < HISTORY + 3; i++) boot_progress_stage("history wraps within its fixed storage");
    panic_for_test("%s", long_message);
    assert(strlen(boot_progress_panic_text) == sizeof(boot_progress_panic_text) - 1 && panic_clipped);
    assert(allocation[GUARD] == 0xffff4040); // Long text never wraps over the panic banner.
    for (unsigned i = 0; i < GUARD; i++) {
        assert(allocation[i] == 0x12345678);
        assert(allocation[GUARD + PITCH * H + i] == 0x12345678);
    }
    for (unsigned y = 0; y < H; y++)
        for (unsigned x = W; x < PITCH; x++) assert(allocation[GUARD + y * PITCH + x] == 0x12345678);
    early_boot_progress_init(&args);
    boot_progress_process_stage(42, "launchd", "load_machfile");
    assert(strstr(stages[(stage_count - 1) % HISTORY], "pid=42 launchd load_machfile"));
    boot_progress_exit(43, "backboardd", 1, 2, 3);
    assert(strstr(stages[(stage_count - 1) % HISTORY], "exit pid=43 backboardd"));
    odin_boot_shadow(1, "init");
    assert(odin_shadow_count == 1 && strstr(exit_stage, "exit pid=43 backboardd"));
    assert(strstr(stages[(stage_count - 1) % HISTORY], "shadow retired"));
    odin_boot_svc(2, 12, 5, 0x1000);
    assert(odin_svc_other == 1 && odin_svc_count == 0);
    odin_boot_svc(1, 13, -47, 0x1234000);
    assert(odin_svc_count == 1 && odin_threads[0].pending && odin_threads[0].call == -47);
    odin_boot_svc_return(12, 0xbad, 0);
    assert(odin_threads[0].pending);
    odin_boot_svc_return(13, 0x10004003, 1U << 29);
    assert(!odin_threads[0].pending && odin_threads[0].returned == 1);
    assert(odin_threads[0].result == 0x10004003 && odin_threads[0].cpsr == 1U << 29);
    for (unsigned i = 1; i < ODIN_THREADS + 2; ++i) odin_boot_svc(1, 13 + i, 5, 0x2000 + i);
    assert(odin_threads_overflow == 2);
    odin_boot_main(1);
    assert(odin_main_ack == 1);
    odin_boot_fault(1, "launchd-long-process-name", 0x92000004, 0x3333, 0x3300, 0x4444, 0x100000000ULL, 0x4000);
    assert(odin_fault_count == 1 && odin_fault_pc == 0x3333 && odin_fault_far == 0x4444);
    assert(!strcmp(odin_faults[0].name, "launchd-long-pro") && odin_faults[0].pid == 1);
    assert(odin_faults[0].lr == 0x3300 && odin_faults[0].mapstart == 0x100000000ULL && odin_faults[0].mapoff == 0x4000);
    for (unsigned i = 0; i < ODIN_FAULTS + 2; ++i) odin_boot_fault(300 + i, NULL, 0x92000045, 0x100931ad0, 0, 0, 0, 0);
    assert(odin_fault_count == 1 + ODIN_FAULTS + 2 && odin_faults[(odin_fault_count - 1) % ODIN_FAULTS].pid == 300 + ODIN_FAULTS + 1);
    odin_fault_count = 1; odin_fault_pid = 1; odin_fault_esr = 0x92000004; odin_fault_pc = 0x3333; odin_fault_far = 0x4444;
    boot_progress_user_breakpoint((void *)0x180abc000, 0xf2000001);
    assert(odin_trap_count == 1 && odin_traps[0].pc == 0x180abc000 && odin_traps[0].esr == 0xf2000001);
    boot_progress_irq(0x5555, 0, 0x7777);
    boot_progress_irq(0x6666, 5, 0x8888);
    assert(odin_irq_user == 0x5555 && odin_irq_kernel == 0x6666 && odin_irq_sp == 0x7777);
    for (unsigned i = 0; i < 2000; ++i) odin_boot_log(i % 91 ? 'L' : '\n');
    assert(odin_log_col < ODIN_LOG_BYTES && odin_log_row < ODIN_LOG_LINES);
    odin_boot_userlog(2, "boot-task mount-phase-1\n", 23);
    assert(strstr(odin_userlog[0], "2>boot-task mount-phase-1"));
    for (unsigned i = 0; i < 100; ++i) odin_boot_userlog(1, "abcdef", 6);
    assert(odin_userlog_col < ODIN_LOG_BYTES && odin_userlog_row < 8);
    boot_progress_display_status("GOP display started", 1);
    assert(strstr(stages[(stage_count - 1) % HISTORY], "stage7: GOP display started k="));
    for (unsigned i = 0; i < HISTORY + 1; i++) boot_progress_process_stage(44, "launchd", "exec");
    assert(strstr(exit_stage, "exit pid=43 backboardd"));
    assert(!strcmp(display_stage, "GOP display started") && display_value == 1);
    for (unsigned i = 0; i < 29; i++) poll(NULL, NULL);
    assert(logo_visible && allocation[logo_at + 3] == 0xffffffff);
    poll(NULL, NULL);
    assert(!logo_visible && !display_owned && !panic_state && poll_count == 30);
    assert(!strcmp(display_status, "DISPLAY GOP display started 1"));
    early_boot_progress_init(&args);
    assert(logo_visible && poll_count == 0);
    assert(boot_progress_display_handoff());
    boot_progress_display_failed();
    assert(!display_owned);
    for (unsigned i = 0; i < 30; i++) poll(NULL, NULL);
    assert(!logo_visible);
    early_boot_progress_init(&args);
    busy = 1;
    assert(!boot_progress_display_handoff());
    busy = 0;
    assert(boot_progress_display_handoff());
    uint32_t *snapshot = malloc(PITCH * H * sizeof(uint32_t));
    assert(snapshot);
    memcpy(snapshot, allocation + GUARD, PITCH * H * sizeof(uint32_t));
    boot_progress_stage("must preserve system frame");
    boot_progress_root("frame ownership", "preserve pixels", 1);
    for (unsigned i = 0; i < 31; i++) poll(NULL, NULL);
    assert(!memcmp(snapshot, allocation + GUARD, PITCH * H * sizeof(uint32_t)));
    diag_resume_seconds = 10;
    poll(NULL, NULL);
    assert(!display_owned && !logo_visible);
    diag_resume_seconds = 0;
    assert(boot_progress_display_handoff());
    memcpy(snapshot, allocation + GUARD, PITCH * H * sizeof(uint32_t));
    boot_progress_peripheral(PAGANI_TOUCH, "TouchComm start", -71, 0x1000010, 0);
    boot_progress_peripheral(PAGANI_KEYS, "HID report", 0, 0xc001, 3);
    boot_progress_fan(0, "FAN: test line");
    boot_progress_fan(3, "ignored");
    assert(!strcmp(fan_status[0], "FAN: test line") && !fan_status[1][0]);
    boot_progress_peripheral(PAGANI_PERIPHERALS, "out of bounds", 0, 0, 0);
    boot_progress_peripheral(PAGANI_SPI, "reserved for SPI snapshot", 0, 0, 0);
    assert(!peripheral_status[PAGANI_SPI][0]);
    boot_progress_touch_spi(0x101,1,1,0x40000000,1,0x80000001);
    assert(strstr(peripheral_status[PAGANI_TOUCH], "r=ffffffb9 v=1000010 n=0"));
    assert(strstr(peripheral_status[PAGANI_KEYS], "n=3"));
    assert(!strcmp(peripheral_status[PAGANI_SPI], "SPI fw=101 if=1 st=1 irq=40000000"));
    assert(!strcmp(peripheral_status[PAGANI_SPI_FIFO], "SPI FIFO tx=1 rx=80000001"));
    boot_progress_touch_smmu(7,81,0x800000a3,0);
    assert(!strcmp(peripheral_status[PAGANI_SPI], "SMMU slot=7 bank=51"));
    assert(!strcmp(peripheral_status[PAGANI_SPI_FIFO], "SMMU smr=800000a3 s2cr=0"));
    boot_progress_touch_spi(0x102,1,0x41,0,0,0);
    boot_progress_touch_dma(4,0,2,1);
    assert(!strcmp(peripheral_status[PAGANI_SPI_FIFO], "SPI DMA tx=4 rx=0 ti=2 ri=1"));
    boot_progress_touch_power(0x40800,0x30000,1,0,0x10100,0x100,0);
    assert(!strcmp(peripheral_status[PAGANI_SPI], "PWR a=40800 hw=30000 t=1"));
    assert(!strcmp(peripheral_status[PAGANI_SPI_FIFO], "PWR st=0 c=10100/100/0"));
    boot_progress_touch_read(4,0,13,1,0x40,4,4,0xc800);
    assert(!strcmp(peripheral_status[PAGANI_SPI], "SPI RX n=4 h=00000000 a=1/0 rst=3"));
    assert(!strcmp(peripheral_status[PAGANI_SPI_FIFO], "SPI dma=1 ti=4 ri=4 st=40 dly=c800"));
    boot_progress_touch_hid(31,0xd0004,430,420,3,0x40007fff,0x800000010000ULL,0x802);
    assert(!strcmp(peripheral_status[PAGANI_SPI], "HID b=1f use=d/4 ev=430/420 e=3"));
    assert(!strcmp(peripheral_status[PAGANI_SPI_FIFO], "XY in=4000/7fff out=8000/10000 mask=802"));
    saved_row=row;saved_column=column;
    boot_progress_peripheral_frame();
    assert(!memcmp(snapshot,allocation+GUARD,PITCH*H*sizeof(uint32_t)));
    boot_progress_peripheral_enable(1);
    boot_progress_peripheral_frame();
    assert(display_owned && row==saved_row && column==saved_column);
    assert(!memcmp(snapshot,allocation+GUARD,4*LINE*PITCH*sizeof(uint32_t)));
    assert(memcmp(snapshot+4*LINE*PITCH,allocation+GUARD+4*LINE*PITCH,5*LINE*PITCH*sizeof(uint32_t)));
    assert(!memcmp(snapshot + 9 * LINE * PITCH, allocation + GUARD + 9 * LINE * PITCH,
                   (H - 9 * LINE) * PITCH * sizeof(uint32_t)));
    for (unsigned y=4*LINE;y<9*LINE;++y)
        for (unsigned x=W;x<PITCH;++x) assert(allocation[GUARD+y*PITCH+x]==0x12345678);
    busy=1;memcpy(snapshot,allocation+GUARD,PITCH*H*sizeof(uint32_t));
    boot_progress_peripheral_frame();
    assert(!memcmp(snapshot,allocation+GUARD,PITCH*H*sizeof(uint32_t)));busy=0;
    boot_progress_peripheral(PAGANI_KEYS,"PMIC sample",0,0x8001,5);
    poll(NULL,NULL);
    assert(!memcmp(snapshot,allocation+GUARD,4*LINE*PITCH*sizeof(uint32_t)));
    assert(memcmp(snapshot+4*LINE*PITCH,allocation+GUARD+4*LINE*PITCH,5*LINE*PITCH*sizeof(uint32_t)));
    assert(!memcmp(snapshot+9*LINE*PITCH,allocation+GUARD+9*LINE*PITCH,(H-9*LINE)*PITCH*sizeof(uint32_t)));
    boot_progress_peripheral_enable(0);
    boot_progress_platform_enable(1);
    processor_count = processor_avail_count = 8; gPhysSize = 4UL << 30;
    for (unsigned cpu=0; cpu<8; cpu++) armcpu_core_timer[cpu] = armcpu_core_sgi[cpu] = 1;
    memcpy(snapshot,allocation+GUARD,PITCH*H*sizeof(uint32_t));
    boot_progress_peripheral_frame();
    assert(memcmp(snapshot+4*LINE*PITCH,allocation+GUARD+4*LINE*PITCH,2*LINE*PITCH*sizeof(uint32_t)));
    assert(!memcmp(snapshot+6*LINE*PITCH,allocation+GUARD+6*LINE*PITCH,(H-6*LINE)*PITCH*sizeof(uint32_t)));
    panic_for_test("panic after system display handoff");
    memcpy(snapshot,allocation+GUARD,PITCH*H*sizeof(uint32_t));
    boot_progress_peripheral(PAGANI_KEYS,"must preserve panic",0,0,0);
    boot_progress_touch_spi(0,0,0,0,0,0);boot_progress_touch_smmu(0,0,0,0);
    boot_progress_touch_dma(0,0,0,0);boot_progress_touch_read(0,0,0,0,0,0,0,0);boot_progress_touch_power(0,0,0,0,0,0,0);
    boot_progress_touch_hid(0,0,0,0,0,0,0,0);boot_progress_peripheral_frame();
    assert(!memcmp(snapshot,allocation+GUARD,PITCH*H*sizeof(uint32_t)));
    assert(strstr(peripheral_status[PAGANI_KEYS],"PMIC sample"));
    assert(strstr(peripheral_status[PAGANI_SPI_FIFO],"in=4000/7fff out=8000/10000 mask=802"));
    free(snapshot);
    boot_progress_display_failed();
    assert(allocation[GUARD] == 0xffff4040 && !boot_progress_display_handoff());
    early_boot_progress_suspend();
    assert(!boot_progress_active());
    free(tables); free(allocation);
    // Real Odin geometry exercises all live observer rows and their right
    // edge without disturbing the deliberately tiny panic-layout fixture.
    enum { OW = 1080, OH = 1920, OP = 1088, OG = 32 };
    uint32_t *odin = malloc((OP * OH + 2 * OG) * sizeof(uint32_t));
    assert(odin);
    for (unsigned i = 0; i < OP * OH + 2 * OG; ++i) odin[i] = 0x12345678;
    args.Video.v_baseAddr = (uintptr_t)(odin + OG); args.Video.v_width = OW;
    args.Video.v_height = OH; args.Video.v_rowBytes = OP * 4;
    early_boot_progress_init(&args);
    for (unsigned i = 0; i < 24; ++i) assert(boot_progress_gpu_note(i,"BEFORE gpucc",0x3d99080,0));
    for (unsigned i = 0; i < 35; ++i) poll(NULL, NULL);
    for (unsigned i = 0; i < OG; ++i) {
        assert(odin[i] == 0x12345678 && odin[OG + OP * OH + i] == 0x12345678);
    }
    for (unsigned y = 0; y < OH; ++y)
        for (unsigned x = OW; x < OP; ++x) assert(odin[OG + y * OP + x] == 0x12345678);
    // Userland snapshot on the card log (odin-userland-log): off by default, one line per period, outside the console lock.
    assert(userland_log_period == 0 && ul_lines == 0);
    boot_progress_display_status("system frame copied", 6);
    boot_progress_display_status("key operation unavailable", 28);   // the status row now shows the AKS message...
    assert(system_frames == 6);                                        // ...but the frame counter kept its value
    boot_progress_aks_failed(28); boot_progress_aks_failed(31); boot_progress_aks_failed(200);
    odin_exit_count = 0; odin_sb_exits = odin_bb_exits = 0;           // earlier sections of this test also recorded exits
    boot_progress_exit(213, "SpringBoard", 5, 2, 5);
    boot_progress_exit(160, "backboardd", 0, 0, 0);
    boot_progress_exit(300, "mediaserverd", 9, 0, 0);
    odin_boot_main(1);
    userland_log_ticks = 0;
    for (unsigned i = 0; i < 10; ++i) poll(NULL, NULL);
    assert(ul_lines == 0 && ul_flushes == 0);                          // period 0: nothing reaches the card
    userland_log_period = 2; boot_progress_start();
    assert(userland_log_period == 5);                                  // each line is a 32 KiB card write: floor of 5 s
    userland_log_period = 5000; boot_progress_start();
    assert(userland_log_period == 3600);
    userland_log_period = 5; userland_log_ticks = 0;
    odin_fault_count = 0; ul_faults_reported = 0;                      // earlier sections of this test recorded aborts too
    odin_boot_fault(213, "SpringBoard", 0x92000047, 0x1000, 0x2000, 0x3000, 0x1000, 0x4000);
    // Change boot-time-review: the transport time, request count, code-signature pass and CPU busy share reach the line.
    gPEClockFrequencyInfo.timebase_frequency_hz = 24000000;            // 24000 ticks per millisecond
    native_ufs_block_reads = 12345; native_sd_read_us = 41672000ULL; native_sd_write_us = 16814000ULL;
    native_data_persist_loaded = 3 * 256;                              // 3 MiB of 4 KiB pages
    cs_native4k_bytes = 3000ULL << 20; cs_native4k_ready_blobs = 77;
    cs_native4k_read_ticks = 2000ULL * 24000; cs_native4k_hash_ticks = 500ULL * 24000;
    test_domain_on = 1;                                                // the first poll only primes the recount: four samples follow
    for (unsigned i = 0; i < 4; ++i) poll(NULL, NULL);
    assert(ul_lines == 0);
    poll(NULL, NULL);
    assert(ul_lines == 1 && ul_flushes == 1 && strlen(ul_text) < 448);
    assert(strstr(ul_text, " rdn=12345 rdms=41672 wrms=16814 dl=3 cs4k=3000/77/2000/500 busy=40/10"));
    assert(ul_busy_samples == 0 && ul_busy_sum[0] == 0 && ul_busy_sum[1] == 0);        // reported and reset
    assert(strstr(ul_text, "UL t=5 main=1 "));
    assert(strstr(ul_text, " frames=6 "));
    assert(strstr(ul_text, " sb=1(213:5:2:5) bb=1(160:0:0:0) last=mediaserverd:300:9 exits=3 "));
    assert(strstr(ul_text, " aks=90000000/3 "));
    assert(strstr(ul_text, " disp=key operation unavailable:1c "));
    for (unsigned i = 0; i < 5; ++i) poll(NULL, NULL);
    assert(ul_lines == 2 && strstr(ul_text, "UL t=10 "));
    // Change fairplayd-off: every abort reaches the card log once, with who/where, oldest first.
    assert(uf_lines == 1 && !strcmp(uf_text[0], "UF t=5 n=1 SpringBoard:213 esr=92000047 pc=1000 far=3000 fo=4000"));
    assert(strstr(ul_text, " ab=1 "));
    for (unsigned i = 0; i < 3; ++i) odin_boot_fault(300 + i, "backboardd", 0x92000004, 0x100931ad0 + i, 0, 0x10, 0x100930000ULL, 0x8000);
    for (unsigned i = 0; i < 5; ++i) poll(NULL, NULL);
    assert(uf_lines == 4 && strstr(ul_text, "UL t=15 ") && strstr(ul_text, " ab=4 "));
    assert(!strcmp(uf_text[1], "UF t=15 n=2 backboardd:300 esr=92000004 pc=100931ad0 far=10 fo=9ad0"));
    assert(strstr(uf_text[3], "n=4 backboardd:302 ") && strstr(uf_text[3], "pc=100931ad2 "));
    for (unsigned i = 0; i < 5; ++i) poll(NULL, NULL);                 // nothing new: nothing written
    assert(uf_lines == 4 && strstr(ul_text, "UL t=20 "));
    // A burst of 30 with a ring of 16 and a cap of 8 per period: the newest 8 are written and the rest is counted, not hidden.
    for (unsigned i = 0; i < 30; ++i) odin_boot_fault(400 + i, "mediaserverd", 0x92000045, 0x2000 + i, 0, 0, 0x1000, 0);
    for (unsigned i = 0; i < 5; ++i) poll(NULL, NULL);
    assert(ODIN_FAULTS == 16 && uf_lines == 4 + 1 + 8 && strstr(ul_text, " ab=34 "));
    assert(!strcmp(uf_text[4], "UF t=25 skipped=22"));
    assert(strstr(uf_text[5], "n=27 mediaserverd:422 ") && strstr(uf_text[12], "n=34 mediaserverd:429 "));
    for (unsigned i = 0; i < 5; ++i) poll(NULL, NULL);
    assert(uf_lines == 13);
    userland_log_period = 0;
    early_boot_progress_init(&args);                                   // a new boot starts the counters over
    assert(system_frames == 0 && aks_fail_mask == 0 && aks_fail_count == 0);
    early_boot_progress_suspend(); free(odin);
    puts("PASS: framebuffer/map bounds, root state, panic formatting, truncation, first-fault retention and frozen observer");
    return 0;
}
