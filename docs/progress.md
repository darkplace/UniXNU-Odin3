# Progress

What works, what does not, and what the evidence is. Dates are the day of the measurement.

## Works

| Milestone | Evidence | Date |
|---|---|---|
| Kernel boots with 8 CPUs | `CPU smp=7 want=1 conduit=1 fail=00` on the diagnostics page | 2026-10-29 |
| Signed OS and SystemOS volumes mount | `VERIFY: … policy accepted`, `PUBLISH: 2 window(s) started` in the card log | 2026-10-29 |
| launchd runs and processes exec | userland log: `main=1`, 200+ processes, exec and signature events | 2026-10-30 |
| SpringBoard reaches the lock screen | observed on the device; userland log shows system frames | 2026-10-30 |
| Data volume persists across boots | `SHNGDATA` header generation 1, CRC valid, 120 855 pages present | 2026-10-30 |
| SD transport with DMA | `DMA: ADMA2-64 selected`, GPT and windows read back correctly | 2026-10-30 |
| 20.5 MiB/s sustained read | 2.9 GiB shared-cache read in ~149 s | 2026-10-30 |

## Does not work

| Problem | Evidence | State |
|---|---|---|
| Touch input | identity stage runs, no contact report on screen | not determined |
| Display handover with the Edge scheduler | kernel with `odin3-clusters.patch` reaches userland but never hands the framebuffer over | mechanism not determined; kernels are built without it |
| Boot time (~14 min) | ~2.5 min card read, ~11 min userland | open; see below |
| GPU, Wi-Fi, audio, brightness | no driver in this tree | out of scope |

## Open threads, with what is known

**Boot time.** The shared-cache hash costs ~149 s at the transport ceiling. The remaining ~11 minutes
are userland: service start-up, signature checks and Data writes. Two respawn loops were observed
(`fairplayd.A2`, `init_exclavekit`); disabling the first removed the loop but not the time, which makes
it a symptom rather than the cause. The next measurements are in [testing.md](testing.md).

**Edge scheduler.** `odin3-clusters.patch` defines `__ARM_AMP__`, which enables XNU's Edge scheduler.
With it, the boot reaches userland and stops before the display handover; without it, the lock screen
appears. The ADT carries no `cluster-type`, so all CPUs register with one scheduling type. Why that
blocks the handover is not determined.

**Touch.** The controller answers identity reads, and the stage stops there on purpose. The next step
needs a measured identity pair for this panel, not a driver rewrite.

**Memory window.** 4 GiB works. 6 GiB sits exactly on the bootstrap page-table budget; 8 GiB exceeds
it and only draws the logo. Any change here starts with a bootstrap patch, then a measured boot.

## How progress is recorded

Each step is a single change with a prediction written before the boot and a result recorded after it,
together with the medium's hash table (kernel, ADT, trust cache, device tree, command line, image).
This repository keeps the hardware facts, the design decisions and the reproducible process; where a
source comment cites a measurement, the citation is kept so the number stays traceable.
