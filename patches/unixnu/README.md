# Patches for the build tool

These patches apply to the UniXNU checkout named by [upstream.rev](../../upstream.rev), not to `main`.
Apply them in order after checking out that revision:

```sh
for p in patches/unixnu/*.patch; do git apply "$p"; done
```

They change host tooling only: no patch here touches a shipped Apple binary, and none is part of the
XNU source series in [patches/xnu/series](../xnu/series).

| Patch | Reason |
|---|---|
| `0001-toolchain-warnings.patch` | Three component archive scripts pass `-Werror` to a clang that grew `-Wunsafe-buffer-usage`, `-Wunnecessary-virtual-specifier` and `-Wshadow-header` after the pin. Without the exemptions the archives do not build. |
| `0002-devicetree-aapl-phandle.patch` | XNU's `FindPHandle` reads `AAPL,phandle`, not `phandle` (`iokit/Kernel/IODeviceTreeSupport.cpp`, `gIODTPHandleKey`). A node that only carries `phandle` leaves the interrupt provider NULL and the ADT build reports a data abort on `IODTGetICellCounts`. The patch emits both keys. |
| `0003-kernel-missing-objects.patch` | `scripts/build-xnu-4k-baseline.sh` deletes the objects whose mode stamp changed (boot progress, crypto, mockfs, cs-hash-cache) before it calls `make`. A pass that stops at its archive check leaves them deleted while their `.o.json` records remain, and the archive builders then run `nm` over files that no longer exist. The patch reruns the bootstrap pass until every recorded object is present. |

`0002` and `0003` are worth sending upstream; they are kept here as patches so this repository
reproduces the medium it documents without depending on a fork.
