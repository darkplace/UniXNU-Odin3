// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <stdint.h>
#include <stddef.h>
#include <stdarg.h>
#include "PeripheralStatus.h"
#include "DomainUsage.h"
extern int odin_cpu_domain_usage(uint64_t [2][2], unsigned [2]);
#ifndef BOOT_PROGRESS_TEST
#include <pexpert/pexpert.h>
#include <pexpert/arm64/boot.h>
#include <machine/machine_routines.h>
#include <kern/clock.h>
#include <kern/thread_call.h>
#include <kern/boot_progress.h>
#include <kern/early_boot_progress.h>
#include <arm64/proc_reg.h>
_Static_assert(ARM_PGBYTES == 4096 && (T0SZ_BOOT == 25 || T0SZ_BOOT == 26) && CACHE_ATTRINDX_WRITECOMB == 1,
               "Early framebuffer map requires the native 4 KiB bootstrap ABI");
#endif
#include <DiagFont.h>
#ifndef BOOT_PROGRESS_TEST
#include <BootLogo.h>
#endif

_Static_assert(offsetof(boot_args, Video) == 0x28, "DarwinBoot video ABI");
_Static_assert(offsetof(boot_args, physBase) == 0x10, "DarwinBoot memory ABI");

extern volatile uint32_t armdt_platform_configured, armcpu_registered, armcpu_gic_ready;
extern volatile uint64_t armcpu_timer_interrupts;
extern volatile uint32_t armcpu_gic_boot[8];
extern volatile uint32_t armcpu_smp_requested, armcpu_smp_running, armcpu_smp_conduit;
extern volatile uint32_t armcpu_smp_fail[8];
extern void early_boot_progress_vectors_done(void);
extern volatile int native_owner_cache_stage __attribute__((weak));
extern volatile uint64_t native_ufs_block_reads __attribute__((weak));
extern volatile uint64_t native_ufs_block_failures __attribute__((weak));
// Odin 3 microSD storage provider (devices/odin3-pagani/drivers/OdinSd): last log lines and counters, printed by the panic page.
extern char native_sd_diag[6][88];
extern volatile uint32_t native_sd_diag_count, native_sd_probe_calls, native_sd_integrity_failures, native_sd_windows_verified;
extern volatile uint64_t native_sd_verified_blocks;
// Measured SD transport throughput (OdinSd SdHost.h): bytes moved and microseconds spent inside transfers.
extern volatile uint64_t native_sd_read_bytes __attribute__((weak));
extern volatile uint64_t native_sd_read_us __attribute__((weak));
extern volatile uint64_t native_sd_write_bytes __attribute__((weak));
extern volatile uint64_t native_sd_write_us __attribute__((weak));
// Boot cost of the 4 KiB code-signature bridge (native-4k-signature-hash-cache.patch, ubc_subr.c): bytes read whole by
// cs_native4k_prepare, files hashed, and mach_absolute_time ticks spent in vn_rdwr (the storage driver included) and in the hashing.
extern uint64_t cs_native4k_read_ticks __attribute__((weak));
extern uint64_t cs_native4k_hash_ticks __attribute__((weak));
extern uint64_t cs_native4k_bytes __attribute__((weak));
extern unsigned long cs_native4k_ready_blobs __attribute__((weak));
// Data pages (4 KiB) served by the persistent store window instead of RAM (NativeRamBlock.cpp, sheng-data-persist=2): each one is an SD read.
extern volatile uint64_t native_data_persist_loaded __attribute__((weak));
// Stack margin probe (changes sd-stack-probe, mount-panic): bytes still free on the kernel stack, smallest value wins.
// STACK_UNREACHED = never sampled; negative = SP below the stack base (guard page). Same rule as OdinSd/StackFold.h.
#define STACK_UNREACHED INT64_MAX
extern volatile int64_t native_sd_stack_min[12];
static int64_t stack_stage_min = STACK_UNREACHED, stack_exc_min = STACK_UNREACHED;
// Thread that runs load_init_program (the Data mount), recorded by kern_exec.c; compared with the panicking thread.
volatile uint64_t native_init_thread;
// Userland snapshot on the card log (change ui-handoff-rca): after the display handoff the page is hidden, so a stuck logo is
// blind. With odin-userland-log=<seconds> one line per period goes to the XNSDLOG1 region through the SD driver. Off by
// default. Weak: targets without OdinSd have no log region.
extern void native_sd_log_note(const char *) __attribute__((weak));
extern _Bool native_sd_log_flush(void) __attribute__((weak));
static unsigned userland_log_period;
static uint64_t userland_log_ticks, system_frames, aks_fail_mask;
static uint64_t ul_faults_reported;      // aborts already written as UF lines; the ring keeps the newest ODIN_FAULTS
static unsigned aks_fail_count;
// Mean CPU busy share (recount, all psets) between two UL lines: poll() adds one sample per second, userland_snapshot() reports and resets.
static uint64_t ul_busy_sum[2], ul_busy_samples;

extern unsigned vm_page_free_count;
extern uint32_t memorystatus_available_pages;

// These values are also read by the stackless fatal exception entry.
uint64_t early_boot_args_pa, early_boot_phys_base, early_boot_virt_base;
uint64_t early_boot_stage;
static const uint64_t font[] = GLYPH_FONT;
static uintptr_t framebuffer, framebuffer_pa;
static unsigned width, height, stride, column, row = 6;
static uint64_t poll_count;
static uint64_t last_system_frame_tick;
static unsigned diag_resume_seconds;
static thread_call_t poll_call;
static int display_owned, logo_visible;
static int busy;
static unsigned console_lock, console_smp;
static uint64_t panic_cpu;
enum { SCALE = 2, CELL = 6 * SCALE, LINE = 14 * SCALE, LOG_TOP = 9 };  // Odin 3 panel 1080x1920: scale 2 gives 90x68 cells; scale 3 clipped the panic page (docs/builds/odin3-panic-20261028.md). LOG_TOP keeps the 252 px logo band.
enum { HISTORY = 24, STAGE_BYTES = 80 };   // 8 on the Pagani base: the Odin 3 panel fits 68 rows at scale 2, and the storage driver adds ~10 milestones
static char stages[HISTORY][STAGE_BYTES], root_stage[STAGE_BYTES], root_detail[192];
static char display_stage[STAGE_BYTES], exit_stage[STAGE_BYTES];
static uint64_t display_value;
static unsigned stage_count, panic_state, panic_limit, panic_clipped;
static uint64_t root_sequence, root_value, root_tick, fault_esr, fault_pc, fault_far, fault_psr;
static int fault_valid;
char boot_progress_panic_text[2048];
extern int vsnprintf(char *, size_t, const char *, va_list);
extern int snprintf(char *, size_t, const char *, ...);
static char storage_status[176];   // worst case of the seven 64-bit counters in the format below; line_raw still stops at the screen edge
static char timer_status[96];
static char display_status[96];
static char cpu_status[96];
static char geometry_status[96], domain_status[96];
static odin_domain_previous domain_previous;
static char gpu_survey_rows[24][96];
static unsigned gpu_survey_count;
static char peripheral_status[PAGANI_PERIPHERALS][80];
static char fan_status[3][80];
static int peripheral_overlay, platform_overlay;
extern unsigned processor_count, processor_avail_count;
extern unsigned long gPhysSize;
extern volatile uint32_t armcpu_core_timer[8], armcpu_core_sgi[8];

static void barrier(void)
{
#ifndef BOOT_PROGRESS_TEST
    __asm__ volatile("dsb sy" ::: "memory");
#endif
}

static uint64_t irq_disable(void)
{
    uint64_t flags = 0;
#ifndef BOOT_PROGRESS_TEST
    __asm__ volatile("mrs %0, daif\n\tmsr daifset, #15" : "=r"(flags) :: "memory");
#endif
    return flags;
}

