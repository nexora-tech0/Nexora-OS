# Nexora Driver Library

This directory is Nexora's native driver catalog and firmware staging area. It is included on live
media and copied to the installed system with the rest of `C:\Nexora`.

## Layout

- `Repository/`: signed or locally trusted Nexora driver package descriptors (`.nxd`).
- `Firmware/`: firmware blobs only when redistribution rights and provenance are recorded.
- `Setup/`: Setup hardware inventory and driver installation logs.

The current catalog includes legacy PCI VirtIO block and network drivers. Their QEMU smoke cases
verify storage reads/writes and network DHCP respectively; these results do not establish support
for physical devices using VirtIO or for the modern PCI transport.

Nexora uses `.nxd` descriptors and native kernel drivers; it does not load Windows `.inf` or `.sys`
files. Current drivers are linked into the kernel, so repository descriptors document device IDs,
capabilities, and evidence. They do not claim that an untested device is supported or download
drivers from the Internet. Future external package installation must verify a package signature,
hardware-ID match, architecture, and firmware license before activation.

Driver IDs use `vendor:device` for PCI and `class:subclass:protocol` for class-matched USB devices.
`status` records evidence: `qemu-tested`, `implemented-unverified`, or `experimental`. Physical
hardware is tracked separately in `physical-test`.
