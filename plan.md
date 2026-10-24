# Nexora OS driver and kernel expansion roadmap

## Target and current baseline

Target both the current VirtualBox reference machine and a practical baseline of modern UEFI
x86-64 PCs. "All drivers" is not a finite target across arbitrary hardware; this roadmap defines
specific devices and retains the UEFI framebuffer and current drivers as compatibility fallbacks.

Already present: core HAL, scheduler, virtual memory, process/object/I/O managers, PCI
enumeration, built-in driver registration, UEFI framebuffer compositor, PS/2 input, AHCI/SATA,
FAT32, Ustar, E1000, DHCP, and bounded packet parsing. Network DNS, routing, TCP, sockets, USB,
NVMe, audio, accelerated graphics, and an additional general-purpose filesystem are not complete.

## Delivery stages

1. **Stabilize boot and instrumentation.** Reproduce and fix the timer-calibration/degraded-boot
   result; measure boot phase durations and compositor frame times on QEMU and VirtualBox. Keep
   startup self-tests deterministic and identify tests that can safely move out of normal boot.
   Gate: normal boot reports healthy, UI smoke cases pass, and startup/frame timing is recorded.
2. **Driver framework and PCI resources.** Evolve the existing `.hzdrivers` and `DRIVER_OBJECT`
   model rather than replacing it. Add explicit device matching, safe PCI BAR/resource ownership,
   DMA allocation/mapping contracts, interrupt registration/masking/teardown, and driver/device
   lifetime rules. Add unit tests for resource bounds and initialization failures.
   Candidate files: `io/driver.c`, `io/resource.c`, `include/driver.h`, and focused tests.
3. **Complete networking.** Build on `NET_INTERFACE`, E1000, DHCP, and packet parsers. Add ARP,
   IPv4 send/receive and routing, UDP checksums, DNS, TCP state/retransmission/timeouts, and a
   bounded socket/user API. Add a second practical NIC target after the framework (VirtIO-net is
   suitable for VM testing; physical NIC choice should be pinned to supported PCI IDs).
   Candidate files: `net/arp.c`, `net/ipv4.c`, `net/udp.c`, `net/dns.c`, `net/tcp.c`,
   `net/socket.c`, `drivers/net/virtio_net.c`, and `include/uapi` API definitions.
   Gate: DHCP, DNS resolution, TCP loopback, and HTTP reachability tests; no claim of HTTPS until
   TLS and certificate validation are separately implemented and tested.
4. **USB xHCI and HID.** Add xHCI controller discovery, command/event/transfer rings, USB device
   enumeration, hub handling as needed, and HID keyboard/mouse parsing. Preserve PS/2 fallback.
   Candidate files: `drivers/usb/xhci.c`, `drivers/usb/usb.c`, `drivers/usb/hid.c` and headers.
   Gate: QEMU/VirtualBox supported USB keyboard and mouse, hotplug/error recovery where supported.
5. **NVMe storage.** Add an NVMe PCI controller driver with queue setup, DMA-safe buffers,
   timeouts/reset, and registration through the existing disk class path. Keep AHCI as fallback.
   Candidate file: `drivers/nvme/nvme.c` plus focused queue/identify tests.
   Gate: read-only identification first; then partition discovery and safe read/write tests on a
   disposable virtual disk.
6. **Additional filesystem.** Implement ext2 read-only first through the existing filesystem
   registration/mount interfaces; only add write support after corruption and recovery tests.
   Candidate files: `fs/ext2/ext2.c`, `fs/ext2/ext2dir.c`, and `fs/ext2/ext2.h`.
   Gate: mount/read/path tests and malformed-image fuzz/regression corpus; FAT32 stays supported.
7. **Audio.** Add an HDA controller and codec discovery path, then a small kernel audio-stream API.
   Candidate files: `drivers/audio/hda.c`, `include/audio.h`, and a targeted playback diagnostic.
   Gate: supported VirtualBox/QEMU device or explicitly selected real codec plays a known test tone.
8. **Accelerated graphics.** Retain the UEFI framebuffer fallback. First choose and support a
   documented virtual GPU (VirtIO-GPU or VirtualBox VMSVGA); implement command validation,
   bounded buffers, synchronization, and display mode ownership before acceleration is exposed.
   Candidate files: `drivers/gpu/virtio_gpu.c` or `drivers/gpu/vmsvga.c`, `include/gfx.h`, and
   compositor integration. Generic Intel/AMD GPU support is explicitly outside this bounded
   target and would require separate per-device roadmaps.
9. **Integration and release gate.** Run the full QEMU suite on one and multiple CPUs, then
   VirtualBox coverage for every selected device, including failure/recovery paths. Update
   architecture, progress, and user documentation from measured results; keep unimplemented
   capabilities explicit.

## Change and validation policy

Implement one stage at a time, build and test each before proceeding, preserve current working
drivers and user changes, and do not claim support for hardware that was not exercised. Driver
choices or real-machine tests requiring unavailable hardware must be called out rather than
simulated as success.
