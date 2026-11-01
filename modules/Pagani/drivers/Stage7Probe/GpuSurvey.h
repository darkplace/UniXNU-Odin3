/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/*
 * G1 of the Adreno 830 bring-up (docs/builds/odin3-adreno830-g1-20261029.md): a READ-ONLY survey of what the firmware left on.
 *
 * Boot-arg odin-gpu-survey=1 (off by default). The survey never writes a register, never enables a clock or a power domain, never
 * touches the GX domain, the GMU, the CP, the GPU SMMU or any secure-world state: its only non-memory effects are 32-bit loads from
 * GCC / GPUCC and SCM *availability queries* (SMC fast calls that ask the secure monitor whether a function exists).
 *
 * Every access is announced through io->before() BEFORE it happens, so that the page (and the stage history that a panic prints) names
 * the access that was in flight if the bus faults. The accesses form a ladder; a rung is only climbed when the evidence below it says
 * the block behind it is clocked and powered:
 *
 *   SCM   availability of the calls the GPU path will need (no GPU block involved)
 *   GCC   gpu_cfg_ahb / qmip_gpu_ahb / gemnoc_gfx branch status (the always-on clock controller)
 *   gate A  GCC_GPU_CFG_AHB_CLK running (CLK_OFF clear)  -> GPUCC registers may be read
 *   GPUCC CX GDSC, critical and GMU branch status
 *   gate B  diagnostic only: CX looks on, software-controlled, and its three critical branches running.
 *           NO CX_MISC access in G1: transient status is not ownership. Fuse/slices/chip ID belong to G2.
 *
 * Header-only and freestanding so the host test (Test/gpu-survey.c) drives the real ladder against a model of the hardware.
 * Offsets: GCC and GPUCC from gcc-sm8750.c / gpucc-sm8750.c, CX_MISC from a6xx.xml (A8XX variant); the CX_MISC absolute addresses
 * equal the ones q1n1 measured on the X2 Elite (0x03d9f014, 0x03d9e0e4, 0x03d9e21c). All pinned in a830-inputs.json.
 */
#ifndef ODIN_GPU_SURVEY_H
#define ODIN_GPU_SURVEY_H
#include <stdint.h>

#define GS_GCC_BASE          0x00100000ULL
#define GS_GPUCC_BASE        0x03d90000ULL
#define GS_CX_MISC_BASE      0x03d9e000ULL

#define GS_GCC_GPU_CFG_AHB   (GS_GCC_BASE + 0x71004)   /* gcc_gpu_cfg_ahb_clk: Linux keeps it always on (gcc-sm8750.c:3234) */
#define GS_GCC_QMIP_GPU_AHB  (GS_GCC_BASE + 0x71008)   /* gcc_qmip_gpu_ahb_clk */
#define GS_GCC_GPU_GEMNOC    (GS_GCC_BASE + 0x71010)   /* gcc_gpu_gemnoc_gfx_clk */

#define GS_GPUCC_RSCC_XO_AON (GS_GPUCC_BASE + 0x9004)  /* the three clocks gpucc-sm8750.c marks always-on (0x9004, 0x9008, 0x93a4) */
#define GS_GPUCC_CXO_AON     (GS_GPUCC_BASE + 0x9008)
#define GS_GPUCC_CB          (GS_GPUCC_BASE + 0x93a4)
#define GS_GPUCC_CX_GDSCR    (GS_GPUCC_BASE + 0x9080)
#define GS_GPUCC_AHB         (GS_GPUCC_BASE + 0x90bc)
#define GS_GPUCC_CX_GMU      (GS_GPUCC_BASE + 0x90d4)
#define GS_GPUCC_GMU_RCG_CMD (GS_GPUCC_BASE + 0x9318)
#define GS_GPUCC_HLOS1_SMMU  (GS_GPUCC_BASE + 0x7000)

#define GS_CX_MISC_TCM_RET_CNTL        (GS_CX_MISC_BASE + 0x039 * 4)
#define GS_CX_MISC_SLICE_ENABLE_FINAL  (GS_CX_MISC_BASE + 0x087 * 4)
#define GS_CX_MISC_FREQ_LIMIT_STATUS   (GS_CX_MISC_BASE + 0x405 * 4)

#define GS_CBCR_CLK_OFF   (1u << 31)
#define GS_GDSCR_PWR_ON   (1u << 31)
#define GS_GDSCR_HW_CTRL  (1u << 1)
#define GS_GDSCR_SW_COLLAPSE (1u << 0)