static void irq_enable(uint64_t flags)
{
#ifndef BOOT_PROGRESS_TEST
    __asm__ volatile("msr daif, %0" :: "r"(flags) : "memory");
#else
    (void)flags;
#endif
}

static uint64_t console_cpu(void)
{
    uint64_t cpu = 0;
#ifndef BOOT_PROGRESS_TEST
    __asm__ volatile("mrs %0, mpidr_el1" : "=r"(cpu));
#else
    cpu = test_cpu;
#endif
    return (cpu & 0xff00ffffffULL) + 1;
}

static int panic_other_cpu(void)
{
    uint64_t owner = __atomic_load_n(&panic_cpu, __ATOMIC_ACQUIRE);
    return owner && owner != console_cpu();
}

// Skip contended diagnostics; a stopped CPU must never block the panic screen.
static uint64_t irq_save(void)
{
    uint64_t flags = irq_disable();
    if (__atomic_load_n(&panic_cpu, __ATOMIC_ACQUIRE) ||
        (console_smp && __atomic_exchange_n(&console_lock, 1, __ATOMIC_ACQUIRE))) {
        irq_enable(flags);
        return UINT64_MAX;
    }
    return flags;
}

static void irq_restore(uint64_t flags)
{
    if (console_smp) __atomic_store_n(&console_lock, 0, __ATOMIC_RELEASE);
    irq_enable(flags);
}

static uint64_t frequency(void)
{
    uint64_t value = 19200000;
#ifndef BOOT_PROGRESS_TEST
    __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(value));
#endif
    return value;
}

static uint64_t counter(void)
{
    uint64_t value = 0;
#ifndef BOOT_PROGRESS_TEST
    __asm__ volatile("mrs %0, cntvct_el0" : "=r"(value));
#endif
    return value;
}

static void copy_line(char *to, size_t size, const char *from)
{
    size_t i = 0;
    if (from) for (; i + 1 < size && from[i]; i++)
        to[i] = from[i] >= 32 && from[i] <= 126 ? from[i] : ' ';
    to[i] = 0;
}

static void record_stage(const char *s)
{
    copy_line(stages[stage_count % HISTORY], STAGE_BYTES, s);
    stage_count++;
}

static int geometry(uintptr_t base, uint64_t w, uint64_t h, uint64_t pitch, uint64_t depth)
{
    return base != 0 && (base & 3) == 0 && (depth & 255) == 32 &&
        w >= CELL && w <= 8192 && h >= (LOG_TOP + 1) * LINE && h <= 8192 &&
        pitch >= w * 4 && pitch <= 32768 && (pitch & 3) == 0 &&
        pitch * h <= 64 * 1024 * 1024 && base <= UINTPTR_MAX - pitch * h;
}

static void clear_line(unsigned y)
{
    if (panic_other_cpu()) return;
    if ((display_owned || (logo_visible && y >= LOG_TOP * LINE)) && !panic_state) return;
    if (!framebuffer || y + LINE > height) return;
    for (unsigned dy = 0; dy < LINE; dy++) {
        volatile uint32_t *pixels = (volatile uint32_t *)framebuffer + (y + dy) * stride;
        for (unsigned x = 0; x < width; x++) pixels[x] = 0xff000000;
    }
}

static void render_boot_logo(void)
{
    int left = ((int)width - BOOT_LOGO_WIDTH) / 2 + BOOT_LOGO_X;
    int top = ((int)height - BOOT_LOGO_HEIGHT) / 2 + BOOT_LOGO_Y;
    if (!framebuffer || left < 0 || top < 0 || left + BOOT_LOGO_WIDTH > (int)width ||
        top + BOOT_LOGO_HEIGHT > (int)height) return;
    for (unsigned y = 0; y < height; y++) {
        volatile uint32_t *line = (volatile uint32_t *)framebuffer + y * stride;
        for (unsigned x = 0; x < width; x++) line[x] = 0xff000000;
    }
    for (unsigned y = 0; y < BOOT_LOGO_HEIGHT; y++) {
        volatile uint32_t *line = (volatile uint32_t *)framebuffer + (top + y) * stride + left;
        for (unsigned x = 0; x < BOOT_LOGO_WIDTH; x++) {
            uint32_t level = boot_logo[y * BOOT_LOGO_WIDTH + x];
            line[x] = 0xff000000 | level * 0x010101;
        }
    }
    logo_visible = 1;
}

static void glyph_pixels(unsigned char c, unsigned x, unsigned y, uint32_t color)
{
    if (panic_other_cpu()) return;
    if (!framebuffer || x + CELL > width || y + LINE > height) return;
    if (c < 32 || c > 126) c = '?';
    uint64_t bits = font[c - 32];
    for (unsigned gy = 0; gy < 12; gy++) {
        unsigned scan = (unsigned)((gy < 6 ? bits >> 32 : bits & 0xffffffff) >> ((gy % 6) * 5));
        for (unsigned gx = 0; gx < 6; gx++) {
            uint32_t pixel = gx < 5 && ((scan >> gx) & 1) ? color : 0xff000000;
            for (unsigned dy = 0; dy < SCALE; dy++) {
                volatile uint32_t *line = (volatile uint32_t *)framebuffer + (y + gy * SCALE + dy) * stride + x + gx * SCALE;
                for (unsigned dx = 0; dx < SCALE; dx++) line[dx] = pixel;
            }
        }
    }
}

static void glyph(unsigned char c, unsigned x, unsigned y, uint32_t color)
{
    if ((display_owned || (logo_visible && y >= LOG_TOP * LINE)) && !panic_state) return;
    glyph_pixels(c, x, y, color);
}

static void putc_raw(char c)
{
    if (panic_other_cpu()) return;
    if (!framebuffer) return;
    if (panic_state && row >= panic_limit) { panic_clipped = 1; return; }
    if (c == '\t') c = ' ';
    if (c == '\r') { column = 0; return; }
    if (c == '\n' || (column + 1) * CELL > width) {
        column = 0;
        row++;
        if (panic_state && row >= panic_limit) { panic_clipped = 1; return; }
        if ((row + 1) * LINE > height) row = LOG_TOP;
        clear_line(row * LINE);
        if (c == '\n') return;
    }
    glyph((unsigned char)c, column++ * CELL, row * LINE, 0xffffffff);
}

static void text_raw(const char *s)
{
    while (*s) putc_raw(*s++);
}

static void line_raw(const char *s)
{
    while (*s && column < width / CELL) putc_raw(*s++);
}

static void hex_raw(uint64_t n)
{
    for (int shift = 60; shift >= 0; shift -= 4) {
        unsigned digit = (unsigned)(n >> shift) & 15;
        putc_raw((char)(digit < 10 ? '0' + digit : 'a' + digit - 10));
    }
}

static void value_raw(const char *name, uint64_t n)
{
    text_raw(name); text_raw(" 0x"); hex_raw(n); putc_raw('\n');
}

// Margin of the current kernel stack, in bytes. Only read: no allocation and no lock.
static int64_t stack_remaining_bytes(void)
{
#ifdef BOOT_PROGRESS_TEST
    return 0;
#else
    return (int64_t)ml_stack_remaining();
#endif
}

static void stack_note(int64_t *slot)
{
    int64_t remaining = stack_remaining_bytes();
    if (*slot == STACK_UNREACHED || remaining < *slot) *slot = remaining;
}

