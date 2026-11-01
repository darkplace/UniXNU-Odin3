// SPDX-License-Identifier: BSD-2-Clause-Patent
// Included by BootProgress.c after its console/IRQ helpers. No allocation,
// user memory access or device I/O. SVC rows are bounded to eight pid-1 threads.
enum { ODIN_THREADS = 8, ODIN_LOG_LINES = 12, ODIN_LOG_BYTES = 100, ODIN_EXITS = 8, ODIN_TRAPS = 8, ODIN_FAULTS = 16 };
static struct {
    uint64_t tid, pc, entered, returned, result, since;
    int call;
    unsigned cpsr, pending;
} odin_threads[ODIN_THREADS];
static uint64_t odin_svc_count, odin_svc_other, odin_shadow_count, odin_fault_count;
static uint64_t odin_fault_pc, odin_fault_far, odin_irq_kernel, odin_irq_user, odin_irq_sp;
static unsigned odin_main_ack, odin_processes, odin_process_mask, odin_jobs_seen, odin_threads_overflow;
static unsigned odin_exit_count;
struct odin_exit_record { char name[17]; int pid; int status; unsigned ns; uint64_t code; };
// Exits of the two processes that put the first frame on screen, with the last exit of each: a crash loop of SpringBoard or
// backboardd shows here even when the 8-entry ring above has been pushed past it. Read by the card-log snapshot.
static unsigned odin_sb_exits, odin_bb_exits;
static struct odin_exit_record odin_sb_last, odin_bb_last;
static struct odin_exit_record odin_exits[ODIN_EXITS];
struct odin_trap_record { uint64_t pc; unsigned esr; };
static struct odin_trap_record odin_traps[ODIN_TRAPS];
static unsigned odin_trap_count;
static unsigned odin_fault_esr;
static int odin_fault_pid;
// Last unhandled EL0 aborts with the process that took them: lr, the start of the mapping that holds the PC and that mapping's pager
// offset (the PC's file offset is mapoff + pc - mapstart for a vnode-backed image).
struct odin_fault_record { char name[17]; int pid; unsigned esr; uint64_t pc, lr, far, mapstart, mapoff; };
static struct odin_fault_record odin_faults[ODIN_FAULTS];
// strcmp without a header dependency (this file declares its own snprintf/vsnprintf): 0 when equal.
static int odin_same_name(const char *a, const char *b)
{
    while (*a && *a == *b) { ++a; ++b; }
    return (unsigned char)*a - (unsigned char)*b;
}
static char odin_log[ODIN_LOG_LINES][ODIN_LOG_BYTES];
static unsigned odin_log_row, odin_log_col;
static char odin_userlog[8][ODIN_LOG_BYTES];
static unsigned odin_userlog_row, odin_userlog_col;

void odin_boot_userlog(unsigned fd, const char *text, unsigned length)
{
    uint64_t flags = irq_save();
    if (flags == UINT64_MAX) return;
    if (fd < 1 || fd > 2 || length > 128) { irq_restore(flags); return; }
    for (unsigned i = 0; i < length; ++i) {
        char c = text[i];
        if (c == '\n' || odin_userlog_col == ODIN_LOG_BYTES - 1) {
            odin_userlog_row = (odin_userlog_row + 1) % 8;
            odin_userlog_col = 0; odin_userlog[odin_userlog_row][0] = 0;
        }
        if (c >= 32 && c < 127) {
            if (!odin_userlog_col) {
                odin_userlog[odin_userlog_row][odin_userlog_col++] = (char)('0' + fd);
                odin_userlog[odin_userlog_row][odin_userlog_col++] = '>';
            }
            odin_userlog[odin_userlog_row][odin_userlog_col++] = c;
            odin_userlog[odin_userlog_row][odin_userlog_col] = 0;
        }
    }
    irq_restore(flags);
}

void odin_boot_svc(int pid, uint64_t tid, int call, uint64_t pc)
{
    uint64_t flags = irq_save();
    if (flags == UINT64_MAX) return;
    if (pid != 1) { ++odin_svc_other; irq_restore(flags); return; }
    int first = odin_svc_count++ == 0;
    unsigned i;
    for (i = 0; i < ODIN_THREADS; ++i) {
        if (!odin_threads[i].tid || odin_threads[i].tid == tid) break;
    }
    if (i < ODIN_THREADS) {
        odin_threads[i].tid = tid; odin_threads[i].call = call;
        odin_threads[i].pc = pc; odin_threads[i].since = poll_count;
        ++odin_threads[i].entered; odin_threads[i].pending = 1;
    } else ++odin_threads_overflow;
    irq_restore(flags);
    if (first) boot_progress_stage("pid=1 first EL0 SVC observed (dyld/userland)");
}