/* SCM: fast call, SMC64, owner SIP: ((1<<31)|(1<<30)|(2<<24)) | (svc<<8) | cmd (qcom_scm-smc.c __scm_smc_call). */
#define GS_SCM_FNID(svc, cmd)   (0xC2000000u | ((uint32_t)(svc) << 8) | (uint32_t)(cmd))
#define GS_SCM_SVC_PIL    0x02
#define GS_SCM_SVC_INFO   0x06
#define GS_SCM_SVC_GPU    0x28
#define GS_SCM_PAS_INIT_IMAGE      0x01
#define GS_SCM_PAS_MEM_SETUP       0x02
#define GS_SCM_PAS_AUTH_AND_RESET  0x05
#define GS_SCM_PAS_IS_SUPPORTED    0x07
#define GS_SCM_INFO_IS_CALL_AVAIL  0x01
#define GS_SCM_GPU_INIT_REGS       0x01
#define GS_GPU_PAS_ID     13   /* GPU_PAS_ID, a6xx_gpu.c:17 and a8xx_gpu.c:17 */

typedef enum { GS_KIND_SCM, GS_KIND_MMIO } gs_kind;
typedef enum { GS_OK = 0, GS_SKIPPED_GATE, GS_UNREADABLE, GS_NO_CONDUIT } gs_status;

typedef struct {
    const char *name;
    gs_kind kind;
    uint64_t address;     /* MMIO: physical address; SCM: the function id queried */
    uint64_t value;       /* MMIO: the 32-bit value; SCM: x1 of the reply (1 = available) */
    uint64_t status;      /* SCM: x0 of the reply (0 = success; -1 = SMCCC "not supported") */
    gs_status result;
} gs_row;

typedef struct {
    gs_row rows[24];
    unsigned count;
    int gate_a, gate_b;   /* -1 = not evaluated, 0 = closed, 1 = open */
    unsigned accesses;    /* callback attempts (not proof a bus transaction completed) */
    int cancelled;        /* a before callback refused; no further access permitted */
} gs_result;

typedef struct {
    void *cookie;
    /* Load one 32-bit register. Returns 0 on success. Must not write. */
    int (*read32)(void *cookie, uint64_t physical, uint32_t *value);
    /* SMC fast call. fn/arginfo/arg0 -> x0..x3 of the reply. Returns 0 if the call was issued, nonzero when there is no usable conduit. */
    int (*scm)(void *cookie, uint32_t fn, uint64_t arginfo, uint64_t arg0, uint64_t reply[4]);
    /* Must durably draw BEFORE EVERY access. Nonzero confirms it; zero cancels the whole survey. */
    int (*before)(void *cookie, unsigned index, const char *what, uint64_t address);
    /* Reported after every row, including skipped ones. */
    void (*after)(void *cookie, unsigned index, const gs_row *row);
} gs_io;

static inline gs_row *gs_add(gs_result *r, const char *name, gs_kind kind, uint64_t address)
{
    gs_row *row = &r->rows[r->count++];
    row->name = name; row->kind = kind; row->address = address; row->value = 0; row->status = 0; row->result = GS_OK;
    return row;
}

static inline void gs_scm_query(const gs_io *io, gs_result *r, const char *name, uint32_t fn, uint64_t arginfo, uint64_t arg0, uint64_t shown_fn)
{
    if (r->cancelled) return;
    unsigned index = r->count;
    gs_row *row = gs_add(r, name, GS_KIND_SCM, shown_fn);
    if (r->cancelled || !io->before(io->cookie, index, name, shown_fn)) { r->cancelled = 1; return; }
    uint64_t reply[4] = {0, 0, 0, 0};
    ++r->accesses;
    if (!io->scm || io->scm(io->cookie, fn, arginfo, arg0, reply)) row->result = GS_NO_CONDUIT;
    else { row->status = reply[0]; row->value = reply[1]; }
    io->after(io->cookie, index, row);
}

static inline int gs_read(const gs_io *io, gs_result *r, const char *name, uint64_t address, uint32_t *value)
{
    if (r->cancelled) return -1;
    unsigned index = r->count;
    gs_row *row = gs_add(r, name, GS_KIND_MMIO, address);
    if (r->cancelled || !io->before(io->cookie, index, name, address)) { r->cancelled = 1; return -1; }
    ++r->accesses;
    uint32_t v = 0;
    if (io->read32(io->cookie, address, &v)) { row->result = GS_UNREADABLE; io->after(io->cookie, index, row); return -1; }
    row->value = v; *value = v;
    io->after(io->cookie, index, row);
    return 0;
}

static inline void gs_skip(const gs_io *io, gs_result *r, const char *name, uint64_t address)
{
    if (r->cancelled) return;
    unsigned index = r->count;
    gs_row *row = gs_add(r, name, GS_KIND_MMIO, address);
    row->result = GS_SKIPPED_GATE;
    io->after(io->cookie, index, row);
}

