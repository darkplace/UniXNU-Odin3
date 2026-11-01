# Build and media

## The model: process, not artefacts

This repository publishes the **process** that turns Apple's open-source XNU plus this project's
drivers into a bootable card. It does not publish Apple software, and it does not publish any patch
that modifies a shipped Apple binary.

| Published | Not published |
|---|---|
| Patches against `apple-oss-distributions/xnu` (open source) | IPSW files |
| Patches against the UniXNU build tool, with their reason | Decompressed kernelcaches |
| This board's drivers, device tree and host tooling | Trust caches |
| The procedure, its inputs and the hashes to compare | Bootable images, prelinked kernels |

## What you must supply

| Input | Where it comes from |
|---|---|
| iPadOS restore image | your own `iPad_Pro_HFR_17.7.11_21H461_Restore.ipsw`, exported as `UNIXNU_IPSW` |
| XNU sources | pinned by `unixnu.lock`, fetched by `unixnu fetch` |
| SoC feed | pinned by `feeds.toml` |
| Firmware sources | `Project-Silicium/Mu-Silicium` at `4cba43c90e02`, for `unixnu build package` |
| Toolchain | the `unixnu-toolchain:v1` container, built from `scripts/toolchain/Dockerfile.toolchain` |
| Data seed | built once on macOS from the Data volume of your IPSW |
| owner-signed policy manifests | built once on macOS by `scripts/q2/mac-osroot.sh` from your IPSW |
| microSD card | 64 GiB or larger, written partition by partition |

## Hosts: two, and the order matters

Linux does everything except two inputs that need `hdiutil`, and it produces them from your IPSW:

| Input | Produced by | Read by |
|---|---|---|
| the OS and SystemOS disk images (`p2`, `p3`) | `scripts/h1/make-target-volumes.sh` on Linux, or `scripts/q2/mac-q2.sh` on macOS | `scripts/h1/write-system-card.sh` and `scripts/stage-owner-policies.py` |
| owner-signed policy manifests (`verified-root-os`, `verified-root-systemos`) | `scripts/q2/mac-osroot.sh` on macOS | the kernel step, through `scripts/stage-owner-policies.py` |
| the Data seed (`data.dbrdisk`) | `scripts/prepare-ram-data.py` on macOS | `unixnu build payload` |

`make-target-volumes.sh` needs no Apple tool: it asks the toolchain for the two IPSW members and
decodes them with `unixnu/udif.py`, which is what `hdiutil attach` would expose as `/dev/rdiskN`. It
runs in the toolchain container only because the OS image carries LZFSE chunks and the decoder ships
there. Measured against this board's IPSW: `044-76040-038.raw` is 6 077 580 800 B with SHA-256
`e5634d04…` and `044-76034-038.raw` is 3 303 014 400 B with SHA-256 `b4908cde…`, both identical to
what the macOS pipeline produced.

The other two inputs still come from a Mac. If you skip them the kernel build stops with
`missing owner policy inputs: …/verified-root-os/manifest.bin, …`. The paths are read from
`IDRAGON_PROJECT`, which `scripts/h1/build-pagani-kernel.sh` exports as this repository's directory.

## 0. Before you spend an hour

```sh
UNIXNU_IPSW="$UNIXNU_IPSW" UNIXNU_TREE="$UNIXNU_TREE" bash scripts/h1/check-host.sh
```

It checks the host tools, the toolchain image, the tree at the pin with its patches, the restore image,
the firmware sources, the macOS inputs and the free space, and prints the command that fixes each
`MISS`. It writes nothing. Nothing else in this document runs until this passes.

**The toolchain image.** It carries the patched clang, cctools, `mig`, `iig` and `lzfse`, so nothing is
installed on the host:

```sh
docker build -t unixnu-toolchain:v1 -f scripts/toolchain/Dockerfile.toolchain .
```

Its own build downloads the toolchain, so it takes a while and needs network; the time is not measured
here.