// Always four characters (a stage line is 80 bytes wide): "----" never sampled, "-xxx" that many bytes below the stack
// base, "????" not a thread-stack reading (ml_stack_remaining on the exception stack returns garbage), else 4 hex digits.
static void hex_margin(int64_t remaining)
{
    static const char digits[] = "0123456789abcdef";
    if (remaining == STACK_UNREACHED) { text_raw("----"); return; }
    if (remaining < 0) {
        uint64_t below = (uint64_t)(-remaining);
        if (below > 0xfff) { text_raw("-big"); return; }
        putc_raw('-');
        for (int shift = 8; shift >= 0; shift -= 4) putc_raw(digits[(below >> shift) & 15]);
        return;
    }
    if (remaining > 0xffff) { text_raw("????"); return; }
    for (int shift = 12; shift >= 0; shift -= 4) putc_raw(digits[((unsigned)remaining >> shift) & 15]);
}

void boot_progress_bind(uintptr_t base, unsigned w, unsigned h, unsigned pitch, unsigned depth)
{
    if (panic_state) return;
    if (!geometry(base, w, h, pitch, depth)) return;
    framebuffer = base; width = w; height = h; stride = pitch / 4;
    if ((row + 1) * LINE > height) row = LOG_TOP;
    if ((column + 1) * CELL > width) column = 0;
}

// Stop diagnostic drawing before the first system frame.
int boot_progress_display_handoff(void)
{
    uint64_t flags;
    // A skipped handoff strands the pending swap; wait for the bounded writer.
    while ((flags = irq_save()) == UINT64_MAX) {
        if (__atomic_load_n(&panic_cpu, __ATOMIC_ACQUIRE)) return 0;
#ifndef BOOT_PROGRESS_TEST
        __asm__ volatile("yield" ::: "memory");
#endif
    }
    int ready = framebuffer && !panic_state && !busy;
    if (ready) { display_owned = 1; last_system_frame_tick = poll_count; barrier(); }
    irq_restore(flags);
    return ready;
}

void boot_progress_display_failed(void)
{
    uint64_t flags = irq_save();
    if (flags == UINT64_MAX) return;
    if (!panic_state) display_owned = 0;
    irq_restore(flags);
}

int boot_progress_active(void) { return framebuffer != 0; }

void boot_progress_putc(char c)
{
    uint64_t flags = irq_save();
    if (flags == UINT64_MAX) return;
    if (!busy && !panic_state) { busy = 1; putc_raw(c); barrier(); busy = 0; }
    irq_restore(flags);
}

void boot_progress_stage(const char *s)
{
    uint64_t flags = irq_save();
    if (flags == UINT64_MAX) return;
    if (!busy && !panic_state) {
        busy = 1;
        record_stage(s);
        if (logo_visible) {
            unsigned saved_row = row, saved_column = column;
            row = 0; column = 0; clear_line(0); line_raw(s);
            row = saved_row; column = saved_column;
        }
        if (column) putc_raw('\n');
        text_raw("[XNU] "); text_raw(s); putc_raw('\n'); barrier();
        busy = 0;
    }
    irq_restore(flags);
}

void boot_progress_root(const char *stage, const char *detail, uint64_t value)
{
    uint64_t flags = irq_save();
    if (flags == UINT64_MAX) return;
    if (!panic_state && !busy) {
        busy = 1;
        copy_line(root_stage, sizeof(root_stage), stage);
        copy_line(root_detail, sizeof(root_detail), detail);
        root_value = value; root_tick = counter(); root_sequence++;
        record_stage(stage);
        char *entry = stages[(stage_count - 1) % HISTORY];
        unsigned n = 0;
        while (entry[n]) n++;
        if (root_detail[0] && n + 3 < STAGE_BYTES) {
            entry[n++] = ':'; entry[n++] = ' ';
            copy_line(entry + n, STAGE_BYTES - n, root_detail);
            // n must end at the NUL: the margin suffix below starts there, not on top of the detail.
            while (entry[n]) n++;
        }
        {
            int64_t remaining = stack_remaining_bytes();
            stack_note(&stack_stage_min);
            if (n + 8 < STAGE_BYTES) {
                static const char digits[] = "0123456789abcdef";
                entry[n++] = ' '; entry[n++] = 'k'; entry[n++] = '=';
                if (remaining < 0) { entry[n++] = '-'; entry[n++] = '-'; entry[n++] = '-'; entry[n++] = '-'; }
                else for (int shift = 12; shift >= 0; shift -= 4) entry[n++] = digits[((unsigned)remaining >> shift) & 15];
                entry[n] = 0;
            }
        }
        if (column) putc_raw('\n');
        text_raw("[ROOT] "); text_raw(root_stage); text_raw(" value=0x"); hex_raw(value); putc_raw('\n');
        if (root_detail[0]) { text_raw(root_detail); putc_raw('\n'); }
        unsigned saved_row = row, saved_column = column;
        row = 3; column = 0; clear_line(3 * LINE);
        text_raw("ROOT ");
        for (unsigned i = 0; root_stage[i] && column < width / CELL; i++) putc_raw(root_stage[i]);
        text_raw(" k="); hex_margin(stack_remaining_bytes());
        row = saved_row; column = saved_column;
        barrier(); busy = 0;
    }
    irq_restore(flags);
}

static int starts_with(const char *text, const char *prefix)
{
    while (*prefix) if (*text++ != *prefix++) return 0;
    return 1;
}

void boot_progress_display_status(const char *detail, uint64_t value)
{
    uint64_t flags = irq_save();
    if (flags == UINT64_MAX) return;
    if (!panic_state && !busy) {
        copy_line(display_stage, sizeof(display_stage), detail);
        display_value = value;
        // The status row keeps only the latest message; the frame counter must survive the next one (AKS rows, client rows).
        if (detail && starts_with(detail, "system frame copied")) system_frames = value;
    }
    irq_restore(flags);
    boot_progress_root("stage7", detail, value);
}

// An AppleKeyStore selector the stub answered with an error (Stage7Probe native_aks_call): remembered as a bit, because the
// status row above is overwritten by the next message.
void boot_progress_aks_failed(unsigned selector)
{
    if (selector < 64) __atomic_or_fetch(&aks_fail_mask, 1ULL << selector, __ATOMIC_RELAXED);
    __atomic_add_fetch(&aks_fail_count, 1, __ATOMIC_RELAXED);
}

void boot_progress_display_geometry(unsigned pw,unsigned ph,uint64_t pitch,
    unsigned sw,unsigned sh,unsigned iw,unsigned ih,uint64_t sourcePitch)
{
    uint64_t flags=irq_save();if(flags==UINT64_MAX)return;
    snprintf(geometry_status,sizeof geometry_status,"GOP %ux%u p%llu SYS %ux%u SURF %ux%u p%llu",
        pw,ph,(unsigned long long)pitch,sw,sh,iw,ih,(unsigned long long)sourcePitch);
    irq_restore(flags);
}

// Unlike ordinary lossy diagnostics, a probe may proceed only after this returns 1.
// The row is drawn synchronously above the logo, saved for polling, and DSB'ed.
// No waiting on another CPU: failure to acquire the console cancels the survey.
int boot_progress_gpu_note(unsigned index, const char *detail, uint64_t value, uint64_t status)
{
    if (index >= 24) return 0;
    uint64_t flags = irq_save();
    if (flags == UINT64_MAX) return 0;
    int ready = framebuffer && !busy && !panic_state && !display_owned;
    if (ready) {
        busy = 1;
        snprintf(gpu_survey_rows[index], sizeof(gpu_survey_rows[index]),
                 "G1 %02u %.36s %llx st=%llx", index, detail,
                 (unsigned long long)value, (unsigned long long)status);
        if (gpu_survey_count <= index) gpu_survey_count = index + 1;
        record_stage(gpu_survey_rows[index]);
        unsigned saved_row = row, saved_column = column;
        row = 4; column = 0; clear_line(4 * LINE); line_raw(gpu_survey_rows[index]);
        row = saved_row; column = saved_column;
        barrier(); busy = 0;
    }
    irq_restore(flags);
    return ready;
}

