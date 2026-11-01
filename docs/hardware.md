# Hardware — measured on this unit

Everything here was read from this device or from a cited vendor source. Where a value is inferred or
still unknown, it says so.

## SoC and CPUs

| | |
|---|---|
| SoC | Qualcomm SM8750 (`sun`), Snapdragon 8 Elite |
| Cores | 8: six performance (`MPIDR 0x0..0x500`) and two prime (`0x10000`, `0x10100`) |
| Bring-up | PSCI over SMC; boot log reports `smp=7 want=1 conduit=1 fail=00` |
| Timer | 19.2 MHz architectural timer |
| GIC | GICv3, distributor and redistributor (`redistributor-stride 0x40000`) |

## Memory

| | |
|---|---|
| Module | 16 GiB, in banks |
| Firmware report | `Total DDR Size 0x00000003E9A5D000` (~15.65 GiB) |
| Banks (from the device's own DRAM map) | `0x80000000`+2 GiB, `0x880000000`+6 GiB, `0xA00000000`+8 GiB |
| Kernel window | 4 GiB, requested with `/chosen/darwinboot-memory-size` |
| Measured page-table base | `TTBR1_EL1 = 0x8c883d000` in one boot, `0x8c88b3000` in another: the window is high and moves between boots |

The device tree declares a single range at `0x80000000`, and the ADT carries the same numbers. That
value is not what the kernel owns: the boot arguments carry the real window. Raising the window is
limited by the bootstrap page-table budget (16 pages): about 12 pages for 4 GiB, 16 for 6 GiB and 20
for 8 GiB. A kernel built with an 8 GiB window draws the logo and stops.

## Storage

| | |
|---|---|
| Internal | UFS at `0x1d84000`; **never written** by this project |
| External | microSD on SDCC2, SDHCI v5 at `0x08804000` |
| Capabilities | `0x3639c8b2` / `0x1000a003`, SDHCI version `0x7202`, ADMA2 and 64-bit addressing, no SDMA |
| Card in use | 238.4 GB, 512-byte logical sectors |
| Partitions | `p1` UNIXNU (2048+819200), `p2` OS (821248+11870275), `p3` SystemOS (12693504+6451200), `p4` UniXNUData (19144704+16777216) |
| Measured throughput | 20.5 MiB/s at 50 MHz, 4-bit high-speed (86 % of the 25 MB/s ceiling) |

## Display

| | |
|---|---|
| Panel | Chipone ICNA3520 at `0xae94000`, 1080x1920 |
| Framebuffer | GOP handover at `0xfc800000`, pitch 4320 bytes, 32 bpp |
| Access | `odin-gop-only=1`: the panel, DSI, PHY and DPU are never programmed |
| Rotation | software, `sheng-fb-rotate=1` |

## Touch

| | |
|---|---|
| Controller | FocalTech FT3519 |
| Bus | I2C4 at `0xa90000`, address `0x38` |
| Reset / IRQ | GPIO 161 / GPIO 162 |
| Evidence | AYN device tree, eleven vendor DTBs, DTBO entries; the DT `compatible` does not determine the firmware pair |

## Bootloader and boot source

| | |
|---|---|
| Bootloader | OpenABL v1.2, written to both ABL slots |
| Boot method | Boot Loader Specification entry on the card's ESP |
| Device tree | entry-level `devicetree \ODIN3.DTB`, which must carry a `splash_region` |
| Menu settings | kept in the last 4 KiB of the `misc` partition; boot source and mode are user settings, not project state |

## What is not measured

- The complete UEFI memory map (only free runs are inferred from the loader's behaviour).
- The effective `dram-base` the kernel uses for I/O cache attributes.
- The 1.8 V rail behaviour for UHS mode.
- The FT3519 identity pair of this specific unit.
- The power draw and thermal behaviour of a full boot.
