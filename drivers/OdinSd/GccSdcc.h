// SPDX-License-Identifier: BSD-2-Clause-Patent
// GCC (global clock controller) registers of SDCC2, the microSD host.
//
// Facts and encoding come from the public Linux clock driver drivers/clk/qcom/gcc-sm8750.c (v6.14) and
// drivers/clk/qcom/clk-rcg2.c (v6.12). SDCC2 is a root clock generator behind one branch:
//
//   gcc_sdcc2_apps_clk_src  cmd_rcgr 0x1401c, hid_width 5, mnd_width 8, parents BI_TCXO (sel 0) and
//                           GCC_GPLL0_OUT_EVEN (sel 6); the rate table is
//                             400000 Hz   BI_TCXO          pre_div 12 (field 23)  m=1 n=4
//                             25000000 Hz GPLL0_OUT_EVEN   pre_div 12 (field 23)
//                             50000000 Hz GPLL0_OUT_EVEN   pre_div  6 (field 11)
//                             100000000 Hz GPLL0_OUT_EVEN  pre_div  3 (field  5)
//                             202000000 Hz GPLL9_OUT_MAIN  pre_div  4 (field  7)
//   gcc_sdcc2_apps_clk      enable 0x14004 bit 0
//   gcc_sdcc2_ahb_clk       enable 0x14014 bit 0
//   gcc_sdcc2_bcr           reset  0x14000 bit 0
//
// The MSM SDHCI core ignores the controller's own SDCLK divider ("MSM controller does not use internal
// divider and instead directly control the GCC clock", sdhci-msm.c:1762), so this is the only way to
// choose the card clock. The firmware leaves SDCC2 running; nothing here resets the controller.
#ifndef SDHCI_MSM_GCC_H
#define SDHCI_MSM_GCC_H

#include <IOKit/IOLib.h>

#ifdef ODIN_SD_TEST
#define ODIN_SD_BARRIER() __asm__ volatile ("" ::: "memory")
#else
#define ODIN_SD_BARRIER() __asm__ volatile ("dsb sy" ::: "memory")
#endif

#define GCC_BASE                    0x00100000ULL
#define GCC_SDCC2_WINDOW            (GCC_BASE + 0x14000ULL)
#define GCC_SDCC2_WINDOW_SIZE       0x1000ULL

#define GCC_SDCC2_BCR               0x0000
#define GCC_SDCC2_APPS_ENABLE       0x0004
#define GCC_SDCC2_AHB_ENABLE        0x0014
#define GCC_SDCC2_APPS_CMD_RCGR     0x001C
#define GCC_SDCC2_APPS_CFG_RCGR     0x0020
#define GCC_SDCC2_APPS_M            0x0024
#define GCC_SDCC2_APPS_N            0x0028
#define GCC_SDCC2_APPS_D            0x002C

#define GCC_CMD_UPDATE              (1U << 0)
#define GCC_CMD_ROOT_OFF            (1U << 31)
#define GCC_BRANCH_ENABLE           (1U << 0)
#define GCC_BRANCH_HALT             (1U << 31)

#define GCC_CFG_SRC_DIV_MASK        0x1FU
#define GCC_CFG_SRC_SEL_SHIFT       8
#define GCC_CFG_MODE_MASK           0x3000U
#define GCC_CFG_MODE_DUAL_EDGE      0x2000U
#define GCC_CFG_HW_CLK_CTRL         0x100000U

#define GCC_SDCC2_SRC_BI_TCXO       0U
#define GCC_SDCC2_SRC_GPLL0_EVEN    6U

// The two rates this driver uses. Values are the encoded frequency table entries, not measurements.
struct SdccClockPlan {
  unsigned hz;
  uint32_t cfg;
  bool mnd;
  uint32_t m, n, d;
};

static const SdccClockPlan gSdccClock400k  = {   400000, 23U | (GCC_SDCC2_SRC_BI_TCXO << 8) | GCC_CFG_MODE_DUAL_EDGE, true, 1, 4, 4 };
static const SdccClockPlan gSdccClock25m   = { 25000000, 23U | (GCC_SDCC2_SRC_GPLL0_EVEN << 8), false, 0, 0, 0 };
static const SdccClockPlan gSdccClock50m   = { 50000000, 11U | (GCC_SDCC2_SRC_GPLL0_EVEN << 8), false, 0, 0, 0 };
// 100 MHz = GPLL0_OUT_EVEN (300 MHz) / 3, the same parent as 25 and 50 MHz, so no PLL has to be started (gcc-sm8750.c
// ftbl_gcc_sdcc2_apps_clk_src). The 202 MHz row needs GPLL9, which nothing here enables: SDR104 is out of scope.
static const SdccClockPlan gSdccClock100m  = { 100000000, 5U | (GCC_SDCC2_SRC_GPLL0_EVEN << 8), false, 0, 0, 0 };