void boot_progress_peripheral(unsigned device, const char *stage, int32_t result, uint32_t value, uint64_t reports)
{
    if (device >= PAGANI_SPI) return;
    static const char *names[] = {"KEYS", "TOUCH", "PANEL"};
    uint64_t flags = irq_save();
    if (flags == UINT64_MAX) return;
    if (!panic_state) snprintf(peripheral_status[device], sizeof(peripheral_status[device]),
        "%s %.18s r=%x v=%x n=%llu", names[device], stage ? stage : "?",
        (unsigned)result, value, (unsigned long long)reports);
    irq_restore(flags);
}

void boot_progress_fan(unsigned line, const char *text)
{
    if (line >= 3 || !text) return;
    uint64_t flags = irq_save();
    if (flags == UINT64_MAX) return;
    if (!panic_state) snprintf(fan_status[line], sizeof(fan_status[line]), "%.79s", text);
    irq_restore(flags);
}

void boot_progress_touch_spi(uint32_t firmware, uint32_t disabled, uint32_t state, uint32_t irq, uint32_t tx, uint32_t rx)
{
    uint64_t flags = irq_save();
    if (flags == UINT64_MAX) return;
    if (!panic_state) {
        snprintf(peripheral_status[PAGANI_SPI], sizeof(peripheral_status[PAGANI_SPI]),
            "SPI fw=%x if=%x st=%x irq=%x", firmware, disabled, state, irq);
        snprintf(peripheral_status[PAGANI_SPI_FIFO], sizeof(peripheral_status[PAGANI_SPI_FIFO]),
            "SPI FIFO tx=%x rx=%x", tx, rx);
    }
    irq_restore(flags);
}

void boot_progress_touch_dma(uint32_t tx, uint32_t rx, uint32_t txin, uint32_t rxin)
{
    uint64_t flags = irq_save();
    if (flags == UINT64_MAX) return;
    if (!panic_state) snprintf(peripheral_status[PAGANI_SPI_FIFO], sizeof(peripheral_status[PAGANI_SPI_FIFO]),
        "SPI DMA tx=%x rx=%x ti=%x ri=%x", tx, rx, txin, rxin);
    irq_restore(flags);
}

void boot_progress_touch_read(uint32_t length, uint32_t head, uint32_t pins, uint32_t dma,
                              uint32_t state, uint32_t txin, uint32_t rxin, uint32_t delay)
{
    uint64_t flags = irq_save();
    if (flags == UINT64_MAX) return;
    if (!panic_state) {
        snprintf(peripheral_status[PAGANI_SPI], sizeof(peripheral_status[PAGANI_SPI]),
            "SPI RX n=%x h=%08x a=%x/%x rst=%x", length, head, pins&1, (pins>>1)&1, (pins>>2)&3);
        snprintf(peripheral_status[PAGANI_SPI_FIFO], sizeof(peripheral_status[PAGANI_SPI_FIFO]),
            "SPI dma=%x ti=%x ri=%x st=%x dly=%x", dma, txin, rxin, state, delay);
    }
    irq_restore(flags);
}

void boot_progress_touch_power(uint32_t address, uint32_t hardware, uint32_t tcs, uint32_t state,
                               uint32_t cmd0, uint32_t cmd1, uint32_t cmd2)
{
    uint64_t flags = irq_save();
    if (flags == UINT64_MAX) return;
    if (!panic_state) {
        snprintf(peripheral_status[PAGANI_SPI], sizeof(peripheral_status[PAGANI_SPI]),
            "PWR a=%x hw=%x t=%x", address, hardware, tcs);
        snprintf(peripheral_status[PAGANI_SPI_FIFO], sizeof(peripheral_status[PAGANI_SPI_FIFO]),
            "PWR st=%x c=%x/%x/%x", state, cmd0, cmd1, cmd2);
    }
    irq_restore(flags);
}

void boot_progress_touch_hid(uint32_t binding, uint32_t usage, uint32_t events, uint32_t digitizers,
                             uint32_t edges, uint32_t input, uint64_t output, uint32_t mask)
{
    uint64_t flags = irq_save();
    if (flags == UINT64_MAX) return;
    if (!panic_state) {
        snprintf(peripheral_status[PAGANI_SPI], sizeof(peripheral_status[PAGANI_SPI]),
            "HID b=%x use=%x/%x ev=%u/%u e=%u", binding, usage>>16, usage&0xffff, events, digitizers, edges);
        snprintf(peripheral_status[PAGANI_SPI_FIFO], sizeof(peripheral_status[PAGANI_SPI_FIFO]),
            "XY in=%x/%x out=%x/%x mask=%x", input>>16, input&0xffff, (uint32_t)(output>>32), (uint32_t)output, mask);
    }
    irq_restore(flags);
}

void boot_progress_touch_smmu(uint32_t slot, uint32_t bank, uint32_t smr, uint32_t s2cr)
{
    uint64_t flags = irq_save();
    if (flags == UINT64_MAX) return;
    if (!panic_state) {
        snprintf(peripheral_status[PAGANI_SPI], sizeof(peripheral_status[PAGANI_SPI]),
            "SMMU slot=%x bank=%x", slot, bank);
        snprintf(peripheral_status[PAGANI_SPI_FIFO], sizeof(peripheral_status[PAGANI_SPI_FIFO]),
            "SMMU smr=%x s2cr=%x", smr, s2cr);
    }
    irq_restore(flags);
}

void boot_progress_peripheral_enable(int enabled)
{
    __atomic_store_n(&peripheral_overlay, enabled != 0, __ATOMIC_RELEASE);
}

void boot_progress_platform_enable(int enabled)
{
    __atomic_store_n(&platform_overlay, enabled != 0, __ATOMIC_RELEASE);
}

// Only the opted-in status rows may repaint after console handoff.
void boot_progress_peripheral_frame(void)
{
    int platform = __atomic_load_n(&platform_overlay, __ATOMIC_ACQUIRE);
    if (!platform && !__atomic_load_n(&peripheral_overlay, __ATOMIC_ACQUIRE)) return;
    uint64_t flags = irq_save();
    if (flags == UINT64_MAX) return;
    if (!display_owned || panic_state || busy || !framebuffer) { irq_restore(flags); return; }
    char line[80];
    for (unsigned i = 0; i < (platform ? 2U : PAGANI_PERIPHERALS); i++) {
        if (platform && i == 0) {
            unsigned timer = 0, ipi = 0;
            for (unsigned cpu = 0; cpu < 8; cpu++) {
                if (__atomic_load_n(&armcpu_core_timer[cpu], __ATOMIC_RELAXED)) timer |= 1U << cpu;
                if (__atomic_load_n(&armcpu_core_sgi[cpu], __ATOMIC_RELAXED)) ipi |= 1U << cpu;
            }
            snprintf(line, sizeof(line), "CPU reg=%u online=%u T=%02x I=%02x",
                     processor_count, processor_avail_count, timer, ipi);
        } else if (platform) {
            snprintf(line, sizeof(line), "RAM MiB=%lu free=%u avail=%u", gPhysSize >> 20,
                     vm_page_free_count >> 8, memorystatus_available_pages >> 8);
        } else if (!peripheral_status[i][0] && i >= PAGANI_PANEL && fan_status[i - PAGANI_PANEL][0])
            copy_line(line, sizeof(line), fan_status[i - PAGANI_PANEL]);
        else copy_line(line, sizeof(line), peripheral_status[i]);
        if (!line[0]) continue;
        unsigned at = 0;
        for (unsigned x = 0; x + CELL <= width; x += CELL) {
            if (panic_state) { irq_restore(flags); return; }
            unsigned char c = line[at];
            if (c) at++;
            glyph_pixels(c ? c : ' ', x, (i + 4) * LINE, 0xffffffff);
        }
    }
    barrier(); irq_restore(flags);
}