**The firmware sources.** `build package` produces `UniXNU.efi`, the loader OpenABL starts, and needs a
Mu-Silicium checkout:

```sh
git clone https://github.com/Project-Silicium/Mu-Silicium
git -C Mu-Silicium checkout 4cba43c90e02
export UNIXNU_UEFI="$PWD/Mu-Silicium"
```

`tools.toml` records these pins; `check-host.sh` compares against them.

## 1. The build tree at the pinned revision

This board is built against a specific UniXNU revision, recorded in [upstream.rev](../upstream.rev).
Upstream `main` has since moved the SM8750 build recipes out of the tool and changed the packaging
model, so **the checkout must be the pin**, not `main`:

```sh
git clone --recurse-submodules https://github.com/UniXNU/UniXNU
cd UniXNU
git checkout "$(cat ../UniXNU-Odin3/upstream.rev)"
```

Then apply this repository's host patches, in order. They are three small changes to the build tool,
each with its reason in the patch header:

```sh
for p in ../UniXNU-Odin3/patches/unixnu/*.patch; do git apply "$p"; done
```

| Patch | What it changes and why |
|---|---|
| `0001-toolchain-warnings.patch` | passes `-Wno-error=` for three clang warnings introduced after the pin, so the component archive scripts compile |
| `0002-devicetree-aapl-phandle.patch` | emits `AAPL,phandle` next to `phandle`, because XNU resolves `interrupt-parent` through `gIODTPHandleKey` and a plain `phandle` leaves the provider NULL |
| `0003-kernel-missing-objects.patch` | reruns the bootstrap pass while a recorded object file is missing, because the baseline script deletes stamped objects before `make` |

Without the pin the build stops with `build script missing: …/build-native-ufs.py`; without the
patches it stops on warnings or links a kernel with no interrupt parents.

## 2. Configure

```sh
export UNIXNU_IPSW="$HOME/ipsw/iPad_Pro_HFR_17.7.11_21H461_Restore.ipsw"
export UNIXNU_UEFI="$HOME/Mu-Silicium"          # only for `build package`
export UNIXNU_TREE="$PWD"                        # read by this repository's host scripts
./unixnu defconfig odin3_defconfig -D ../UniXNU-Odin3
./unixnu ipsw fetch -D ../UniXNU-Odin3 --ipsw "$UNIXNU_IPSW"
./unixnu fetch
./unixnu status --json

`ipsw fetch` decompresses the kernelcache from your restore image into `out/ipsw/21H461/`. The board's
component recipes read it from there (`components.toml`, the `--kernelcache` argument), so the kernel
step fails with `No such file or directory: ../../../../ipsw/21H461/kernelcache.decompressed` until
this has run once.
```

`status` must report `"device"` pointing at this repository, `schema_version: 2`, and the twelve
components of `board.toml` resolved.

## 3. Kernel

```sh
JOBS=10 bash scripts/h1/build-pagani-kernel.sh build kernel
```

The script enters the toolchain container, applies `patches/xnu/series` in order and prelinks against
the kernelcache it decompresses from your IPSW. Output: `out/odin3/<profile>/odin3/kernel/kernel.macho`.

## 4. Loader, payload and device tree

```sh
JOBS=10 bash scripts/h1/build-pagani-kernel.sh build package
JOBS=10 bash scripts/h1/build-pagani-kernel.sh build payload
python3 scripts/build-darwinboot-efi.py
```

`build package` produces `UniXNU.efi` and `HelloPayload.efi` in the package output and compiles
`device-tree/hardware.dtb` from `dts/odin3.dts`. `build payload` packs `payload.img` from
`kernel.macho`, its ADT, the trust cache and the Data seed.

## 5. The ESP image

```sh
UNIXNU_TREE="$PWD" bash scripts/h1/make-boot-medium.sh
```

