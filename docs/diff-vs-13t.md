# Differences from the OnePlus 13T reference

The closest reference port is the OnePlus 13T (`IssacL04/UniXNU-Pagani_SM8750`, PKX110). Same SoC,
same Pagani platform layer, different device.

## Reused as-is

- The Pagani driver modules (`modules/Pagani`), the platform expert and `ArmCpu`.
- The prelink kit (`kits/NativePrelink`) and the verified-root kit (`kits/PaganiVerifiedRoot`).
- The boot-progress page and the panic page.
- The XNU source patches that are board-independent.

## Changed for this board

| Area | 13T | Odin 3 |
|---|---|---|
| Root | internal UFS | microSD on SDCC2, signed windows verified per 4 KiB block |
| Root device | `NativeUfs` on the UFS node | `OdinSd` on the `sdhci` node, with PIO and ADMA2-64 |
| Data | UFS userdata layout | a dedicated 8 GiB partition with a prefix bitmap store |
| Display | panel driver | GOP handover only; panel, DSI and DPU never programmed |
| Touch | Synaptics S3910 over SPI | FocalTech FT3519 over I2C4 |
| Memory window | 4 GiB (5 GiB trial documented) | 4 GiB, in a high bank; bootstrap budget blocks more |
| Bootloader | vendor ABL with a boot image | OpenABL with a BLS entry on the SD card |
| Kernel arguments | Pagani defaults | GOP-only, software rotation, SD DMA, touch stages, Data persistence |

## What this changed in the code

- New driver: `OdinSd` (host controller, verified windows, Data store). It reuses the platform
  conventions of `NativeUfs` but shares none of its UFS-specific state.
- New board tree: `dts/odin3.dts`, with the SD host, the panel and the touch nodes as *non-exported*
  placeholders until their drivers exist.
- New patch series: the SD root, the display handover and the boot diagnostics.
- `package/PaganiPeripherals.h` stays inherited and is **not** used to enable Odin hardware: it
  describes the 13T's wiring, not this device's.

## Known consequences

- The 13T boots from internal storage; this port cannot, and does not try. The internal UFS and the
  bootloader are never written.
- The 13T has a working panel driver path; this port deliberately avoids one to keep the display
  observable from the first second.
- The 13T's Data layout assumes UFS semantics; this port replaces it with its own store and its own
  failure modes.
