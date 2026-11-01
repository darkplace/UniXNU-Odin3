# Device agent guide

This repository owns the board DTS, peripheral wiring, component selections,
firmware inputs, partition layout, and packaging.

## Read first

Read this README and the following UniXNU documentation before changing the port:

| Task | Required reading |
| --- | --- |
| Every port | [Porting](https://unixnu.com/porting/), [device repository](https://unixnu.com/porting/device-repository), [component catalog](https://unixnu.com/reference/components), UniXNU `components.toml` |
| Hardware | [Device tree](https://unixnu.com/porting/device-tree), [matching](https://unixnu.com/drivers/device-tree-matching), selected SoC README, AGENTS, soc.toml and DTSI |
| Component | Its site page, README, catalog build command, dependencies and source |
| Kernel | [Source and patches](https://unixnu.com/xnu/source-and-patches), [patch reference](https://unixnu.com/xnu/patch-reference), selected OS profile |
| Images | [Prelink](https://unixnu.com/concepts/prelink), [payload](https://unixnu.com/concepts/payload), [UEFI](https://unixnu.com/porting/uefi-firmware) |
| Verification | [Building](https://unixnu.com/building), [bring-up](https://unixnu.com/porting/bring-up) |
| Migration | [Migration](https://unixnu.com/porting/migration) |

## Work

Search the catalog and inspect existing component interfaces before adding code.
Match hardware scope, OS ABI, and board dependencies. Shared SoC resources and
operations belong in the selected SoC repository. Reusable drivers and libraries
belong in their component repositories. Use canonical component IDs and preserve
runtime identifiers recorded in the catalog.

Replace every `REQUIRED_` value, set the board name and firmware paths, and select
the complete dependency set. Verify memory ranges, CPU affinities, interrupts,
firmware hashes, and partition geometry from board evidence. Set `example = false`
after completing the inputs. Pin the SoC commit and inspect resolved profiles.

Build against the UniXNU revision recorded in `upstream.rev` and apply
`patches/unixnu/*.patch` to that checkout. Upstream `main` has a different
packaging model (`unixnu release`, `scripts/package.sh`, `[package]` in
board.toml) that this board does not use: it boots from an ESP image written to
a microSD card, not from a packaged boot image. The build commands are:

```sh
export UNIXNU_TREE=<UniXNU checkout at the pin>
JOBS=10 bash scripts/h1/build-pagani-kernel.sh build kernel
JOBS=10 bash scripts/h1/build-pagani-kernel.sh build package
JOBS=10 bash scripts/h1/build-pagani-kernel.sh build payload
python3 scripts/build-darwinboot-efi.py
bash scripts/h1/make-boot-medium.sh
```

`docs/build.md` is the procedure; `docs/legal.md` is the boundary. Do not add a
`[package]` table or a packaging workflow to this repository unless the whole
build migrates to upstream `main` in one change, with a rebuild and a boot.

The platform seed and the firmware checkout are not in any repository, and the
seed is built on macOS with `hdiutil`.

For a Mu-Silicium initrd payload, include `UniXNURamDiskDxe`, DiskIoDxe and
FAT support in the firmware DSC and FDF. Follow
[required firmware integration](https://unixnu.com/porting/uefi-firmware#required-integration)
before building the firmware and boot image.

Record commands, revisions, output paths, results, and unresolved inputs.
Report hardware boot, storage, display, input, and GPU results separately.
Finish both new ports and migrations with the same
[repository checklist](https://unixnu.com/porting/device-repository#completion).
Follow [publication](https://unixnu.com/porting/device-repository#publish), add
the `unixnu` GitHub topic, and update the device revision in UniXNU's
`devices.toml`. Keep this README concise.

## Porting record

Read the device's `porting.toml` and the
[record guide](https://unixnu.com/porting/records) before implementation.
For storage, also read the [UFS guide](https://unixnu.com/porting/ufs).
Update each affected area's findings or evidence with its code change.
The documentation site renders this same record. Follow its schema and
completion levels for both maintainer and agent work.

## Local builds and boot diagnosis

Copy `unixnu.example.toml` to `unixnu.local.toml` in the main checkout and fill
in device inputs using the [configuration reference](https://unixnu.com/reference/configuration#local-build-inputs).
Read [boot arguments](https://unixnu.com/xnu/boot-arguments#apply-arguments)
before changing the command line. Rebuild the payload and boot image after
argument changes. Use the [green-bar diagnosis](https://unixnu.com/guide/debugging#green-bar)
for stalls at loader handoff. Record the last confirmed stage and relevant logs.

## Hardware adaptation

Follow [Port a device](https://unixnu.com/porting/) in order. Read the
[driver adaptation checklist](https://unixnu.com/porting/drivers) before selecting
components. Follow each subsystem guide for board inputs, implementation paths,
archive integration and expected results. Include board-package services such as
brightness, buttons, Wi-Fi, DSP, battery and CPU frequency in the porting record.