It writes `out/odin3-sd.img` (alias `odin3-boot.img`), `MANIFEST.sha256` and `SEAL.json`: a FAT32 partition of
400 MiB with `\UNIXNU.EFI`, `\ODIN3.DTB`, `\EFI\UniXNU\unixnu.toml`, the payload and the BLS entry
OpenABL's menu lists. It also injects `/reserved-memory/splash_region` into the device tree, without
which OpenABL turns the panel off at `ExitBootServices`.

The script writes a **file**. It never touches a block device.

## 6. The card, in order

The card is `p1` ESP, `p2` OS, `p3` SystemOS, `p4` UniXNUData, at the geometry recorded in
[docs/hardware.md](hardware.md). The order below is the only one that works: each step needs the map
the previous one left.

| # | Command | Writes | Its guards |
|---|---|---|---|
| 1 | `bash scripts/h1/make-target-volumes.sh` | `out/target-volumes/*.raw` (files) | compares both volumes against the measured SHA-256 |
| 2 | `bash scripts/h1/make-boot-medium.sh` | `out/odin3-boot.img` (file) + `MANIFEST.sha256` + `SEAL.json` | a file, never a device; hashes read back from the image |
| 3 | `pkexec bash scripts/h1/write-system-card.sh /dev/sdX` | `p1`, `p2`, `p3` | USB only, nothing mounted, card large enough, every byte verified by SHA-256 |
| 4 | `pkexec python3 scripts/h1/sd_data_partition.py plan /dev/sdX --guid <uuid> --out <dir>` then `apply` | `p4` | backs up every sector it will change, needs the plan id to apply |
| 5 | `bash scripts/h1/sd-inspect.sh /dev/sdX` | nothing | reads the card back: partition map, ESP contents, hashes, driver log |

Step 1 runs on Linux and needs no Apple tool:

```sh
UNIXNU_IPSW="$UNIXNU_IPSW" UNIXNU_TREE="$UNIXNU_TREE" bash scripts/h1/make-target-volumes.sh
```

It writes `out/target-volumes/044-76040-038.raw`, `044-76034-038.raw` and one `.json` record each, with
the SHA-256 of your own IPSW. `write-system-card.sh` reads them from there.

After step 3 the card has three partitions; step 4 adds the fourth at LBA 19144704. `sd-inspect.sh
write <image> <sha256>` is a *refresh* path for `p1` alone once the card exists, not part of the
first build.

## 6b. Measured sizes and times

| Step | What it costs on this machine |
|---|---|
| kernel build (first time, from the pin) | 35–40 min, 10 jobs |
| `unixnu ipsw fetch` of the two volumes | about 5 min, 8.3 GiB extracted |
| decode of the OS volume | 37 s, 6.08 GB written |
| ESP image assembly | about 2 min, 125 MB of files |
| `write-system-card.sh` | about 10 GB written, verified byte by byte |
| disk needed | ~30 GiB: IPSW 6.8 GiB, tree and cache ~2 GiB, volumes 9.4 GB, card images ~10 GB |

## 6c. The one step that needs macOS

Two inputs still come from a Mac, and the kernel build fails without them:

| Input | Produced by | Where `scripts/stage-owner-policies.py` looks for it |
|---|---|---|
| owner-signed policy manifests | `scripts/q2/mac-osroot.sh`, with the `UniXNU-qemu` checkout pinned in `tools.toml` | `$IDRAGON_PROJECT/firmware/UniXNU/out/qemu/tree/build/verified-root-{os,systemos}` |
| the Data seed `data.dbrdisk` | `scripts/prepare-ram-data.py` | passed to `unixnu build payload` |

Both use `hdiutil`, which is macOS only. A Linux-only host stops before the kernel; everything before
that point, and everything after it, runs on Linux.

## 7. What the seal records

