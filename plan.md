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
3. **Complete networking and browser HTTP.** Build on `NET_INTERFACE`, E1000, DHCP, and packet
   parsers. Deliver in testable layers: ARP and IPv4 send/receive/routing; UDP checksums and DNS;
   TCP connection state, retransmission, and timeouts; a bounded socket/user API; then a bounded
   plain-HTTP client wired into browser navigation. Add a second practical NIC target after the
   framework (VirtIO-net is suitable for VM testing; physical NIC choice must be pinned to tested
   PCI IDs). Candidate files: `net/arp.c`, `net/ipv4.c`, `net/udp.c`, `net/dns.c`, `net/tcp.c`,
   `net/socket.c`, `drivers/net/virtio_net.c`, and `include/uapi` API definitions.
   Gate each layer with protocol tests; final gate is DNS plus HTTP reachability through QEMU NAT
   and VirtualBox NAT with bounded response handling. HTTPS remains unavailable until a TLS
   implementation verifies certificate chains and hostnames against a managed trust store.
4. **USB xHCI and HID.** Add xHCI PCI discovery, DMA-safe command/event/transfer rings, controller
   reset and timeouts, USB device enumeration, hub handling, and USB HID keyboard/mouse parsing.
   Preserve PS/2 fallback. Candidate files: `drivers/usb/xhci.c`, `drivers/usb/usb.c`,
   `drivers/usb/hid.c` and headers. Gate: supported QEMU/VirtualBox xHCI keyboard and mouse
   input, plus enumeration failure and recovery tests.
5. **Bluetooth host and device profiles.** Build only after the USB host controller and transfer
   APIs are stable. Add Bluetooth USB transport/controller detection, HCI command/event handling,
   device discovery and pairing, then BLE L2CAP/SMP/ATT/GATT and HID-over-GATT keyboard/mouse.
   Add Classic Bluetooth L2CAP/RFCOMM/profile support and an audio path (A2DP) only when the
   kernel audio-stream interface exists. Candidate files: `drivers/bluetooth/usb.c`,
   `bluetooth/hci.c`, `bluetooth/l2cap.c`, `bluetooth/smp.c`, `bluetooth/att.c`,
   `bluetooth/gatt.c`, and profile modules plus focused protocol tests.
   Gate: tested, explicitly listed USB adapter IDs; BLE scan/pair/GATT and HID input; and Classic
   pairing/audio tests on actual supported hardware. Do not advertise universal Bluetooth or audio
   support from controller detection alone.
6. **NVMe storage.** Add an NVMe PCI controller driver with queue setup, DMA-safe buffers,
   timeouts/reset, and registration through the existing disk class path. Keep AHCI as fallback.
   Candidate file: `drivers/nvme/nvme.c` plus focused queue/identify tests.
   Gate: read-only identification first; then partition discovery and safe read/write tests on a
   disposable virtual disk.
7. **Additional filesystem.** Implement ext2 read-only first through the existing filesystem
   registration/mount interfaces; only add write support after corruption and recovery tests.
   Candidate files: `fs/ext2/ext2.c`, `fs/ext2/ext2dir.c`, and `fs/ext2/ext2.h`.
   Gate: mount/read/path tests and malformed-image fuzz/regression corpus; FAT32 stays supported.
8. **Audio.** Add an HDA controller and codec discovery path, then a small kernel audio-stream API.
   Candidate files: `drivers/audio/hda.c`, `include/audio.h`, and a targeted playback diagnostic.
   Gate: supported VirtualBox/QEMU device or explicitly selected real codec plays a known test tone.
   Bluetooth A2DP depends on this API and is gated separately under Bluetooth.
9. **Accelerated graphics.** Retain the UEFI framebuffer fallback. First choose and support a
   documented virtual GPU (VirtIO-GPU or VirtualBox VMSVGA); implement command validation,
   bounded buffers, synchronization, and display mode ownership before acceleration is exposed.
   Candidate files: `drivers/gpu/virtio_gpu.c` or `drivers/gpu/vmsvga.c`, `include/gfx.h`, and
   compositor integration. Generic Intel/AMD GPU support is explicitly outside this bounded
   target and would require separate per-device roadmaps.
10. **Integration and release gate.** Run the full QEMU suite on one and multiple CPUs, then
   VirtualBox coverage for every selected device, including failure/recovery paths. Update
   architecture, progress, and user documentation from measured results; keep unimplemented
   capabilities explicit.

## Change and validation policy

Implement one stage at a time, build and test each before proceeding, preserve current working
drivers and user changes, and do not claim support for hardware that was not exercised. Driver
choices or real-machine tests requiring unavailable hardware must be called out rather than
simulated as success.

## Nexora OS installer delivery plan

### Scope and decisions