/* A clock branch counts as running only when CLK_OFF is clear and the register is not the all-ones of a dead bus. */
static inline int gs_branch_running(uint32_t v) { return v != 0xffffffffu && !(v & GS_CBCR_CLK_OFF); }

static inline void gs_run(const gs_io *io, gs_result *r)
{
    r->count = 0; r->gate_a = -1; r->gate_b = -1; r->accesses = 0; r->cancelled = 0;
    uint32_t v = 0;
    /* ---- SCM availability: asks the secure monitor, touches no GPU block ---- */
    uint64_t arginfo1 = 1; /* QCOM_SCM_ARGS(1): one value argument */
    gs_scm_query(io, r, "scm info/is_call_avail", GS_SCM_FNID(GS_SCM_SVC_INFO, GS_SCM_INFO_IS_CALL_AVAIL), arginfo1,
                 (GS_SCM_FNID(GS_SCM_SVC_INFO, GS_SCM_INFO_IS_CALL_AVAIL) & 0x00ffffffu) | (2u << 24), GS_SCM_FNID(GS_SCM_SVC_INFO, GS_SCM_INFO_IS_CALL_AVAIL));
    static const struct { const char *name; uint32_t svc, cmd; } wanted[] = {
        {"scm avail pas_init_image", GS_SCM_SVC_PIL, GS_SCM_PAS_INIT_IMAGE},
        {"scm avail pas_mem_setup", GS_SCM_SVC_PIL, GS_SCM_PAS_MEM_SETUP},
        {"scm avail pas_auth_and_reset", GS_SCM_SVC_PIL, GS_SCM_PAS_AUTH_AND_RESET},
        {"scm avail gpu_init_regs", GS_SCM_SVC_GPU, GS_SCM_GPU_INIT_REGS},
        {"scm avail pas_is_supported", GS_SCM_SVC_PIL, GS_SCM_PAS_IS_SUPPORTED},
    };
    for (unsigned i = 0; i < sizeof wanted / sizeof wanted[0]; ++i) {
        uint32_t target = GS_SCM_FNID(wanted[i].svc, wanted[i].cmd);
        gs_scm_query(io, r, wanted[i].name, GS_SCM_FNID(GS_SCM_SVC_INFO, GS_SCM_INFO_IS_CALL_AVAIL), arginfo1,
                     (target & 0x00ffffffu) | (2u << 24), target);
    }

    /* ---- GCC: always-on controller ---- */
    uint32_t cfg_ahb = 0;
    int cfg_ok = gs_read(io, r, "gcc gpu_cfg_ahb", GS_GCC_GPU_CFG_AHB, &cfg_ahb) == 0;
    gs_read(io, r, "gcc qmip_gpu_ahb", GS_GCC_QMIP_GPU_AHB, &v);
    gs_read(io, r, "gcc gpu_gemnoc_gfx", GS_GCC_GPU_GEMNOC, &v);

    /* ---- gate A ---- */
    r->gate_a = cfg_ok && gs_branch_running(cfg_ahb);
    if (!r->gate_a) {
        gs_skip(io, r, "gpucc cx_gdscr", GS_GPUCC_CX_GDSCR);
        r->gate_b = 0;
        return;
    }

    /* ---- GPUCC ---- */
    uint32_t gdscr = 0, rscc_xo = 0, cxo = 0, cb = 0;
    int g_ok = gs_read(io, r, "gpucc cx_gdscr", GS_GPUCC_CX_GDSCR, &gdscr) == 0;
    int a_ok = gs_read(io, r, "gpucc rscc_xo_aon", GS_GPUCC_RSCC_XO_AON, &rscc_xo) == 0;
    int b_ok = gs_read(io, r, "gpucc cxo_aon", GS_GPUCC_CXO_AON, &cxo) == 0;
    int c_ok = gs_read(io, r, "gpucc cb", GS_GPUCC_CB, &cb) == 0;
    gs_read(io, r, "gpucc ahb", GS_GPUCC_AHB, &v);
    gs_read(io, r, "gpucc cx_gmu", GS_GPUCC_CX_GMU, &v);
    gs_read(io, r, "gpucc gmu_rcg_cmd", GS_GPUCC_GMU_RCG_CMD, &v);

    /* ---- gate B: CX powered, software-controlled, and its always-on clocks running ---- */
    r->gate_b = g_ok && a_ok && b_ok && c_ok && gdscr != 0xffffffffu && (gdscr & GS_GDSCR_PWR_ON) && !(gdscr & (GS_GDSCR_HW_CTRL | GS_GDSCR_SW_COLLAPSE)) &&
                gs_branch_running(rscc_xo) && gs_branch_running(cxo) && gs_branch_running(cb);
}
#endif
