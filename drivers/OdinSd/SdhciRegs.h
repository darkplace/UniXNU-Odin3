// SPDX-License-Identifier: BSD-2-Clause-Patent
// SDHCI register map used by SdhciMsm.
//
// Transcribed from the public Linux driver (drivers/mmc/host/sdhci.h, v6.12): every constant below is
// the controller's own register or bit, not a value read from the Odin. The Odin's host is an SDHCI v5
// core ("qcom,sdhci-msm-v5") with the standard register block at 0x08804000 (vendor sm8750.dtsi:2698).
#ifndef SDHCI_MSM_REGS_H
#define SDHCI_MSM_REGS_H

#define SDHCI_BASE                    0x08804000ULL
#define SDHCI_BASE_SIZE               0x1000ULL

#define SDHCI_DMA_ADDRESS             0x00
#define SDHCI_BLOCK_SIZE              0x04
#define SDHCI_BLOCK_COUNT             0x06
#define SDHCI_ARGUMENT                0x08
#define SDHCI_TRANSFER_MODE           0x0C
#define SDHCI_COMMAND                 0x0E
#define SDHCI_RESPONSE                0x10
#define SDHCI_BUFFER                  0x20
#define SDHCI_PRESENT_STATE           0x24
#define SDHCI_HOST_CONTROL            0x28
#define SDHCI_POWER_CONTROL           0x29
#define SDHCI_CLOCK_CONTROL           0x2C
#define SDHCI_TIMEOUT_CONTROL         0x2E
#define SDHCI_SOFTWARE_RESET          0x2F
#define SDHCI_INT_STATUS              0x30
#define SDHCI_ERR_STATUS              0x32
#define SDHCI_INT_ENABLE              0x34
#define SDHCI_ERR_ENABLE              0x36
#define SDHCI_SIGNAL_ENABLE           0x38
#define SDHCI_ERR_SIGNAL_ENABLE       0x3A
#define SDHCI_HOST_CONTROL2           0x3E
#define SDHCI_CAPABILITIES            0x40
#define SDHCI_CAPABILITIES_1          0x44
#define SDHCI_MAX_CURRENT             0x48
#define SDHCI_ADMA_ERROR              0x54
#define SDHCI_SLOT_INT_STATUS         0xFC
#define SDHCI_HOST_VERSION            0xFE

// Qualcomm vendor block (sdhci_msm_v5_offset, Linux sdhci-msm.c). Read only unless a comment says otherwise.
#define SDHCI_MSM_VENDOR_SPEC         0x20C
#define SDHCI_MSM_MCI_VERSION         0x318
#define SDHCI_MSM_MCI_STATUS          0x324
#define SDHCI_MSM_MCI_FIFO_CNT        0x308
#define SDHCI_MSM_MCI_DATA_CNT        0x35C
#define SDHCI_MSM_MCI_GENERICS        0x320
#define SDHCI_MSM_PWRCTL_STATUS       0x240
#define SDHCI_MSM_PWRCTL_MASK         0x244
#define SDHCI_MSM_PWRCTL_CLEAR        0x248
#define SDHCI_MSM_PWRCTL_CTL          0x24C
#define SDHCI_MSM_DEBUG               0x358
#define SDHCI_MSM_VENDOR_SPEC3        0x250

// PWRCTL handshake bits (sdhci-msm.c:40-47). Any change of bus power or of the 1.8 V signalling bit raises a
// status bit that software must clear and acknowledge, or the core does not proceed.
#define SDHCI_MSM_PWRCTL_BUS_OFF      0x1
#define SDHCI_MSM_PWRCTL_BUS_ON       0x2
#define SDHCI_MSM_PWRCTL_IO_LOW       0x4
#define SDHCI_MSM_PWRCTL_IO_HIGH      0x8
#define SDHCI_MSM_PWRCTL_BUS_SUCCESS  0x1
#define SDHCI_MSM_PWRCTL_IO_SUCCESS   0x4
// VENDOR_SPEC fields (sdhci-msm.c:78-85) used to select the default (non-UHS) host clocking.
#define SDHCI_MSM_HC_MCLK_SEL_MASK    (3U << 8)
#define SDHCI_MSM_HC_MCLK_SEL_DFLT    (2U << 8)
#define SDHCI_MSM_HC_SELECT_IN_EN     (1U << 18)
#define SDHCI_MSM_HC_SELECT_IN_MASK   (7U << 19)
#define SDHCI_MSM_PWRSAVE_DLL         (1U << 3)

// Status-enable mask (the bits of SDHCI_INT_STATUS that can latch at all): command complete, transfer complete,
// buffer read/write ready and every error source. sdhci_init() writes the same kind of mask (sdhci.c); the register
// resets to 0 and a status bit that is not enabled never sets, so a polling driver must write it.
#define SDHCI_INT_ENABLE_MASK         (SDHCI_INT_RESPONSE | SDHCI_INT_DATA_END | SDHCI_INT_SPACE_AVAIL | SDHCI_INT_DATA_AVAIL | \
                                       SDHCI_INT_CMD_ERRORS | SDHCI_INT_DATA_ERRORS | 0x01000000U | 0x02000000U)

#define SDHCI_TRNS_DMA                0x0001
#define SDHCI_TRNS_BLK_CNT_EN         0x0002
#define SDHCI_TRNS_AUTO_CMD12         0x0004
#define SDHCI_TRNS_READ               0x0010
#define SDHCI_TRNS_MULTI              0x0020

