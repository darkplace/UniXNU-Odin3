# UniXNU-Odin3 (WIP)

AYN Odin 3 device support for [UniXNU](https://unixnu.com/): an iPadOS 17.7.11 (21H461) userland
running on a Snapdragon 8 Elite handheld, booted from a microSD card.

This repository contains **source code and documentation only**. It ships no Apple software and no
bootable images; it documents and automates the process that turns your own restore image into a card
for your own device. See [docs/legal.md](docs/legal.md).

## Identity

| Field | Value |
|---|---|
| Model | `AYN Odin 3` (`ayn,odin3`) |
| SoC | SM8750 (`sun`), Snapdragon 8 Elite |
| chip-id | `0x8011` (iPad Pro HFR family, iPadOS 17.7.11 / 21H461) |
| RAM | 16 GiB in banks; the kernel owns one 4 GiB window |
| Storage | internal UFS (never written) + microSD on SDCC2, SDHCI v5 at `0x08804000` |
| Panel | Chipone ICNA3520 at `0xae94000`, 1080x1920, driven through the firmware GOP |
| Touch | FocalTech FT3519 on I2C4 `0xa90000`, address `0x38`, reset/IRQ GPIO 161/162 |
| Bootloader | OpenABL v1.2 (vendor ABL with a menu and standard Linux boot) |
| Boot medium | microSD: `p1` ESP, `p2` OS, `p3` SystemOS, `p4` UniXNUData |

## Provenance

Every hardware value below came off this unit or from a cited vendor source; nothing is guessed.

| Input | Source |
|---|---|
| SoC, CPU topology, GIC, timer | vendor DTS (`firmware/AYN-linux`, `sm8750.dtsi`), measured boot log |
| RAM size and banks | firmware menu report (`Total DDR Size 0x3E9A5D000`) and the measured kernel page tables |
| SD host, capabilities | driver readout at boot (`CAPABILITIES 0x3639c8b2`, SDHCI `0x7202`) |
| Panel, touch wiring | AYN device tree, eleven vendor DTBs and DTBO entries, compared by SHA-256 |
| Partition map | GPT read from the card, four entries with their LBAs |
| Bootloader behaviour | OpenABL v1.2 release notes and its source (`Core/BootDecision.c`, `Boot/BootManager.c`) |

## Status

Measured on hardware, 2026-10-30.

| Area | Status |
|---|---|
| Build | kernel, package, volumes and ESP image on Linux; two inputs come from macOS |
| Boot | reaches the lock screen from microSD; 8 CPUs alive (PSCI over SMC) |
| Root storage | `OdinSd`: PIO and ADMA2-64, unsigned 20.5 MiB/s, signed windows verified per 4 KiB block |
| Data volume | persistent page store on `p4`; ~472 MiB present, generation 1, CRC valid |
| Display | GOP handover, software rotation; no panel/DSI/DPU programming |
| Touch | identity stages implemented; **touch not demonstrated** |
| GPU / Wi-Fi / audio / brightness | **out of scope** in this tree |
| Boot time | ~14 min to SpringBoard; ~2.5 min of it is the shared-cache read |

Open problems, with their evidence, are in [docs/progress.md](docs/progress.md).

## Layout

```
board.toml                  board configuration, kernel arguments, payload inputs
feeds.toml                  pins the SM8750 SoC feed
tools.toml                  pins the toolchain image and the two auxiliary repositories
configs/odin3_defconfig     kernel configuration
dts/odin3.dts               board root: CPUs, memory, SD host, panel, touch
drivers/                    board view of the drivers (symlinks into modules/)
modules/Pagani              the driver modules themselves
kits/NativePrelink          prelink tool and its XNU patches
kits/PaganiVerifiedRoot     signed-window verification kit
package/PaganiPeripherals.h board constants (inherited; not Odin wiring)
patches/xnu/series          ordered XNU source patches for this board
patches/unixnu/             three patches for the pinned UniXNU tool checkout
scripts/h1/make-target-volumes.sh  p2 and p3 from your IPSW on Linux
scripts/h1/seal-medium.py   SEAL.json: every input that produced a medium
scripts/                    device and host tooling (build, package, write media, tests)
docs/                       hardware, progress, storage, build, testing, legal
```

## Differences from the reference port

The closest reference is the OnePlus 13T (`IssacL04/UniXNU-Pagani_SM8750`, PKX110). Reused as-is:
the Pagani driver set, the platform expert, the prelink kit, the verified-root kit and the boot
progress page. Changed for this board: device tree, memory window, SD root instead of UFS, GOP-only
display, touch controller, and the patch series. Node-by-node comparison in
[docs/diff-vs-13t.md](docs/diff-vs-13t.md).

## Quick start

```sh
docker build -t unixnu-toolchain:v1 -f scripts/toolchain/Dockerfile.toolchain .
git clone https://github.com/Project-Silicium/Mu-Silicium && git -C Mu-Silicium checkout 4cba43c90e02

export UNIXNU_IPSW="$HOME/ipsw/iPad_Pro_HFR_17.7.11_21H461_Restore.ipsw"
export UNIXNU_UEFI="$PWD/Mu-Silicium"
export UNIXNU_TREE="$HOME/UniXNU"
bash scripts/h1/check-host.sh          # says what is still missing, writes nothing
```

Then follow [docs/build.md](docs/build.md): the tree at the pin, kernel, package, payload, the two
volumes, the ESP image, and the card in four ordered steps. `check-host.sh` is the gate before any of
it, and `SEAL.json` records what your medium was built from.

## Prerequisites

- Linux host with Docker (the build runs in the UniXNU toolchain container), `python3`, `git`, `mtools`, `dtc`, `sfdisk`.
- A macOS host for the two inputs that need `hdiutil`: the owner policy manifests and the Data seed.
- Your own copy of the iPadOS 17.7.11 (21H461) restore image; it is not part of this repository.
- A Mu-Silicium checkout, for the firmware package step.
- A microSD card and a USB reader.

## Build

The board is pinned to one UniXNU revision, recorded in [upstream.rev](upstream.rev). Upstream `main`
has moved since: its build recipes are not the ones this kernel needs, so the checkout must be the pin.

```sh
git clone --recurse-submodules https://github.com/UniXNU/UniXNU
cd UniXNU
git checkout "$(cat ../UniXNU-Odin3/upstream.rev)"
for p in ../UniXNU-Odin3/patches/unixnu/*.patch; do git apply "$p"; done

export UNIXNU_IPSW="$HOME/ipsw/iPad_Pro_HFR_17.7.11_21H461_Restore.ipsw"
export UNIXNU_UEFI="$HOME/Mu-Silicium"
export UNIXNU_TREE="$PWD"
./unixnu defconfig odin3_defconfig -D ../UniXNU-Odin3
./unixnu ipsw fetch -D ../UniXNU-Odin3 --ipsw "$UNIXNU_IPSW"
./unixnu fetch
./unixnu status --json
```

The three patches under [patches/unixnu/](patches/unixnu/) carry their reasons; without them the build
stops on clang warnings or links a kernel with no interrupt parents.

Then, in the device repository, inside the toolchain container:

```sh
JOBS=10 bash scripts/h1/build-pagani-kernel.sh build kernel
JOBS=10 bash scripts/h1/build-pagani-kernel.sh build package
JOBS=10 bash scripts/h1/build-pagani-kernel.sh build payload
python3 scripts/build-darwinboot-efi.py
bash scripts/h1/make-target-volumes.sh          # p2 and p3 from your IPSW, on Linux
UNIXNU_TREE="$UNIXNU_TREE" bash scripts/h1/make-boot-medium.sh   # ESP image file plus SEAL.json
pkexec bash scripts/h1/sd-inspect.sh write <image> <sha256>      # the only step that writes hardware
```

The last command rewrites partition 1 only. The full procedure — partitions `p2` and `p3` from your
IPSW, `p4` for the persisted Data, the hashes to compare and the one step that is not automated yet —
is in [docs/build.md](docs/build.md).

## Known unknowns

| Item | Why |
|---|---|
| Why the Edge scheduler blocks the display handover | a kernel built with `patches/xnu/odin3-clusters.patch` reaches userland but never hands the framebuffer to SpringBoard; the patch is kept in the tree but removed from `patches/xnu/series` until the mechanism is understood |
| Touch identity pair for this unit | only one unit measured, and the identity read has not been reproduced |
| Effective DRAM base | the ADT declares `0x80000000`; the kernel's page tables sit near `0x8c883d000` |
| Full UEFI memory map | never read; needed before raising the 4 GiB window |
| UHS (1.8 V) electrical behaviour | protocol path exists, opt-in, never executed on this unit |

## Guides and record

| Guide | Link |
|---|---|
| Porting, all steps | <https://unixnu.com/porting/> |
| Device repository field reference | <https://unixnu.com/porting/device-repository> |
| Device tree | <https://unixnu.com/porting/device-tree> |
| Build and boot | <https://unixnu.com/building> |
| Bring-up sequence | <https://unixnu.com/porting/bring-up> |
| Porting record and completion levels | <https://unixnu.com/porting/records> |

The catalogue entry for this board is generated from `board.toml` and `porting.toml`; until it is
published under <https://unixnu.com/devices/>, the record lives in this repository as
[porting.toml](porting.toml). The upstream record checker rejects one name in it, `PaganiVerifiedRoot`,
because that kit is this repository's and not part of the upstream catalogue; the check therefore runs
on demand (`.github/workflows/porting-record.yml`) instead of on every push, and the mismatch is
written down in that file.

## Licence

BSD-2-Clause-Patent; see [LICENSE](LICENSE). Third-party components keep their own licences; see
[NOTICE.md](NOTICE.md).
