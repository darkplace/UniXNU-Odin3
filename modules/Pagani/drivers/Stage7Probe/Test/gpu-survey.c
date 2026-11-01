// SPDX-License-Identifier: BSD-2-Clause-Patent
// Host test of the G1 read-only ladder (GpuSurvey.h) against a model of the hardware. Build with -fsanitize=address,undefined.
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../GpuSurvey.h"

typedef struct { uint64_t addr; uint32_t value; } cell;
typedef struct {
    cell cells[32]; unsigned ncells;
    uint64_t fault_at;                // a read of this address "faults" (read32 returns -1)
    int conduit;                      // 0 = no SMC conduit
    uint64_t scm_available_mask;      // target function ids answered with "available"
    // trace
    unsigned events; struct { char kind; uint64_t arg; } trace[96];
    int announced;                    // an access was announced and not yet executed
    uint64_t announced_addr;
    unsigned reads, scm_calls, order_errors;
    uint64_t reads_seen[48];
    unsigned refuse_at;              // index + 1, or zero
} model;

static void put(model *m, uint64_t addr, uint32_t value) { m->cells[m->ncells].addr = addr; m->cells[m->ncells++].value = value; }
static int mread(void *c, uint64_t addr, uint32_t *value) {
    model *m = c;
    if (!m->announced || m->announced_addr != addr) ++m->order_errors;   // every access must be announced first, for that address
    m->announced = 0;
    m->reads_seen[m->reads++] = addr;
    m->trace[m->events].kind = 'R'; m->trace[m->events++].arg = addr;
    if (addr == m->fault_at) return -1;
    for (unsigned i = 0; i < m->ncells; ++i) if (m->cells[i].addr == addr) { *value = m->cells[i].value; return 0; }
    *value = 0; return 0;
}
static int mscm(void *c, uint32_t fn, uint64_t arginfo, uint64_t a0, uint64_t reply[4]) {
    model *m = c;
    if (!m->announced) ++m->order_errors;
    m->announced = 0; ++m->scm_calls;
    m->trace[m->events].kind = 'S'; m->trace[m->events++].arg = fn;
    if (!m->conduit) return 1;
    assert((fn & 0xff000000u) == 0xC2000000u);   // fast call, SMC64, owner SIP
    assert(arginfo == 1);                         // one value argument
    if (fn == GS_SCM_FNID(GS_SCM_SVC_INFO, GS_SCM_INFO_IS_CALL_AVAIL)) {
        assert((a0 >> 24) == 2);                  // owner SIP in the queried id
        uint32_t target = 0xC2000000u | (uint32_t)(a0 & 0x00ffffffu);
        reply[0] = 0; reply[1] = (m->scm_available_mask >> 0) & 1 ? ((target == GS_SCM_FNID(GS_SCM_SVC_INFO, 1)) || (target & 0xffff) ? 1 : 0) : 0;
        return 0;
    }
    assert(!"G1 must issue availability queries only");
    return -1;
}
static int mbefore(void *c, unsigned index, const char *what, uint64_t addr) {
    model *m = c; (void)index; (void)what;
    if (m->refuse_at == index + 1) return 0;
    if (m->announced) ++m->order_errors;   // the previous announcement was never followed by its access
    m->announced = 1; m->announced_addr = addr;
    m->trace[m->events].kind = 'B'; m->trace[m->events++].arg = addr;
    return 1;
}
static void mafter(void *c, unsigned index, const gs_row *row) { (void)c; (void)index; (void)row; }

static gs_io io_for(model *m) { gs_io io = {m, mread, mscm, mbefore, mafter}; return io; }
static int was_read(model *m, uint64_t addr) { for (unsigned i = 0; i < m->reads; ++i) if (m->reads_seen[i] == addr) return 1; return 0; }
static gs_result run(model *m) {
    gs_result r; memset(&r, 0, sizeof r);
    gs_io io = io_for(m);
    gs_run(&io, &r);
    assert(m->order_errors == 0);
    assert(!m->announced);
    return r;
}
static void powered(model *m) {
    put(m, GS_GCC_GPU_CFG_AHB, 0x00000001); put(m, GS_GCC_QMIP_GPU_AHB, 0x00000001); put(m, GS_GCC_GPU_GEMNOC, 0x00000001);
    put(m, GS_GPUCC_CX_GDSCR, 0x80000000u); put(m, GS_GPUCC_RSCC_XO_AON, 1); put(m, GS_GPUCC_CXO_AON, 1); put(m, GS_GPUCC_CB, 1);
    put(m, GS_GPUCC_AHB, 1); put(m, GS_GPUCC_CX_GMU, 0x80000000u); put(m, GS_GPUCC_GMU_RCG_CMD, 0);
    put(m, GS_CX_MISC_FREQ_LIMIT_STATUS, 0x184); put(m, GS_CX_MISC_TCM_RET_CNTL, 0); put(m, GS_CX_MISC_SLICE_ENABLE_FINAL, 0x7);
}