static void panic_halt(void)
{
    barrier();
#ifndef BOOT_PROGRESS_TEST
    for (;;) __asm__ volatile("wfe");
#endif
}

static void panic_open(const char *title)
{
    panic_state = 1; busy = 1; panic_clipped = 0;
    panic_limit = height / LINE - 2;
    for (unsigned y = 0; y + LINE <= height; y += LINE) clear_line(y);
    for (unsigned y = 0; y < 12; y++) {
        volatile uint32_t *pixels = (volatile uint32_t *)framebuffer + y * stride;
        for (unsigned x = 0; x < width; x++)
            pixels[x] = x < width / 3 ? 0xffff4040 : x < 2 * width / 3 ? 0xff40ff40 : 0xff4080ff;
    }
    row = 1; column = 0; text_raw(title); putc_raw('\n');
}

static void panic_footer(const char *status)
{
    unsigned clipped = panic_clipped;
    panic_limit = height / LINE;
    row = panic_limit - 2; column = 0; clear_line(row * LINE); clear_line((row + 1) * LINE);
    text_raw(clipped ? "SCREEN TEXT CLIPPED\n" : "\n");
    text_raw(status); barrier();
}

#if defined(__clang__)
#pragma clang optimize off
#endif
// -O2 en este toolchain descarta los literales de esta funcion (objeto sin MESSAGE ni
// RECENT STAGES, comprobado 2026-10-30): se compila sin optimizar para que la pagina salga.
void boot_progress_panic(const char *format, va_list *args, unsigned long caller,
                         unsigned reason, uint64_t options)
{
    if (!framebuffer) return;
    uint64_t flags = irq_disable();
    uint64_t owner = 0, cpu = console_cpu();
    if (!__atomic_compare_exchange_n(&panic_cpu, &owner, cpu, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE) && owner != cpu) {
        panic_halt(); return;
    }
    if (panic_state) {
        panic_footer("NESTED PANIC: first message preserved"); panic_halt(); return;
    }
    uint64_t sp = 0, fp = 0, mpidr = 0, sctlr = 0, tcr = 0, ttbr1 = 0;
#ifndef BOOT_PROGRESS_TEST
    __asm__ volatile("mov %0, sp" : "=r"(sp));
    __asm__ volatile("mov %0, x29" : "=r"(fp));
    __asm__ volatile("mrs %0, mpidr_el1" : "=r"(mpidr));
    __asm__ volatile("mrs %0, sctlr_el1" : "=r"(sctlr));
    __asm__ volatile("mrs %0, tcr_el1" : "=r"(tcr));
    __asm__ volatile("mrs %0, ttbr1_el1" : "=r"(ttbr1));
#endif
    panic_open("AYN Odin 3 / XNU PANIC");
    text_raw("panic entry reached; values below are hexadecimal\n");
    value_raw("caller", caller); value_raw("MPIDR", mpidr);
    text_raw("last stage: "); text_raw(stage_count ? stages[(stage_count - 1) % HISTORY] : "none"); putc_raw('\n');
    text_raw("root: "); text_raw(root_sequence ? root_stage : "not entered"); putc_raw('\n');
    if (root_sequence) {
        value_raw("root seq", root_sequence); value_raw("root value", root_value);
        uint64_t hz = frequency();
        value_raw("root stage age (seconds)", hz ? (counter() - root_tick) / hz : 0);
    }
    unsigned message_row = row;
    text_raw("MESSAGE (formatting...)\n"); barrier();
    if (format && args) {
        va_list copy;
        va_copy(copy, *args);
        int n = vsnprintf(boot_progress_panic_text, sizeof(boot_progress_panic_text), format, copy);
        va_end(copy);
        if (n < 0) copy_line(boot_progress_panic_text, sizeof(boot_progress_panic_text), "<panic formatting failed>");
        if (n >= (int)sizeof(boot_progress_panic_text)) panic_clipped = 1;
    } else {
        copy_line(boot_progress_panic_text, sizeof(boot_progress_panic_text), format ? format : "<no panic format>");
    }
    for (unsigned r = message_row; r < row && r < panic_limit; r++) clear_line(r * LINE);
    row = message_row; column = 0; text_raw("MESSAGE\n");
    text_raw(boot_progress_panic_text); putc_raw('\n');
    text_raw("PANIC HANDLER STATE\n");
    text_raw("SP "); hex_raw(sp); text_raw(" FP "); hex_raw(fp); putc_raw('\n');
    text_raw("DAIF "); hex_raw(flags); text_raw(" reason "); hex_raw(reason); putc_raw('\n');
    value_raw("options", options); value_raw("SCTLR_EL1", sctlr);
    value_raw("TCR_EL1", tcr); value_raw("TTBR1_EL1", ttbr1);
    if (fault_valid) {
        text_raw("RECORDED KERNEL EXCEPTION\n");
        value_raw("ESR_EL1", fault_esr); value_raw("ELR_EL1", fault_pc);
        value_raw("FAR_EL1", fault_far); value_raw("SPSR_EL1", fault_psr);
    } else text_raw("No fatal exception recorded before this panic\n");
    text_raw("poll "); hex_raw(poll_count); text_raw(" timer "); hex_raw(armcpu_timer_interrupts); putc_raw('\n');
    text_raw("SD DRIVER probe calls "); hex_raw(native_sd_probe_calls); text_raw(" log lines "); hex_raw(native_sd_diag_count); putc_raw('\n');
    if (&native_ufs_block_reads && &native_ufs_block_failures) {
        text_raw("SD reads "); hex_raw(native_ufs_block_reads); text_raw(" errors "); hex_raw(native_ufs_block_failures);
        text_raw(" verified4K "); hex_raw(native_sd_verified_blocks); text_raw(" integrity "); hex_raw(native_sd_integrity_failures);
        text_raw(" windows "); hex_raw(native_sd_windows_verified); putc_raw('\n');
    }
    text_raw("STK stage "); hex_margin(stack_stage_min);
    text_raw(" exc "); hex_margin(stack_exc_min);
    text_raw(" now "); hex_margin(stack_remaining_bytes()); putc_raw('\n');
#ifndef BOOT_PROGRESS_TEST
    for (unsigned base = 0; base < 12; base += 4) {
        text_raw("STK");
        for (unsigned i = base; i < base + 4; i++) {
            text_raw(" m"); putc_raw((char)(i < 10 ? '0' + i : 'a' + i - 10)); putc_raw(' ');
            hex_margin(native_sd_stack_min[i]);
        }
        putc_raw('\n');
    }
    {
        // Same thread as the one that runs load_init_program? (pid 1 does the Data mount.)
        uint64_t me = 0;
        __asm__ volatile("mrs %0, tpidr_el1" : "=r"(me));
        text_raw("THREAD panic "); hex_raw(me); text_raw(" init "); hex_raw(native_init_thread);
        text_raw(me == native_init_thread ? " SAME\n" : " other\n");
    }
#endif
    {
        unsigned lines = native_sd_diag_count < 6 ? native_sd_diag_count : 6;
        for (unsigned i = native_sd_diag_count - lines; i < native_sd_diag_count; i++) { text_raw(native_sd_diag[i % 6]); putc_raw('\n'); }
    }
    text_raw("RECENT STAGES (oldest first)\n");
    unsigned count = stage_count < HISTORY ? stage_count : HISTORY;
    for (unsigned i = stage_count - count; i < stage_count; i++) {
        text_raw(stages[i % HISTORY]); putc_raw('\n');
    }
    panic_footer("HALTED: no software reboot"); panic_halt();
}

