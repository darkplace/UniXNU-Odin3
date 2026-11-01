# Notice — third-party components

This project builds on work by others. Their licences apply to their components, not to this
project's code.

| Component | Author | Licence | Used as |
|---|---|---|---|
| UniXNU | UniXNU contributors — <https://unixnu.com/> | BSD-2-Clause-Patent | porting framework, build tooling, loader |
| UniXNU-Pagani `SM8750` | Pagani port authors | see the upstream repository | board layer this device repository derives from |
| UniXNU-Pagani-Drivers | Pagani driver authors | see the upstream repository | driver modules under `modules/Pagani` |
| OpenABL | Open Handheld Collective | GPL-3.0-or-later | bootloader used by the device (not redistributed) |
| EDK2 / Mu | TianoCore and Mu contributors | BSD-2-Clause-Patent | UEFI build environment |
| Linux kernel | Linux contributors | GPL-2.0-only | reference for SD/SDHCI behaviour; cited, not redistributed |
| XNU | Apple Inc. | Apple Public Source Licence / BSD | patches apply to its open-source releases; XNU is not redistributed |

## Trademarks

Apple, iPadOS and XNU are trademarks of Apple Inc. Qualcomm and Snapdragon are trademarks of Qualcomm
Incorporated. AYN is a trademark of its owner. This project is not affiliated with, endorsed by or
supported by any of them.

This project's licence is BSD-2-Clause-Patent (SPDX: `BSD-2-Clause-Patent`); the full text is in
[LICENSE](LICENSE).