The user completed [NEXORA-OS-INSTALLER-AND-OS-DECISIONS.md](./NEXORA-OS-INSTALLER-AND-OS-DECISIONS.md).
Treat those decisions as product requirements: retain Nexora identity while borrowing Windows 11
usability patterns; installation media starts Setup automatically and exposes About, not the live
desktop; install the full OS and apps to disk. Prioritize reliable functionality, persistent data,
and verified installed-disk boot; defer dual boot, manual partitioning, automatic updates, and
broad recovery until safe.

Nexora already has a native Setup wizard in `user/apps/setup/setup.cpp`, GPT/FAT32 builders in
`user/apps/setup/installer.cpp`, and `install`/`diskboot` smoke cases in `scripts/smoke-test.py`.
Improve these existing surfaces rather than creating a second installer. The destructive path must
never guess a disk, silently erase an existing OS, or claim success without verification. Any
VirtualBox validation must use a new disposable VDI, not the preserved old test disk.

### Phased checklist

#### Phase 1 — Reproduce and establish a trustworthy baseline

- [ ] Build the current tree with the E-drive NexoraBuild toolchain and record the resulting ISO.
- [ ] Run focused QEMU `install` and `diskboot` cases on fresh disposable images.
- [ ] Investigate the known GPT/partition self-test failure independently from installer behavior.
      Record whether it occurs in QEMU, VirtualBox, or both and fix its root cause before treating
      the affected boot as healthy.
- [ ] Inspect current disk enumeration, device identity, eligibility reasons, layout sizing, copy
      progress, error paths, and first-boot/persistence path. Do not make unrelated subsystem
      changes.

#### Phase 2 — Make installation safe and genuinely verifiable

- [ ] Keep the initial supported target as explicit full-disk UEFI/GPT/FAT32 installation. Exclude
      the live medium, the running-system disk, read-only/in-use/unsupported disks, and disks below
      the measured minimum capacity.
- [ ] Present model, actual size, and existing partition information; require explicit target
      selection and refresh; explain why a target is unavailable.
- [ ] Before writing, show the exact selected disk and data-loss effect; require the user to type
      `ERASE` and provide a final Cancel action. Do not start writes on an ambiguous selection.
- [ ] Copy the actual boot and OS payload, configure the bootloader, and verify installed files and
      boot configuration before showing success. Surface failures, preserve local diagnostics
      where possible, and offer retry only when safe.
- [ ] Keep offline installation supported. Label progress by actual operation/stage; provide
      estimated time only when measured/reliable. Allow cancellation after writes begin only at
      safe checkpoints with a warning.
- [ ] Preserve or initialize first-boot account setup, secure password storage/authentication, and
      persistent user files/settings. Do not bypass login or imply persistence unless verified.

#### Phase 3 — Polish the setup experience to the agreed direction

- [ ] Make the installer the default boot experience, start Setup automatically, and keep About
      available on its welcome screen. Do not expose the app desktop in the release boot menu.
- [ ] Refine the existing one-page wizard into responsive Welcome, optional locale/keyboard/network,
      disk choice, supported install choice, final review, progress, verification, and completion
      steps. Use sensible defaults and allow optional network setup without requiring internet.
- [ ] Apply Nexora's charcoal/graphite, original branding, clean sans-serif, restrained transparency,
      configurable accent, and subtle fast transitions. Keep it consistent with the installed
      desktop and avoid audio by default.
- [ ] Completion must state that verification passed, explain removing installation media, and offer
      Restart and Shut Down. If verification fails, never show a success screen.

#### Phase 4 — Validate installed OS and desktop requirements

- [ ] Run QEMU `install` on a new blank disposable disk. Check GPT layout, both FAT32 partitions,
      EFI loader/config, kernel/symbols, actual system payload, and first-boot marker with host-side
      read-back tools.
- [ ] Run `diskboot` from that output through first-boot account setup. Verify boot independent of
      the ISO, actual password checks (when configured), app launch, file/settings persistence
      after another restart, and expected wallpaper/theme/time-zone preferences.
- [ ] Recreate a separate VirtualBox VM with the new ISO and a disposable blank VDI. Install,
      detach the ISO, and verify boot from the VDI; do not reuse the preserved old test disk.
- [ ] Where possible, inspect the desktop at 1024x768 and 1920x1080. Prioritize responsive input,
      reliable window management, keyboard access/focus, accessibility defaults, and real controls.
      Keep unimplemented settings clearly unavailable.

#### Phase 5 — Document and deliver

- [ ] Update README/build guidance for producing the bootable ISO and writing it to USB, tested
      hardware/firmware, safe installation steps, and current limitations.
- [ ] Report modified files and separate QEMU, VirtualBox, and physical-hardware results. Do not
      claim any environment or persistence result that was not directly tested.

### Completion gate

The ISO builds; QEMU installation and installed-disk boot/persistence checks pass with read-back
validation; and a clean VirtualBox VM installs to a disposable VDI and boots independently of the
ISO. Destructive-disk safeguards and honest error reporting are verified. Any failing or
environment-blocked gate remains explicitly incomplete.