// Programs the root generator and waits for the update bit to drop. Returns the register read back.
static inline uint32_t gccProgramSdcc2 (volatile uintptr_t gcc, const SdccClockPlan &plan)
{
  uint32_t cmd = *reinterpret_cast<volatile uint32_t *> (gcc + GCC_SDCC2_APPS_CMD_RCGR);
  if (plan.mnd) {
    uint32_t mask = 0xFFU;
    uint32_t dValue = plan.d;
    uint32_t low = plan.m;
    uint32_t high = 2U * (plan.n - plan.m);
    if (dValue < low) dValue = low;
    if (dValue > high) dValue = high;
    *reinterpret_cast<volatile uint32_t *> (gcc + GCC_SDCC2_APPS_M) = plan.m & mask;
    *reinterpret_cast<volatile uint32_t *> (gcc + GCC_SDCC2_APPS_N) = (~(plan.n - plan.m)) & mask;
    *reinterpret_cast<volatile uint32_t *> (gcc + GCC_SDCC2_APPS_D) = (~dValue) & mask;
  }
  uint32_t cfg = *reinterpret_cast<volatile uint32_t *> (gcc + GCC_SDCC2_APPS_CFG_RCGR);
  uint32_t mask = GCC_CFG_SRC_DIV_MASK | (0x7U << GCC_CFG_SRC_SEL_SHIFT) | GCC_CFG_MODE_MASK | GCC_CFG_HW_CLK_CTRL;
  cfg = (cfg & ~mask) | (plan.cfg & mask);
  *reinterpret_cast<volatile uint32_t *> (gcc + GCC_SDCC2_APPS_CFG_RCGR) = cfg;
  ODIN_SD_BARRIER ();
  cmd |= GCC_CMD_UPDATE;
  *reinterpret_cast<volatile uint32_t *> (gcc + GCC_SDCC2_APPS_CMD_RCGR) = cmd;
  ODIN_SD_BARRIER ();
  uint32_t readback = 0;
  for (unsigned spin = 0; spin < 10000; ++spin) {
    readback = *reinterpret_cast<volatile uint32_t *> (gcc + GCC_SDCC2_APPS_CFG_RCGR);
    uint32_t pending = *reinterpret_cast<volatile uint32_t *> (gcc + GCC_SDCC2_APPS_CMD_RCGR);
    if (!(pending & GCC_CMD_UPDATE)) break;
    IODelay (1);
  }
  return readback;
}

static inline uint32_t gccSdcc2Branch (volatile uintptr_t gcc)
{
  return *reinterpret_cast<volatile uint32_t *> (gcc + GCC_SDCC2_APPS_ENABLE);
}

static inline uint32_t gccSdcc2Ahb (volatile uintptr_t gcc)
{
  return *reinterpret_cast<volatile uint32_t *> (gcc + GCC_SDCC2_AHB_ENABLE);
}

static inline uint32_t gccSdcc2Cmd (volatile uintptr_t gcc)
{
  return *reinterpret_cast<volatile uint32_t *> (gcc + GCC_SDCC2_APPS_CMD_RCGR);
}

static inline uint32_t gccSdcc2Cfg (volatile uintptr_t gcc)
{
  return *reinterpret_cast<volatile uint32_t *> (gcc + GCC_SDCC2_APPS_CFG_RCGR);
}

// Best-effort enable of the SDCC2 apps branch. The firmware normally leaves it on; this only restores
// bit 0 when a previous owner gated it, and never touches the reset line.
static inline bool gccEnableSdcc2 (volatile uintptr_t gcc)
{
  uint32_t enable = gccSdcc2Branch (gcc);
  if (!(enable & GCC_BRANCH_ENABLE)) {
    *reinterpret_cast<volatile uint32_t *> (gcc + GCC_SDCC2_APPS_ENABLE) = enable | GCC_BRANCH_ENABLE;
    ODIN_SD_BARRIER ();
  }
  for (unsigned spin = 0; spin < 1000; ++spin) {
    if (!(gccSdcc2Branch (gcc) & GCC_BRANCH_HALT)) return true;
    IODelay (10);
  }
  return false;
}

#endif