#if defined(__clang__)
#pragma clang optimize on
#endif

// No kernel services, stack protector, SIMD, or relocated pointers on this path.
void early_boot_progress_init(boot_args *args)
{
    console_smp = 0; console_lock = 0; panic_cpu = 0;
    early_boot_args_pa = (uintptr_t)args;
    early_boot_phys_base = args->physBase;
    early_boot_virt_base = args->virtBase;
    early_boot_stage = 1;
    display_owned = 0; logo_visible = 0; panic_state = 0; stage_count = 0; root_sequence = 0; fault_valid = 0;
    poll_count = 0; display_value = 0;
    display_stage[0] = 0; exit_stage[0] = 0;
    geometry_status[0] = 0; domain_previous.valid = 0;
    peripheral_overlay = 0; platform_overlay = 0;
    userland_log_ticks = 0; system_frames = 0; aks_fail_mask = 0; aks_fail_count = 0; ul_faults_reported = 0;
    ul_busy_sum[0] = ul_busy_sum[1] = 0; ul_busy_samples = 0;
    gpu_survey_count = 0;
    for (unsigned i = 0; i < 24; ++i) gpu_survey_rows[i][0] = 0;
    for (unsigned i = 0; i < PAGANI_PERIPHERALS; i++) peripheral_status[i][0] = 0;
    for (unsigned i = 0; i < 3; i++) fan_status[i][0] = 0;
    boot_progress_panic_text[0] = 0;
    framebuffer = 0; column = 0; row = LOG_TOP; busy = 0;
    framebuffer_pa = 0;
    if (!geometry(args->Video.v_baseAddr, args->Video.v_width, args->Video.v_height,
                  args->Video.v_rowBytes, args->Video.v_depth)) return;
    boot_progress_bind(args->Video.v_baseAddr, (unsigned)args->Video.v_width, (unsigned)args->Video.v_height,
                       (unsigned)args->Video.v_rowBytes, (unsigned)args->Video.v_depth);
    framebuffer_pa = framebuffer;
    for (unsigned y = 0; y + LINE <= height; y += LINE) clear_line(y);
    unsigned saved = row;
    row = 0; text_raw("AYN Odin 3 / source XNU"); row = saved; column = 0;
    boot_progress_stage("S1: XNU entry, MMU off");
    value_raw("args PA", (uintptr_t)args);
    value_raw("RAM PA", args->physBase);
    value_raw("RAM VA", args->virtBase);
    value_raw("RAM bytes", args->memSize);
    value_raw("CNTFRQ Hz", frequency());
    render_boot_logo();
    barrier();
}

// Called with MMU off. Extend the existing TTBR0 identity map only over GOP.
int early_boot_progress_map(uint64_t *l1, uintptr_t free_page, uintptr_t end)
{
    const uint64_t block = 1ULL << 21, table_mask = 0x0000fffffffff000ULL;
    if (!framebuffer) return 1;
    uint64_t first = framebuffer_pa & ~(block - 1);
    uint64_t limit = (framebuffer_pa + (uint64_t)stride * 4 * height + block - 1) & ~(block - 1);
    // The kernel selects a full or half L1 table; never alias addresses beyond it.
    if (limit > (1ULL << (64 - T0SZ_BOOT)) || (free_page & 4095) || (end & 4095)) return 0;
    for (uint64_t pa = first; pa < limit; pa += block) {
        uint64_t *slot = &l1[(pa >> 30) & 511];
        if (*slot == 0) {
            if (free_page > end || end - free_page < 4096) return 0;
            volatile uint64_t *page = (volatile uint64_t *)free_page;
            for (unsigned i = 0; i < 512; i++) page[i] = 0;
            *slot = free_page | 3;
            free_page += 4096;
        }
        if ((*slot & 3) != 3) return 0;
        uint64_t *l2 = (uint64_t *)(uintptr_t)(*slot & table_mask);
        // Attr1 = normal non-cacheable in XNU's MAIR, outer-shareable, AF, EL1 RW, XN.
        uint64_t mapping = pa | (1ULL << 54) | (1ULL << 53) | 0x605;
        if (l2[(pa >> 21) & 511] != 0 && l2[(pa >> 21) & 511] != mapping) return 0;
        l2[(pa >> 21) & 511] = mapping;
    }
    barrier();
    early_boot_stage = 2;
    boot_progress_stage("S2: bootstrap maps ready");
    return 1;
}

void early_boot_progress_mmu_on(void) { early_boot_stage = 3; boot_progress_stage("S3: MMU enabled"); }
void early_boot_progress_arm_init(void) { early_boot_stage = 4; boot_progress_stage("S4: arm_init entered"); }
void early_boot_progress_suspend(void) { early_boot_stage = 0x50; barrier(); framebuffer = 0; }

void early_boot_progress_vm_ready(void)
{
    uintptr_t mapped = framebuffer_pa ? ml_io_map_wcomb(framebuffer_pa, (uint64_t)stride * 4 * height) : 0;
    if (mapped) framebuffer = mapped;
    early_boot_progress_vectors_done();
    console_smp = 1;
    early_boot_stage = 6;
    boot_progress_stage("S6: kernel VM and exception vectors ready");
}

void boot_progress_init(void)
{
    early_boot_stage = 5;
    boot_progress_stage("S5: PE_init_platform ready");
    value_raw("kernel timebase Hz", gPEClockFrequencyInfo.timebase_frequency_hz);
    barrier();
}

// The assembly fatal path re-enters with physical addresses and MMU/cache off.
void early_boot_progress_fatal(boot_args *args, uint64_t esr, uint64_t elr, uint64_t far, uint64_t spsr, uint64_t stage)
{
    early_boot_progress_init(args);
    if (framebuffer) panic_open("AYN Odin 3 / EARLY EXCEPTION");
    value_raw("last checkpoint", stage);
    value_raw("ESR_EL1", esr); value_raw("ELR_EL1", elr);
    value_raw("FAR_EL1", far); value_raw("SPSR_EL1", spsr);
    if (framebuffer) panic_footer("HALTED: no software reboot");
    panic_halt();
}

void boot_progress_kernel_exception(uint64_t esr, uint64_t elr, uint64_t far)
{
    if (panic_other_cpu()) { panic_halt(); return; }
    uint64_t flags = panic_state ? irq_disable() : irq_save();
    if (flags == UINT64_MAX) return;
    if (panic_state) {
        panic_limit = height / LINE;
        row = panic_limit - 2; column = 0; clear_line(row * LINE); clear_line((row + 1) * LINE);
        text_raw("NESTED EXCEPTION ESR "); hex_raw(esr); putc_raw('\n');
        text_raw("ELR "); hex_raw(elr); text_raw(" FAR "); hex_raw(far);
        panic_halt(); return;
    }
    stack_note(&stack_exc_min);
    fault_valid = 1; fault_esr = esr; fault_pc = elr; fault_far = far;
#ifndef BOOT_PROGRESS_TEST
    __asm__ volatile("mrs %0, spsr_el1" : "=r"(fault_psr));
#endif
    record_stage("kernel exception");
    text_raw("[XNU] kernel exception\n");
    value_raw("ESR_EL1", esr); value_raw("ELR_EL1", elr); value_raw("FAR_EL1", far);
    barrier(); irq_restore(flags);
}

#include "UserlandObserver.h"