void odin_boot_svc_return(uint64_t tid, uint64_t result, unsigned cpsr)
{
    uint64_t flags = irq_save();
    if (flags == UINT64_MAX) return;
    for (unsigned i = 0; i < ODIN_THREADS; ++i) {
        if (odin_threads[i].tid != tid || !odin_threads[i].pending) continue;
        odin_threads[i].pending = 0; ++odin_threads[i].returned;
        odin_threads[i].result = result; odin_threads[i].cpsr = cpsr;
        break;
    }
    irq_restore(flags);
}

void odin_boot_fault(int pid, const char *name, unsigned esr, uint64_t pc, uint64_t lr, uint64_t far,
                     uint64_t mapstart, uint64_t mapoff)
{
    uint64_t flags = irq_save();
    if (flags == UINT64_MAX) return;
    struct odin_fault_record *f = &odin_faults[odin_fault_count % ODIN_FAULTS];
    unsigned i = 0;
    for (; i + 1 < sizeof(f->name); ++i) {
        char c = name ? name[i] : 0;
        f->name[i] = c;
        if (!c) break;
    }
    f->name[i] = 0;
    f->pid = pid; f->esr = esr; f->pc = pc; f->lr = lr; f->far = far; f->mapstart = mapstart; f->mapoff = mapoff;
    ++odin_fault_count; odin_fault_pid = pid; odin_fault_esr = esr;
    odin_fault_pc = pc; odin_fault_far = far;
    irq_restore(flags);
    boot_progress_process_stage(pid, "user", "unhandled EL0 abort -> exception");
}

void odin_boot_log(char c)
{
    // printk can run with locks held: retain characters only, never draw or
    // call printf recursively. Contention drops evidence instead of waiting.
    uint64_t flags = irq_save();
    if (flags == UINT64_MAX) return;
    if (c == '\n' || odin_log_col == ODIN_LOG_BYTES - 1) {
        odin_log_row = (odin_log_row + 1) % ODIN_LOG_LINES;
        odin_log_col = 0; odin_log[odin_log_row][0] = 0;
    }
    if (c >= 32 && c < 127) {
        odin_log[odin_log_row][odin_log_col++] = c;
        odin_log[odin_log_row][odin_log_col] = 0;
    }
    irq_restore(flags);
}

void odin_boot_shadow(int pid, const char *name)
{
    uint64_t flags = irq_save();
    if (flags == UINT64_MAX) return;
    ++odin_shadow_count;
    irq_restore(flags);
    boot_progress_process_stage(pid, name, "exec shadow retired (not live exit)");
}

void odin_boot_main(unsigned value)
{
    uint64_t flags = irq_save();
    if (flags == UINT64_MAX) return;
    odin_main_ack = value;
    irq_restore(flags);
    boot_progress_stage("launchd main ACK: kern.initproc_spawned");
}

// Last real exits (exec shadows are filtered by the caller), so a service that
// dies once does not vanish behind the 24-stage history.
void odin_boot_exit_record(int pid, const char *name, int status, unsigned ns, uint64_t code)
{
    uint64_t flags = irq_save();
    if (flags == UINT64_MAX) return;
    struct odin_exit_record *entry = &odin_exits[odin_exit_count++ % ODIN_EXITS];
    unsigned i = 0;
    for (; i + 1 < sizeof(entry->name); ++i) {
        char c = name ? name[i] : 0;
        entry->name[i] = c;
        if (!c) break;
    }
    entry->name[i] = 0;
    entry->pid = pid; entry->status = status; entry->ns = ns; entry->code = code;
    if (!odin_same_name(entry->name, "SpringBoard")) { ++odin_sb_exits; odin_sb_last = *entry; }
    else if (!odin_same_name(entry->name, "backboardd")) { ++odin_bb_exits; odin_bb_last = *entry; }
    irq_restore(flags);
}

static void odin_observer_line(unsigned y, const char *line)
{
    row = y; column = 0; clear_line(y * LINE); line_raw(line);
}

