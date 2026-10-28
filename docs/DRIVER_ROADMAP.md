# Nexora native hardware support roadmap

This roadmap records Nexora-native hardware support. A PCI/USB match or successful controller reset
does not count as a working driver: support needs useful I/O and error handling, with QEMU and
physical-hardware evidence recorded separately. These seven workstreams are active; none implies
that the listed hardware is already supported.

## 1. Wi-Fi

**Current state:** No operational 802.11 adapter or Wi-Fi connection manager. `include/net80211.h`
is only an interface contract. The current wired DHCP/TCP/IP stack is not a Wi-Fi stack.

**Start with:** Build the minimum 802.11 management/security interface and firmware loader, then
select one exact, available adapter ID for a first driver. Intel `iwlwifi` is a candidate only after
the required MAC/PHY, regulatory, authentication, and firmware support is scoped. Do not claim
Intel/Realtek/MediaTek/Qualcomm/Broadcom family support from a vendor name alone.

**Acceptance:** enumerate exact IDs; load only reviewed, redistributable firmware; scan and connect
to a secured access point; acquire DHCP; transfer data; recover from disconnect and reboot.

## 2. Wired Ethernet

**Current state:** Intel 82540EM (`8086:100E`) and 82574L (`8086:10D3`) have QEMU link/DHCP
coverage. RTL8139 (`10EC:8139`) and AMD PCnet (`1022:2000`) now pass QEMU DHCP smoke tests,
including PCnet DMA-ring packet exchange. The PCnet match is limited to the AMD vendor/device ID;
physical cards still need validation. Intel 82579LM (`8086:1502`)
has an experimental identity-only path, not an operational NIC driver. There is no general
Realtek RTL8111/8168 gigabit driver yet. The network startup now creates an independent DHCP worker
for every registered interface, and the interface registry assigns unique `ethN` names when
different drivers both enumerate an adapter as `eth0`. A new two-adapter QEMU case covers this
multi-interface path.

**Start with:** Port an RTL8111/8168 generation from the Linux `r8169` family to Nexora's PCI, DMA,
interrupt/polling, and network-interface interfaces, while preserving the device-ID and revision
gating. In parallel, replace the 82579LM identity-only path with a separately reviewed operational
implementation only when its PHY requirements can be validated.

**Acceptance:** exact supported ID table; QEMU or hardware link; transmit/receive; DHCP; sustained
traffic; reset/link-loss recovery. Physical support must be labeled only after a real-machine test.

## 3. USB controllers and devices

**Current state:** xHCI enumerates multiple devices, boot HID keyboard input works, and USB mass
storage BOT has QEMU read/write coverage with two attached storage devices. The `xhci-controller` smoke case now verifies boot keyboard
key delivery and boot mouse relative motion through the shared input queue. xHCI hubs/hotplug, full
HID report parsing, UASP, and physical controller coverage remain incomplete. EHCI/OHCI support is
absent.

**Start with:** complete xHCI hub/hotplug and USB HID mouse/general report handling, then add BOT
recovery and UASP where the storage stack can support it. Add EHCI/OHCI only for machines whose
USB topology requires them; keep xHCI as the modern baseline.

**Acceptance:** keyboard and mouse, multiple devices, hub attach/detach, USB BOT read/write and
installer use; UASP and each legacy controller tested separately.

## 4. Graphics

**Current state:** UEFI/GOP provides the framebuffer; Nexora's compositor draws through its own
framebuffer path. No native Intel/AMD/NVIDIA GPU driver is present. A declaration in `include/gfx.h`
is not a GPU driver.

**Start with:** define and document the display-device acceleration contract, then target a virtual
GPU such as VirtIO-GPU for a bounded first implementation. Intel/AMD integrated GPU support requires
additional memory-management, display-mode, command-submission, and synchronization work. Discrete
GPU drivers are later work.

**Acceptance:** retain GOP fallback; verify mode setting, framebuffer ownership, acceleration
operations, reset/failure fallback, and compositor stability for each device.

## 5. Audio and Bluetooth

**Current state:** no operational audio or Bluetooth hardware drivers.

**Start with:** add an Intel HD Audio controller and codec discovery path, then output PCM through a
small Nexora audio interface. Treat USB audio as a separate class driver. Bluetooth needs HCI
transport drivers plus a host stack and pairing/security manager; a USB-ID match alone is not
support.

**Acceptance:** codec enumeration, audible output/capture where implemented, device removal and
error handling; Bluetooth discovery, pairing, connection and data transfer on an exact adapter.

## 6. Laptop hardware and power

**Current state:** ACPI table discovery and basic platform timers/power-off exist. Battery state,
lid switches, thermal zones, fans, embedded controllers, I2C-HID touchpads and laptop power policy
are not implemented as complete device support.

**Start with:** add safe ACPI device discovery and read-only battery/thermal reporting before
control operations. Then add I2C-HID touchpad support through a shared HID input layer. Keep
firmware-specific embedded-controller operations gated by exact hardware and conservative limits.

**Acceptance:** test on physical laptop models; validate lid, battery, sleep/wake, thermal readings,
and touchpad input without unsafe fan or power-state writes.

## 7. Additional storage

**Current state:** AHCI/SATA, NVMe, VirtIO block, and xHCI USB BOT are present. NVMe smoke
coverage includes setup/readback, and two simultaneous USB BOT disks pass QEMU enumeration,
filesystem, and read/write coverage. BOT-compatible USB-to-SATA bridges use the same class path,
but no physical bridge has been tested. SD/eMMC, SAS, Fibre
Channel, vendor RAID, SATA port multipliers, and optical drives are not generally supported.

**Start with:** add USB-to-SATA/NVMe bridge coverage using the existing BOT/block layers; then SD
host-controller support and eMMC as separate steps. Defer RAID/SAS/Fibre Channel until the required
SCSI/block queue and recovery interfaces are defined.

**Acceptance:** discover partitions, mount supported filesystems, perform verified read/write,
flush/cache handling, report media errors, and exercise installer source/target use where applicable.

## Shared requirements

- Build a Nexora firmware loader and record firmware origin and redistribution permission per blob.
- Keep hardware-ID match tables narrow and include subsystem/revision constraints when required.
- Use Nexora PCI, DMA, interrupt, memory, synchronization, device, block, network, and input APIs;
  Linux drivers are source references and require a port, not direct binary loading.
- Update `rootfs/Nexora/Drivers/Repository` descriptors only when the implementation and evidence
  state match. Descriptors currently inventory built-in drivers; they are not independently
  loadable packages, and Setup does not fetch drivers online.
- For every implementation record QEMU tests and physical-machine tests separately. Never infer
  physical support from QEMU success.

## Installer driver library

The live and installed `C:\Nexora` tree carries `Drivers\Repository` with Nexora `.nxd`
descriptors, `Drivers\Firmware` for reviewed redistributable firmware, and `Drivers\Setup` for
installer inventory/diagnostics. The current descriptors inventory PCI, AHCI, NVMe, xHCI, legacy
PCI VirtIO block/network, Intel E1000, experimental Intel 82579LM, and RTL8139 implementations.
`python3 scripts/validate-driver-catalog.py` validates descriptor schema, IDs, evidence states,
and source paths before system-tree packaging.