// One card-log line with what the page shows plus what it cannot show once the system owns the display. Reads shared state
// without the console lock (counters only; a torn read costs one wrong digit) and runs outside it: the SD flush takes the
// transport lock and writes 32 KiB, which must never happen with interrupts masked.
static void userland_snapshot(void)
{
    if (!&native_sd_log_note || !&native_sd_log_flush) return;
    // Static, not on the stack: -ftrivial-auto-var-init=zero turns a local 320-byte array into a memset, and
    // build-boot-progress.py rejects an early console that depends on a compiler runtime helper. Only the poll
    // thread runs this.
    static char line[448];
    const struct odin_exit_record *last = odin_exit_count ? &odin_exits[(odin_exit_count - 1) % ODIN_EXITS] : 0;
    unsigned long long rd = &native_sd_read_bytes ? (unsigned long long)(native_sd_read_bytes >> 20) : 0;
    unsigned long long wr = &native_sd_write_bytes ? (unsigned long long)(native_sd_write_bytes >> 10) : 0;
    // Where the time goes (change boot-time-review): microseconds the SD transport was busy reading and writing, read requests, the
    // bytes/files/ticks of the 4 KiB code-signature pass, and the mean CPU busy share since the previous line. All counters only
    // ever grow, so the difference of two lines is the interval; rdms/t is the transport duty cycle.
    const uint64_t hz = gPEClockFrequencyInfo.timebase_frequency_hz, per_ms = hz >= 1000 ? hz / 1000 : 0;
    unsigned long long rdn = &native_ufs_block_reads ? (unsigned long long)native_ufs_block_reads : 0;
    unsigned long long rdms = &native_sd_read_us ? (unsigned long long)(native_sd_read_us / 1000) : 0;
    unsigned long long wrms = &native_sd_write_us ? (unsigned long long)(native_sd_write_us / 1000) : 0;
    unsigned long long csmib = &cs_native4k_bytes ? (unsigned long long)(cs_native4k_bytes >> 20) : 0;
    unsigned long long csn = &cs_native4k_ready_blobs ? (unsigned long long)cs_native4k_ready_blobs : 0;
    unsigned long long csrd = (&cs_native4k_read_ticks && per_ms) ? (unsigned long long)(cs_native4k_read_ticks / per_ms) : 0;
    unsigned long long cshash = (&cs_native4k_hash_ticks && per_ms) ? (unsigned long long)(cs_native4k_hash_ticks / per_ms) : 0;
    unsigned long long dlmib = &native_data_persist_loaded ? (unsigned long long)(native_data_persist_loaded >> 8) : 0;
    unsigned busy_p = ul_busy_samples ? (unsigned)(ul_busy_sum[1] / ul_busy_samples) : 0;
    unsigned busy_e = ul_busy_samples ? (unsigned)(ul_busy_sum[0] / ul_busy_samples) : 0;
    ul_busy_sum[0] = ul_busy_sum[1] = 0; ul_busy_samples = 0;
    snprintf(line, sizeof(line),
        "UL t=%llu main=%u procs=%u jobs=%02x/%02x svc1=%llu own=%d logo=%d frames=%llu "
        "sb=%u(%d:%x:%u:%llx) bb=%u(%d:%x:%u:%llx) last=%.16s:%d:%x exits=%u ab=%llu trap=%u aks=%llx/%u "
        "disp=%.28s:%llx free=%u avail=%u rd=%lluMiB wr=%lluKiB "
        "rdn=%llu rdms=%llu wrms=%llu dl=%llu cs4k=%llu/%llu/%llu/%llu busy=%u/%u",
        (unsigned long long)userland_log_ticks, odin_main_ack, odin_processes, odin_jobs_seen, odin_process_mask,
        (unsigned long long)odin_svc_count, display_owned, logo_visible, (unsigned long long)system_frames,
        odin_sb_exits, odin_sb_last.pid, (unsigned)odin_sb_last.status, odin_sb_last.ns, (unsigned long long)odin_sb_last.code,
        odin_bb_exits, odin_bb_last.pid, (unsigned)odin_bb_last.status, odin_bb_last.ns, (unsigned long long)odin_bb_last.code,
        last ? last->name : "-", last ? last->pid : 0, last ? (unsigned)last->status : 0, odin_exit_count,
        (unsigned long long)odin_fault_count, odin_trap_count, (unsigned long long)aks_fail_mask, aks_fail_count,
        display_stage, (unsigned long long)display_value, vm_page_free_count, memorystatus_available_pages, rd, wr,
        rdn, rdms, wrms, dlmib, csmib, csn, csrd, cshash, busy_p, busy_e);
    native_sd_log_note(line);
    // Who aborted (change fairplayd-off: T showed ab=12 and the UL line could not say which processes). One UF line per abort not yet
    // written, oldest first, at most 8 per period; what the ring has already overwritten or the cap leaves out is counted, never silent.
    const uint64_t faults = odin_fault_count;
    uint64_t first = ul_faults_reported, skipped = 0;
    if (faults > first && faults - first > ODIN_FAULTS) { skipped = faults - first - ODIN_FAULTS; first = faults - ODIN_FAULTS; }
    if (faults > first && faults - first > 8) { skipped += faults - first - 8; first = faults - 8; }
    if (skipped) {
        snprintf(line, sizeof(line), "UF t=%llu skipped=%llu", (unsigned long long)userland_log_ticks, (unsigned long long)skipped);
        native_sd_log_note(line);
    }
    for (uint64_t n = first; n < faults; ++n) {
        const struct odin_fault_record *f = &odin_faults[n % ODIN_FAULTS];
        snprintf(line, sizeof(line), "UF t=%llu n=%llu %.16s:%d esr=%x pc=%llx far=%llx fo=%llx",
            (unsigned long long)userland_log_ticks, (unsigned long long)(n + 1), f->name, f->pid, f->esr,
            (unsigned long long)f->pc, (unsigned long long)f->far, (unsigned long long)(f->mapoff + f->pc - f->mapstart));
        native_sd_log_note(line);
    }
    ul_faults_reported = faults;
    native_sd_log_flush();
}

