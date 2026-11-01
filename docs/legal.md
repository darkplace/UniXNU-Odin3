# Legal position

This project documents and distributes a **porting process**. It does not distribute Apple software.

## In this repository

- Source code written for this project: drivers, device tree, host tooling.
- Patches against Apple's open-source releases (`apple-oss-distributions/xnu`), which keep their own
  licence.
- Documentation: hardware facts, design notes, procedures.

## Not in this repository

- IPSW restore images, kernelcaches, trust caches, SEP or iBoot components.
- Any patched or modified Apple binary.
- Bootable images, prelinked kernels or any build output containing Apple code.
- Decryption keys, signing material, or anything that bypasses Apple's code signing.

## What you must do yourself

You must own the restore image for the OS you port. The build reads it on your machine: it decompresses
the kernelcache and prelinks the kernel against it. The resulting kernel contains Apple code — keep it
on your own device, do not redistribute it.

## Code signing

The project does not bypass Apple's signature validation. One example: the hash table XNU derives for
the shared cache does not survive a power cycle, and rebuilding it costs minutes on every boot.
Persisting it would skip validation, so this project does not do it, and says so in its design notes.

## Hardware

The internal UFS and the bootloader are never written. The device boots from an external card, and the
tooling checks its target before writing: transport, logical sector size, partition map and image
hash. Writing to a card destroys its first partition; you do it at your own risk, on your own hardware.

## No warranty

Provided as is, without warranty of any kind. A device modified by following these instructions may
stop booting. Recovery paths are documented where they are known; where they are not, the document
says so.

## Third-party material

UniXNU, OpenABL, EDK2/Mu and the referenced device documentation belong to their authors and keep their
licences. Names and licences are listed in [NOTICE.md](../NOTICE.md).
