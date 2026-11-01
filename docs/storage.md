# Storage

The SD host controller and the Data volume are two separate things that happen to share a card.

## Host controller

| | |
|---|---|
| Instance | SDCC2, SDHCI v5 at `0x08804000` |
| Bus | 4-bit high-speed at 50 MHz (25 MB/s ceiling); measured 20.5 MiB/s |
| DMA | ADMA2-64, one 64 KiB descriptor per transfer; PIO when `odin-sd-dma=0` |
| Verification | every 4 KiB block checked against the signed policy before delivery |

Known defects:

- **Descriptor stride.** The host-test model walks ADMA descriptors with a 16-byte stride while the
  table is filled every 12 bytes. Invisible with one descriptor, wrong with two or more. The test was
  corrected; the driver's walk is the next fix.
- **UHS is not validated.** `odin-sd-uhs=2` implements the SDR50 protocol (1.8 V, 100 MHz) behind a
  platform hook. It is off in every medium shipped, and it has never run on this unit.
- Mechanisms a vendor driver implements and this one does not — inhibit recovery, CQE, error-path
  recovery, tuning — are the subject of an ongoing audit against `sdhci-msm`.

## Data volume

The Data volume is seeded on first boot and persisted afterwards on `p4` (8 GiB, fixed UUID).

| Block | Contents |
|---|---|
| 0 | `SHNGDATA`, version, page count, seed digest, generation, CRC-32 |
| 1..B | presence bitmap, one bit per 4 KiB page |
| pages | page contents |

- Writes go only into the partition whose GUID matches `odin-data-guid`; anything else is refused.
- A page and its bitmap bit reach the medium before the write completes.
- The mode is opt-in (`sheng-data-persist=2`); without it Data is the seed and is lost on power-off.
- The store never persists the shared-cache hash: that would skip signature validation.
- A working card measured: generation 1, CRC `0x0835b39f`, 120 855 pages present (~472 MiB).

## Boot-time cost

The shared-cache read (~2.9 GiB) takes ~149 s at 20.5 MiB/s. The rest of the ~14 minutes to
SpringBoard is userland: service start-up, signature checks and the persisted-page traffic.