static void odin_observer_render(void)
{
    char line[100];
    odin_observer_line(0, "ODIN3 D6 / GOP CPU + userland observer / no UFS / no physical panel control");
    snprintf(line, sizeof(line), "USER main=%u procs=%u jobs=%02x(seen) svc1=%llu other=%llu shadows=%llu",
        odin_main_ack, odin_processes, odin_jobs_seen,
        (unsigned long long)odin_svc_count, (unsigned long long)odin_svc_other,
        (unsigned long long)odin_shadow_count);
    odin_observer_line(6, line);
    snprintf(line, sizeof(line), "IRQ kernel=%016llx EL0=%016llx sp=%016llx",
        (unsigned long long)odin_irq_kernel, (unsigned long long)odin_irq_user,
        (unsigned long long)odin_irq_sp);
    odin_observer_line(7, line);
    odin_observer_line(8, "jobs bits: 1 launchd 2 logd 4 backboardd 8 SpringBoard 10 runningboardd 20 watchdogd 40 UEA");
    // Keep the 24-stage boot history in the middle and reserve the bottom 25
    // rows for the live observer; no scrolling or unbounded strings here.
    if (height / LINE < 60) return;
    unsigned y = height / LINE - 27;
    odin_observer_line(y++, "PID1 SVC: tid / signed call / IN|OUT / ticks / entered-returned / PC / last x0,carry");
    for (unsigned i = 0; i < 2; ++i) {
        if (!odin_threads[i].tid) { odin_observer_line(y++, "-"); continue; }
        snprintf(line, sizeof(line), "%llx %d %s %llu %llu/%llu pc=%llx x0=%llx C=%u",
            (unsigned long long)odin_threads[i].tid, odin_threads[i].call,
            odin_threads[i].pending ? "IN" : "OUT",
            (unsigned long long)(poll_count - odin_threads[i].since),
            (unsigned long long)odin_threads[i].entered,
            (unsigned long long)odin_threads[i].returned,
            (unsigned long long)odin_threads[i].pc,
            (unsigned long long)odin_threads[i].result, !!(odin_threads[i].cpsr & (1U << 29)));
        odin_observer_line(y++, line);
    }
    snprintf(line, sizeof(line), "Unhandled aborts=%llu last pid=%d esr=%08x overflow=%u",
        (unsigned long long)odin_fault_count, odin_fault_pid, odin_fault_esr, odin_threads_overflow);
    odin_observer_line(y++, line);
    // The two latest aborts, two lines each: who (pid, name, ESR), where (PC, LR), what (FAR) and the file offset of the PC.
    for (unsigned n = 0; n < 2; ++n) {
        if (odin_fault_count <= n) { odin_observer_line(y++, "-"); odin_observer_line(y++, "-"); continue; }
        struct odin_fault_record *f = &odin_faults[(odin_fault_count - 1 - n) % ODIN_FAULTS];
        snprintf(line, sizeof(line), "A%u pid=%d %.16s esr=%08x far=%llx", n, f->pid, f->name, f->esr, (unsigned long long)f->far);
        odin_observer_line(y++, line);
        snprintf(line, sizeof(line), "  pc=%llx lr=%llx map=%llx off=%llx fileoff=%llx", (unsigned long long)f->pc,
            (unsigned long long)f->lr, (unsigned long long)f->mapstart, (unsigned long long)f->mapoff,
            (unsigned long long)(f->mapoff + f->pc - f->mapstart));
        odin_observer_line(y++, line);
    }
    odin_observer_line(y++, "KERNEL printf tail (last 3 wrapped lines; lossy):");
    for (unsigned i = ODIN_LOG_LINES - 2; i <= ODIN_LOG_LINES; ++i) {
        odin_observer_line(y++, odin_log[(odin_log_row + i) % ODIN_LOG_LINES]);
    }
    odin_observer_line(y++, "PID1 fd1/fd2 write requests (bounded; not proof of delivery):");
    for (unsigned i = 1; i <= 3; ++i) {
        odin_observer_line(y++, odin_userlog[(odin_userlog_row + i) % 8]);
    }
    odin_observer_line(y++, "LAST REAL EXITS (oldest first; exec shadows excluded):");
    unsigned shown = odin_exit_count < 4 ? odin_exit_count : 4;
    for (unsigned i = odin_exit_count - shown; i < odin_exit_count; ++i) {
        struct odin_exit_record *e = &odin_exits[i % ODIN_EXITS];
        snprintf(line, sizeof(line), "%.16s pid=%d st=%x ns=%u c=%llx",
                 e->name, e->pid, e->status, e->ns, (unsigned long long)e->code);
        odin_observer_line(y++, line);
    }
    odin_observer_line(y++, "LAST USER TRAPS (BRK/SIGTRAP; pid, PC, ESR):");
    unsigned traps = odin_trap_count < 4 ? odin_trap_count : 4;
    for (unsigned i = odin_trap_count - traps; i < odin_trap_count; ++i) {
        struct odin_trap_record *t = &odin_traps[i % ODIN_TRAPS];
        snprintf(line, sizeof(line), "pc=%llx esr=%08x", (unsigned long long)t->pc, t->esr);
        odin_observer_line(y++, line);
    }
    while (y < height / LINE) odin_observer_line(y++, "");
}