#define SDHCI_CMD_RESP_NONE           0x00
#define SDHCI_CMD_RESP_LONG           0x01
#define SDHCI_CMD_RESP_SHORT          0x02
#define SDHCI_CMD_RESP_SHORT_BUSY     0x03
#define SDHCI_CMD_CRC                 0x08
#define SDHCI_CMD_INDEX               0x10
#define SDHCI_CMD_DATA                0x20

#define SDHCI_CMD_INHIBIT             0x00000001
#define SDHCI_DATA_INHIBIT            0x00000002
#define SDHCI_DOING_WRITE             0x00000100
#define SDHCI_DOING_READ              0x00000200
#define SDHCI_SPACE_AVAILABLE         0x00000400
#define SDHCI_DATA_AVAILABLE          0x00000800
#define SDHCI_CARD_PRESENT            0x00010000
#define SDHCI_CD_STABLE               0x00020000
#define SDHCI_WRITE_PROTECT           0x00080000
#define SDHCI_CMD_LVL                 0x01000000
#define SDHCI_DATA_LVL_MASK           0x00F00000   // DAT[3:0] pin levels (sdhci.h); all low = card busy / holding the lines

#define SDHCI_CTRL_LED                0x01
#define SDHCI_CTRL_4BITBUS            0x02
#define SDHCI_CTRL_HISPD              0x04
#define SDHCI_CTRL_DMA_MASK           0x18
#define SDHCI_CTRL_SDMA               0x00
#define SDHCI_CTRL_8BITBUS            0x20

#define SDHCI_POWER_ON                0x01
#define SDHCI_POWER_180               0x0A
#define SDHCI_POWER_300               0x0C
#define SDHCI_POWER_330               0x0E

#define SDHCI_CLOCK_CARD_EN           0x0004
#define SDHCI_CLOCK_INT_STABLE        0x0002
#define SDHCI_CLOCK_INT_EN            0x0001

#define SDHCI_RESET_ALL               0x01
#define SDHCI_RESET_CMD               0x02
#define SDHCI_RESET_DATA              0x04

// SDHCI_INT_STATUS is a 32-bit read: normal interrupts in the low half, error interrupts in the high half.
#define SDHCI_INT_RESPONSE            0x00000001
#define SDHCI_INT_DATA_END            0x00000002
#define SDHCI_INT_SPACE_AVAIL         0x00000010
#define SDHCI_INT_DATA_AVAIL          0x00000020
#define SDHCI_INT_CARD_INSERT         0x00000040
#define SDHCI_INT_CARD_REMOVE         0x00000080
#define SDHCI_INT_ERROR               0x00008000
#define SDHCI_INT_TIMEOUT             0x00010000
#define SDHCI_INT_CRC                 0x00020000
#define SDHCI_INT_END_BIT             0x00040000
#define SDHCI_INT_INDEX               0x00080000
#define SDHCI_INT_DATA_TIMEOUT        0x00100000
#define SDHCI_INT_DATA_CRC            0x00200000
#define SDHCI_INT_DATA_END_BIT        0x00400000
#define SDHCI_INT_BUS_POWER           0x00800000
#define SDHCI_INT_ADMA_ERROR          0x02000000

#define SDHCI_INT_CMD_ERRORS          (SDHCI_INT_TIMEOUT | SDHCI_INT_CRC | SDHCI_INT_END_BIT | SDHCI_INT_INDEX)
#define SDHCI_INT_DATA_ERRORS         (SDHCI_INT_DATA_TIMEOUT | SDHCI_INT_DATA_CRC | SDHCI_INT_DATA_END_BIT)
#define SDHCI_INT_ALL_KNOWN           0x0AFBFFFF

#define SDHCI_CTRL_VDD_180            0x0008
#define SDHCI_CTRL_UHS_MASK           0x0007
#define SDHCI_CTRL_UHS_SDR12          0x0000
#define SDHCI_CTRL_UHS_SDR25          0x0001
#define SDHCI_CTRL_UHS_SDR50          0x0002

// Capabilities (sdhci.h @ c339f401): CAPABILITIES bit 26 = 1.8 V; CAPABILITIES_1 bit0 SDR50, bit1 SDR104, bit2 DDR50,
// bit13 'SDR50 needs tuning'.
#define SDHCI_CAN_VDD_180             0x04000000
#define SDHCI_CAN_DO_ADMA2            0x00080000
#define SDHCI_CAN_64BIT               0x10000000
#define SDHCI_ADMA_ADDRESS            0x58
#define SDHCI_ADMA_ADDRESS_HI         0x5C
#define SDHCI_CTRL_ADMA64             0x18
#define SDHCI_SUPPORT_SDR50           0x00000001
#define SDHCI_SUPPORT_SDR104          0x00000002
#define SDHCI_SUPPORT_DDR50           0x00000004
#define SDHCI_USE_SDR50_TUNING        0x00002000

// Qualcomm vendor bits (sdhci-msm.c @ c339f401, sdhci_msm_v5_offset): DLL config, and the I/O pad supply switch that must
// follow the real signalling rail (CORE_IO_PAD_PWR_SWITCH_EN / CORE_IO_PAD_PWR_SWITCH).
#define SDHCI_MSM_DLL_CONFIG          0x200
#define SDHCI_MSM_DLL_RST             (1U << 30)
#define SDHCI_MSM_DLL_PDN             (1U << 29)
#define SDHCI_MSM_IO_PAD_PWR_SWITCH_EN (1U << 15)
#define SDHCI_MSM_IO_PAD_PWR_SWITCH   (1U << 16)
#define SDHCI_MSM_PWRCTL_IO_FAIL      0x8

#endif