static void poll(void *unused0, void *unused1)
{
    (void)unused0; (void)unused1;
    if (panic_state) return;
    // Recount is sampled outside the framebuffer/IRQ lock.
    uint64_t domain_time[2][2];unsigned domain_cpus[2],percent[2];
    int domain_valid=odin_cpu_domain_usage(domain_time,domain_cpus);
    int domain_ready=odin_domain_percent(&domain_previous,domain_time,domain_valid,percent);
    if(domain_ready){ul_busy_sum[0]+=percent[0];ul_busy_sum[1]+=percent[1];++ul_busy_samples;}
    // The process lists are initialized by the time root preparation starts.
    // Never acquire a process lock while holding the framebuffer/IRQ lock.
    if (&native_owner_cache_stage && native_owner_cache_stage) {
        unsigned mask = 0;
        odin_boot_snapshot(&odin_processes, &mask);
        odin_process_mask = mask;
        odin_jobs_seen |= mask;
    }
    uint64_t flags = irq_save();
    if (flags == UINT64_MAX) goto schedule;
    if (framebuffer && !busy) {
        busy = 1;
        ++poll_count;
        if (display_owned && diag_resume_seconds && poll_count - last_system_frame_tick >= diag_resume_seconds) {
            display_owned = 0; logo_visible = 0;
            for (unsigned y = 0; y + LINE <= height; y += LINE) clear_line(y);
            row = LOG_TOP; column = 0;
            text_raw("No scanout for resume interval; diagnostic page reclaimed\n");
            unsigned count = stage_count < HISTORY ? stage_count : HISTORY;
            for (unsigned i = stage_count - count; i < stage_count; i++) {
                text_raw(stages[i % HISTORY]); putc_raw('\n');
            }
        }
        if (logo_visible && !display_owned && poll_count >= 30) {
            logo_visible = 0;
            for (unsigned y = 0; y + LINE <= height; y += LINE) clear_line(y);
            row = LOG_TOP; column = 0;
            text_raw("No system frame after 30s; boot continues\n");
            unsigned count = stage_count < HISTORY ? stage_count : HISTORY;
            for (unsigned i = stage_count - count; i < stage_count; i++) {
                text_raw(stages[i % HISTORY]); putc_raw('\n');
            }
            text_raw(root_stage); putc_raw('\n');
            text_raw(root_detail); putc_raw('\n'); value_raw("root value", root_value);
        }
        unsigned saved_row = row, saved_column = column;
        row = 1; column = 0; clear_line(LINE);
        snprintf(timer_status, sizeof(timer_status), "poll %016llx timer %016llx",
                 (unsigned long long)poll_count, (unsigned long long)armcpu_timer_interrupts);
        line_raw(timer_status);
        row = 3; column = 0; clear_line(3 * LINE);
        {
            unsigned fail = 0;
            for (unsigned i = 0; i < 8; ++i) if (armcpu_smp_fail[i]) fail |= 1U << i;
            snprintf(cpu_status, sizeof(cpu_status), "CPU smp=%u want=%u conduit=%u fail=%02x RAM=%luMiB",
                     (unsigned)armcpu_smp_running, (unsigned)armcpu_smp_requested,
                     (unsigned)armcpu_smp_conduit, fail, gPhysSize >> 20);
            line_raw(cpu_status);
        }
        row = 2; column = 0; clear_line(2 * LINE);
        if (&native_owner_cache_stage && native_owner_cache_stage &&
            &native_ufs_block_reads && &native_ufs_block_failures) {
            // "SD=" counts requests (the old "UFS=" label); MiB/busy come from the byte and microsecond counters.
            unsigned long long sd_mib = &native_sd_read_bytes ? (unsigned long long)(native_sd_read_bytes >> 20) : 0;
            unsigned long long sd_ms = &native_sd_read_us ? (unsigned long long)(native_sd_read_us / 1000) : 0;
            unsigned long long sd_wkib = &native_sd_write_bytes ? (unsigned long long)(native_sd_write_bytes >> 10) : 0;
            unsigned long long sd_wms = &native_sd_write_us ? (unsigned long long)(native_sd_write_us / 1000) : 0;
            snprintf(storage_status, sizeof(storage_status), "free=%u avail=%u SD=%llu err=%llu MiB=%llu ms=%llu W=%lluKiB/%llums",
                     vm_page_free_count, memorystatus_available_pages,
                     (unsigned long long)native_ufs_block_reads, (unsigned long long)native_ufs_block_failures,
                     sd_mib, sd_ms, sd_wkib, sd_wms);
            line_raw(storage_status);
        } else {
            snprintf(storage_status, sizeof(storage_status), "DT/CPU/GIC %u/%u/%u PPI-enable %016llx",
                     !!armdt_platform_configured, !!armcpu_registered, !!armcpu_gic_ready,
                     (unsigned long long)armcpu_gic_boot[1]);
            line_raw(storage_status);
        }
        row = 4; column = 0; clear_line(4 * LINE);
        snprintf(display_status, sizeof(display_status), "DISPLAY %.48s %llx", display_stage,
                 (unsigned long long)display_value);
        line_raw(display_status);
        row = 5; column = 0; clear_line(5 * LINE); line_raw(geometry_status[0]?geometry_status:exit_stage);
        odin_observer_render();
        if(domain_ready)snprintf(domain_status,sizeof domain_status,"E:%u %u%% busy P:%u %u%% busy (accounting, not MHz/UI affinity)",
            domain_cpus[0],percent[0],domain_cpus[1],percent[1]);
        else snprintf(domain_status,sizeof domain_status,"E/P pending E:%u P:%u (need two valid samples)",domain_cpus[0],domain_cpus[1]);
        row=8;column=0;clear_line(8*LINE);
        // Identity-only touch must be photographable even before the first UI
        // swap (otherwise waiting for the cache hash would hide the result).
        line_raw(peripheral_status[PAGANI_TOUCH][0]?peripheral_status[PAGANI_TOUCH]:domain_status);
        if (gpu_survey_count && !logo_visible && !display_owned) {
            for (unsigned i = 0; i < gpu_survey_count; ++i) {
                row = LOG_TOP + i; column = 0; clear_line(row * LINE); line_raw(gpu_survey_rows[i]);
            }
        }
        row = saved_row; column = saved_column; barrier(); busy = 0;
    }
    irq_restore(flags);
    boot_progress_peripheral_frame();
schedule:;
    if (userland_log_period && ++userland_log_ticks % userland_log_period == 0) userland_snapshot();
    uint64_t deadline;
    clock_interval_to_deadline(1, NSEC_PER_SEC, &deadline);
    thread_call_enter_delayed(poll_call, deadline);
}

void boot_progress_start(void)
{
#ifndef BOOT_PROGRESS_TEST
    PE_parse_boot_argn("boot-diag-resume", &diag_resume_seconds, sizeof(diag_resume_seconds));
    PE_parse_boot_argn("odin-userland-log", &userland_log_period, sizeof(userland_log_period));
#endif
    if (diag_resume_seconds > 300) diag_resume_seconds = 300;
    if (userland_log_period && userland_log_period < 5) userland_log_period = 5;    // each line is a 32 KiB card write
    if (userland_log_period > 3600) userland_log_period = 3600;
    poll_call = thread_call_allocate_with_options(poll, NULL, THREAD_CALL_PRIORITY_KERNEL, THREAD_CALL_OPTIONS_ONCE);
    if (!poll_call) { boot_progress_stage("timer observer allocation failed"); return; }
    boot_progress_stage("timer observer scheduled; counters are hexadecimal");
    thread_call_enter(poll_call);
}

// Process observations retain identity so repeated execs and restart loops differ.
void boot_progress_irq(uint64_t pc, uint64_t psr, uint64_t sp)
{
    // IRQ observations have no process identity: never attribute this PC to
    // launchd. EL0 distinguishes user execution from a kernel wait/spin.
    if ((psr & 15) == 0) { odin_irq_user = pc; odin_irq_sp = sp; }
    else odin_irq_kernel = pc;
}
void boot_progress_poll(void) {}
void boot_progress_user_breakpoint(void *state, unsigned esr)
{
    (void)state;
    uint64_t flags = irq_save();
    if (flags == UINT64_MAX) return;
    struct odin_trap_record *t = &odin_traps[odin_trap_count++ % ODIN_TRAPS];
    t->pc = get_saved_state_pc((arm_saved_state_t *)state);
    t->esr = esr;
    irq_restore(flags);
}
void boot_progress_user_abort(void *state, unsigned esr, uint64_t far) { (void)state; (void)esr; (void)far; }
void boot_progress_critical_exit(int pid, char *s, unsigned long n) { (void)pid; (void)s; (void)n; }
void boot_progress_process_stage(int pid, const char *name, const char *stage)
{
    char line[STAGE_BYTES];
    snprintf(line, sizeof(line), "pid=%d %.20s %.36s", pid, name ? name : "?", stage ? stage : "?");
    boot_progress_stage(line);
}
void boot_progress_exit(int pid, const char *name, int status, unsigned ns, uint64_t code)
{
    odin_boot_exit_record(pid, name, status, ns, code);
    char line[STAGE_BYTES];
    snprintf(line, sizeof(line), "exit pid=%d %.16s status=%x ns=%u code=%llx", pid,
             name ? name : "?", status, ns, (unsigned long long)code);
    uint64_t flags = irq_save();
    if (flags == UINT64_MAX) return;
    if (!panic_state && !busy) copy_line(exit_stage, sizeof(exit_stage), line);
    irq_restore(flags);
    boot_progress_stage(line);
}
