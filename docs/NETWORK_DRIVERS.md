# Network adapter support

## Supported hardware

The built-in polled Intel Ethernet driver currently binds these controller IDs:

| Vendor:Device | Controller | Status |
| --- | --- | --- |
| `8086:100E` | Intel 82540EM / PRO/1000 MT Desktop | Supported by `drivers/net/e1000.c` |
| `8086:10D3` | Intel 82574L | QEMU `e1000e` initialization and DHCP tested; physical hardware untested |
| `8086:1502` | Intel 82579LM | Experimental exact subsystem/revision path; physical traffic untested |
| `10EC:8139` | Realtek RTL8139 | QEMU DHCP smoke test passes; physical hardware untested |

The driver matches only these exact vendor/device pairs; it does not claim compatibility with the
wider Intel E1000 family. The 82579LM path is restricted to subsystem `1028:052C`, revision `04`,
and remains experimental because Nexora has not validated the PHY workarounds or traffic on that
physical device. The build includes `drivers/net/*.c` in the kernel through
`KERNEL_SRC_DIRS` in the top-level Makefile. The QEMU `network-interface` case exercises the
82540EM emulation, while `network-e1000e` exercises the 82574L model through interface startup,
link detection, and DHCP. These establish emulated paths, not support for other physical adapters.

The driver is linked into `build/hzkrnl.elf`; it is not a separate driver file on the ISO. The
kernel has no external network-firmware blobs to package for this controller. The UEFI firmware
that starts the ISO is supplied by the machine or VM and is not bundled in the ISO.

## Physical-machine prerequisites and limits

On a physical PC, networking requires an enumerated PCI function with one of the exact IDs above,
plus:

- a usable memory-mapped BAR0 at least `0x6000` bytes long;
- a valid nonzero unicast MAC address from the controller's receive-address registers or EEPROM;
- available contiguous DMA memory below 4 GiB, and working PCI memory decoding and bus mastering;
- a connected Ethernet link and an upstream network that provides the expected IPv4 configuration.

The driver polls transmit and receive descriptor rings; it does not require MSI/INTx routing. PCI
discovery must also find the function: the PCI driver uses segment-0 ECAM when available, otherwise
legacy configuration ports, and scans through PCI bridges. Hardware on an unenumerated PCI segment
or behind an unsupported topology will not be found.

The VirtualBox AMD PCnet Am79C973 (`1022:2000`), other Intel E1000-family IDs, Realtek RTL8111/8168
gigabit and Broadcom wired controllers, PCIe Wi-Fi adapters, and USB Ethernet/Wi-Fi adapters remain
unsupported.
The available Windows host reports the Intel 82579LM ID, but this host-side discovery is not a
bare-metal Nexora test.

The presence of `include/net80211.h` is only a wireless-layer interface contract; there is no
wireless device driver or 802.11 supplicant yet. The connected development machine reported no
present Wi-Fi adapter. Do not infer support from a brand name or controller family: check the PCI
or USB vendor/device ID.

Unsupported PCI network-class functions are reported by the driver as
`E1000: unsupported network controller VVVV:DDDD ...`; the driver continues searching for the
one supported ID. This diagnostic does not add hardware support.

## Checks

Build the driver object with the repository toolchain:

```sh
make build/kobj/drivers/net/e1000.c.o
```

Exercise the 82540EM emulation and DHCP initialization with:

```sh
python3 scripts/smoke-test.py --case network-interface
```

Passing this QEMU test verifies the `8086:100E` emulation path only. The `network-e1000e` case
verifies `8086:10D3` emulation. Neither certifies a physical NIC or an unsupported virtual adapter.