`scripts/h1/seal-medium.py` runs at the end of the boot-medium step and writes `SEAL.json` with every
input that produced the medium: the device repository's commit and whether its tree was clean, the
build tree's commit and `upstream.rev`, the SHA-256 of each patch in `patches/unixnu/`, the SHA-256 of
`unixnu.lock`, `board.toml` and the defconfig, the docker image id of the toolchain, the SHA-256 of
your IPSW, and the table of `MANIFEST.sha256`. No absolute paths.

To rebuild a sealed medium, check out the two recorded commits, apply the recorded patches, supply an
IPSW with the recorded hash, and follow this document. The seal names inputs, not results: see
"Hashes to compare" for what that does and does not guarantee.

## 8. Boot

Hold Volume Down during power-on to open the OpenABL menu and choose the `UniXNU Odin 3` entry.

## Hashes to compare

Every medium records the SHA-256 of its own contents in `MANIFEST.sha256`, and the kernel step prints
the kernel hash when it finishes. These are the recorded builds of this board:

| Build | `kernel.macho` | Size |
|---|---|---|
| the card written on 2026-10-29 | `b1014e5d…` | 124 755 968 B |
| the build tree on 2026-10-30 | `3016cd72…` | 124 846 080 B |
| this repository, before the date fix, run 1 and run 2 | `ded6e2e6…`, `db41a63a…` | 124 841 984 B |
| this repository, with `KERNEL_BUILD_DATE`, run 1 and run 2 | `0ba7ad99…`, `db935b02…` | 124 841 984 B |

Three measured facts:

- **The version string and the Mach-O UUID are now stable.** XNU writes its build date into
  `version.c` through `newvers.pl`, which honours `KERNEL_BUILD_DATE`
  (`apple-oss-distributions/xnu/config/newvers.pl:162`); clang stamps `__DATE__`/`__TIME__` from
  `SOURCE_DATE_EPOCH`. `scripts/h1/build-pagani-kernel.sh` sets both to the date of the pinned
  revision. Before that every build carried the hour it ran at, and the LC_UUID followed the content.
- **The kernel is still not byte-reproducible, and the difference is in the XNU link.** Two builds
  from the same checkout, the same patches and the same toolchain image differ in about 1 930 bytes.
  The prelink step is not the cause: run twice over the same inputs, with different
  `PYTHONHASHSEED`, it produces the same file. The difference is already in
  `mach.development.bcm2837.unstripped.noctf`, the linked kernel the prelink step receives — 1 906
  bytes there, all past the last section, in symbol-table entries whose `n_value` moves by a few
  hundred bytes from build to build. **Which link input moves, and why, is not determined.** Compare
  size and behaviour between builds, not the hash.
- **The 2026-10-29 medium is not produced by today's tree.** The drivers and the patch series changed
  after the card was written. Reproducing that exact medium needs the tree as it was that day, which
  this repository does not carry as a snapshot; what it carries is the procedure and the current
  state. Treat its row as the record of a measurement, not as a build target.

## Patch model

`patches/xnu/series` lists, in order, the patches applied to XNU source. Adding one means adding the
file and its name to the list. Each patch carries its reason in the header. Board-independent patches
come from the SoC and OS layers pinned in `feeds.toml`; the host patches under `patches/unixnu/` are
applied by hand to the tool checkout, as in step 1.

## Scheduler profile

`patches/xnu/odin3-clusters.patch` defines `__ARM_AMP__` and enables the Edge scheduler. Kernels built
with it reach userland but never hand the display to SpringBoard on this device; a kernel built
without it reaches the lock screen. The patch is kept in the tree but is **not** in
`patches/xnu/series`, so the default build produces the working kernel. `scripts/h1/make-noedge-profile.py`
reproduces the same profile from an explicit snapshot.

## Not reproducible

- The kernel binary, byte for byte: two runs of the same build differ (see above).
- Anything that depends on your IPSW being exactly the build named here.
- The Data seed, which is produced on macOS from that IPSW; a different seed is a different boot.
- Boot time, which varies with the card, the persisted data and the services that start.