int main(void) {
    // Addresses are the ones q1n1 measured on the X2 Elite for the same CX_MISC registers.
    assert(GS_CX_MISC_FREQ_LIMIT_STATUS == 0x03d9f014ULL && GS_CX_MISC_TCM_RET_CNTL == 0x03d9e0e4ULL && GS_CX_MISC_SLICE_ENABLE_FINAL == 0x03d9e21cULL);
    assert(GS_GCC_GPU_CFG_AHB == 0x171004ULL && GS_GPUCC_CX_GDSCR == 0x03d99080ULL);
    assert(GS_SCM_FNID(GS_SCM_SVC_INFO, 1) == 0xC2000601u && GS_SCM_FNID(GS_SCM_SVC_GPU, 1) == 0xC2002801u);

    { // 1. everything off: gate A closed, GPUCC and CX_MISC never read
        model m = {0}; m.conduit = 1; m.scm_available_mask = 1;
        put(&m, GS_GCC_GPU_CFG_AHB, 0x80000000u); put(&m, GS_GCC_QMIP_GPU_AHB, 0x80000000u); put(&m, GS_GCC_GPU_GEMNOC, 0x80000000u);
        gs_result r = run(&m);
        assert(r.gate_a == 0 && r.gate_b == 0 && m.reads == 3 && m.scm_calls == 6 && r.accesses == 9);
        assert(!was_read(&m, GS_GPUCC_CX_GDSCR) && !was_read(&m, GS_CX_MISC_FREQ_LIMIT_STATUS));
        assert(r.rows[r.count - 1].result == GS_SKIPPED_GATE);
    }
    { // 2. AHB running, CX GDSC off: GPUCC read, CX_MISC not
        model m = {0}; m.conduit = 1; powered(&m);
        m.cells[3].value = 0x00000000u; /* CX_GDSCR: PWR_ON clear */
        gs_result r = run(&m);
        assert(r.gate_a == 1 && r.gate_b == 0 && was_read(&m, GS_GPUCC_CX_GDSCR) && was_read(&m, GS_GPUCC_CB));
        assert(!was_read(&m, GS_CX_MISC_FREQ_LIMIT_STATUS) && !was_read(&m, GS_CX_MISC_TCM_RET_CNTL));
    }
    { // 3. everything on: the whole ladder
        model m = {0}; m.conduit = 1; m.scm_available_mask = 1; powered(&m);
        gs_result r = run(&m);
        assert(r.gate_a == 1 && r.gate_b == 1 && m.reads == 3 + 7 && m.scm_calls == 6 && r.accesses == 16);
        assert(!was_read(&m, GS_CX_MISC_FREQ_LIMIT_STATUS));
        assert(r.rows[0].kind == GS_KIND_SCM && r.rows[0].address == 0xC2000601u);
        assert(r.rows[5].address == GS_SCM_FNID(GS_SCM_SVC_PIL, GS_SCM_PAS_IS_SUPPORTED) && r.rows[5].value == 1);
    }
    { // 4. CX GDSC under hardware control: a collapse could race the read, so CX_MISC stays closed
        model m = {0}; m.conduit = 1; powered(&m); m.cells[3].value = 0x80000002u;
        gs_result r = run(&m);
        assert(r.gate_a == 1 && r.gate_b == 0 && !was_read(&m, GS_CX_MISC_FREQ_LIMIT_STATUS));
    }
    for (unsigned off = 4; off <= 6; ++off) { // 5. any of the three always-on GPUCC clocks off closes gate B
        model m = {0}; m.conduit = 1; powered(&m); m.cells[off].value = 0x80000000u;
        gs_result r = run(&m);
        assert(r.gate_a == 1 && r.gate_b == 0 && !was_read(&m, GS_CX_MISC_FREQ_LIMIT_STATUS));
    }
    { // 6. a dead bus (all ones) is not "running"
        model m = {0}; m.conduit = 1; powered(&m); m.cells[0].value = 0xffffffffu;
        gs_result r = run(&m);
        assert(r.gate_a == 0 && !was_read(&m, GS_GPUCC_CX_GDSCR));
    }
    { // 7. no SMC conduit: SCM rows say so and the ladder carries on
        model m = {0}; m.conduit = 0; powered(&m);
        gs_result r = run(&m);
        assert(r.rows[0].result == GS_NO_CONDUIT && r.gate_b == 1);
    }
    { // 8. a faulting GCC read: reported, gate A closed, nothing above it is touched
        model m = {0}; m.conduit = 1; powered(&m); m.fault_at = GS_GCC_GPU_CFG_AHB;
        gs_result r = run(&m);
        assert(r.rows[6].result == GS_UNREADABLE && r.gate_a == 0 && !was_read(&m, GS_GPUCC_CX_GDSCR));
    }
    { // 9. the trace alternates announce / access, and no address outside the ladder is ever read
        model m = {0}; m.conduit = 1; powered(&m);
        run(&m);
        for (unsigned i = 0; i < m.events; i += 2) assert(m.trace[i].kind == 'B' && (m.trace[i + 1].kind == 'R' || m.trace[i + 1].kind == 'S'));
        for (unsigned i = 0; i < m.reads; ++i) {
            uint64_t a = m.reads_seen[i];
            assert((a >= GS_GCC_GPU_CFG_AHB && a <= GS_GCC_GPU_GEMNOC) || (a >= GS_GPUCC_BASE && a < GS_GPUCC_BASE + 0x9800));
        }
    }
    for (unsigned i = 1; i <= 16; ++i) { // No successful checkpoint -> no access, no later callback.
        model m = {0}; m.conduit = 1; powered(&m); m.refuse_at = i;
        gs_result r = run(&m);
        assert(r.cancelled && r.accesses == i - 1);
    }
    { // A collapse request in progress closes diagnostic gate B too.
        model m = {0}; m.conduit = 1; powered(&m); m.cells[3].value = 0x80000001u;
        gs_result r = run(&m); assert(r.gate_b == 0);
    }
    puts("PASS: G1 ladder (gates A and B, dead bus, fault, no conduit, announce-before-access, no stray address)");
    return 0;
}
