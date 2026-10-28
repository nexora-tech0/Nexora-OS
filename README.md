# Nexora OS

<img src="assets/branding/nexora-os-logo.svg" width="96" align="right" alt="Nexora OS logo">

An original hobby operating system for x86-64 UEFI PCs. Its kernel is structured like
Windows NT (HAL, Ke, Mm, Ob, Ps, Io, Ex, win32k), and its desktop is inspired by the look of
Windows 11. It is written in C, C++, Rust and assembly, and it boots in VirtualBox and QEMU. It
contains no Microsoft code or assets: the name, logo, icons and wallpapers are its own, and the
interface font is Inter (SIL Open Font License).

**Status: version 1.1.0.** The roadmap ended with 1.0.0 (M11); 1.1.0 (M12) added the window
manager's mouse lock.

![The Nexora OS desktop with Notepad, Files and the Terminal](docs/images/desktop.png)

## What is in it

- **Kernel:** a HAL (GDT/TSS/IDT, ACPI, Local and I/O APICs, calibrated timers), a symbolized
  stop screen, a memory manager (PFN database, W^X kernel page tables, demand-zero paging,
  guarded stacks, a checksummed pool), preemptive threads with 32 priorities and cycle-based
  quanta, NT-style waits, events, semaphores, mutexes, timers and DPCs, an object manager with
  handles and a namespace, processes in their own address spaces running PE32+ programs, and an
  I/O manager with IRPs.
- **Multiprocessor:** every processor the firmware reports (up to 8) runs threads: one ready
  queue shared under the dispatcher lock, thread affinity, inter-processor interrupts for
  rescheduling, TLB shootdown and stopping the others at a crash.
- **Hardware:** PCI inventory in Settings > Devices, AHCI/SATA and NVMe storage with GPT
  partitions, an Intel 82540EM (`8086:100E`) Ethernet driver, PS/2 keyboard and mouse, the CMOS
  clock, ACPI power off and reset, and the UEFI framebuffer. QEMU smoke tests verify full Setup
  installation and readback on AHCI and NVMe disks. Physical devices still need model-by-model
  validation. See [network adapter support](docs/NETWORK_DRIVERS.md) for NIC coverage.
- **File systems:** a read-only C: from the boot ramdisk on the live system, and read/write
  FAT32 with long file names (write-back FAT cache, multi-cluster transfers).
- **Desktop:** win32k's windows, message queues and input routing, a compositor with Mica-like
  translucency, rounded corners, soft shadows and an eased desktop reveal; hzui, a C++ toolkit
  drawn in user mode with shared Inter body, large-body, display and technical-text font roles,
  cached font files and fallback faces; the shell with a timezone-aware lock-screen clock
  (optionally with a password), a floating pill-shaped dark top dock with rounded status and clock bubbles, an animated bottom taskbar, Control Center,
  Start with search, Alt+Tab, a clipboard, and consistent themed right-click menus. The
  supplied photographs in `assets/Wallpapers` are packaged with the system; Settings can choose
  independent desktop and lock-screen images. Accounts with a password get a separate sign-in
  layout with network, accessibility and power controls; password-free sessions keep the existing
  clock-first lock screen. Settings includes System, Personalization, Network & Internet, Devices,
  Apps, Accounts, Privacy & Security, Accessibility, Date & Language, Updates & Recovery, and About
  Nexora OS; unsupported services are identified rather than shown as working controls.
  App shortcuts can be pinned independently of Start's app tiles, and those taskbar pins are
  saved in user settings.
- **Programs:** Notepad (word wrap, find, undo), Files (copy, cut, paste and delete of whole
  folders, with item context menus), Terminal (built-in commands, console programs, `> file`, `| more`, line wrapping),
  Calculator, Clock, Calendar, Stopwatch, System Information, Paint (mouse drawing and BMP export),
  Browser (native shell with address entry, history, bookmarks, downloads listing, local files,
  bounded HTTP GET and verified TLS 1.2 HTTPS; pages use structured HTML, a conservative CSS
  subset, and bounded PNG/JPEG/GIF/BMP images; JavaScript and full browser compatibility are not
  supported), Settings (searchable Home and
  category navigation, five shared wallpaper choices, account credentials, display/storage/network
  status, time zone and live system information; sound, USB/Bluetooth and advanced management
  features are clearly marked unavailable when their system APIs are not implemented), Task Manager (processes,
  per-processor load) and Setup, which installs Nexora OS on a disk. New native app
  source directories are discovered by the build automatically and checked against their manifests.
  The shell loads the app catalog from `C:\Nexora\Apps`.
- **Robustness:** a crashing program ends alone and the shell says which one; if the shell
  itself dies, the session starts it again; at shutdown programs are asked to close (Notepad
  asks about unsaved work) before the volumes are flushed.

## Walkthrough

### 1. Installation media

Build the ISO (below) or take `build/nexora-os.iso`, and start it in a UEFI virtual machine
(VirtualBox: `scripts/create-vbox-vm.sh build/nexora-os.iso --wipe-disk --start`). The boot menu
automatically starts Setup. Installation media exposes the installer and its **About Nexora OS**
information, not the application desktop. The complete OS and apps are bundled on the ISO for
offline installation and copied to the selected disk.

### 2. Install

Setup lists disks with their identity, capacity and current partitions. Select the intended disk,
review what will be erased, type `ERASE`, and choose Install; Cancel returns to disk selection
without writing. Setup rechecks the target immediately before writing and verifies the GPT/FAT32
sectors and copied files before reporting success. It partitions the disk (GPT: an EFI system
partition and a FAT32 system partition), installs the boot loader and copies the complete system.
Then restart without the ISO (VirtualBox:
`scripts/create-vbox-vm.sh --boot-disk --start`). The installed Limine menu defaults to Nexora OS,
so subsequent boots start the installed system automatically; first boot opens the account setup.

![Setup](docs/images/install.png)

### 3. First boot

The first start from the disk opens first-boot setup instead of the lock screen: your name, an
optional password (stored as a salted hash) and a wallpaper. Your documents folder is
`C:\Users\<name>\Documents`; Notepad, Files and the Terminal start there.

![First-boot setup](docs/images/firstboot.png)

From then on the lock screen greets you by name and, with a password, asks for it. Settings >
Accounts changes the name and the password, Settings > Time the time zone the clocks show, and
Start > Power has Lock, Restart and Shut down.

![Settings, Accounts](docs/images/settings.png)

## Build

Either use Docker:

```sh
scripts/build.sh iso          # builds the container once, then build/nexora-os.iso
```

or build natively on Linux / WSL2. The pinned LLVM, Rust and Limine install without root:

```sh
sudo apt install make git curl xz-utils gcc libc6-dev nasm xorriso mtools gdisk dosfstools qemu-system-x86 ovmf python3 python3-pil
make toolchain                # ~/.nexora-os-toolchain (about 1.4 GB)
make iso
```

## Run and test

```sh
make run                # QEMU + OVMF, serial on stdio (QEMU_SMP=4 make run: four processors)
make test               # headless boot tests: boot, desktop, shell, apps, SMP, recovery, install,
                        # boot from disk, accounts, diagnostics, input, power; screenshots in build/test/
python3 tests/wincompat/loader_cases/test_pe_fixtures.py # structural PE-fixture model, not OS execution
python3 scripts/smoke-test.py --case catalog-apps  # launch the expanded built-in app catalog
python3 scripts/smoke-test.py --case network-interface  # probe the E1000 driver in QEMU
QEMU_SMP=4 make test    # the same on four processors
make testdisk           # build/test/disk.img: the GPT + FAT32 test disk (make run and make test attach it)
make vbox               # create or refresh the VirtualBox VM "Nexora OS" with the new ISO
```

Nexora runs its own statically linked PE32+ applications and has an initial, deliberately narrow
Windows console compatibility path for AMD64 PE32+ console images. Built-in `kernel32.dll` and
`kernelbase.dll` import thunks currently cover `GetStdHandle`, synchronous `WriteFile`,
`ExitProcess`, `GetLastError`, `SetLastError`, `CloseHandle`, `GetCurrentProcessId`,
`GetCurrentThreadId`, `QueryPerformanceCounter` (1 MHz), `GetSystemTimeAsFileTime`,
`InitializeSListHead`, `GetCommandLineA`, `GetProcessHeap`, `HeapAlloc`, and `HeapFree`; the heap
shim uses tracked page-backed allocations and is not the Windows heap implementation. This is not
a Windows DLL loader.
Unsupported architectures, GUI subsystems, TLS/delay imports, DLLs, and APIs fail closed. Terminal
and desktop launch failures can show a bounded compatibility report listing the image format and
imported DLL/API support status. The report does not imply that listed imports are implemented as
full Windows contracts: for example, overlapped I/O and consistent Win32 last-error mapping are
not supported. General CRT startup, app-local/redistributable DLL loading, exception/unwind
behavior, Win32 GUI APIs, and Windows drivers remain unsupported. Settings > Devices reports
enumerated PCI IDs and the actual startup result for supported built-in drivers. Nexora currently
has native AHCI and NVMe storage drivers; Windows `.sys` drivers cannot be loaded by Nexora's
kernel. The Intel 82579LM entry is identity-only, not an operational NIC driver.
This is not general Windows
application compatibility or a Linux ELF/syscall runtime.
The native Nexora Browser shell can open local text and HTML files using an absolute path or a
`file:///C:/...` URL. Remote pages use a bounded structured HTML renderer, a small external
stylesheet subset, and limited PNG/JPEG/GIF/BMP loading; JavaScript, the DOM, full CSS layout,
and general-purpose browser compatibility are not implemented. Its networking code supports
bounded HTTP GET and TLS 1.2 HTTPS with certificate and hostname checks. The latest QEMU/NAT HTTP
fixture passed, but the latest external HTTPS test resolved DNS and then timed out before a verified
page load; reliable external browsing is therefore not yet verified. Network hardware support is
limited to the documented Intel 82540EM (`8086:100E`)
driver; other physical NICs are not claimed to work. The `network-interface` and `dns-api` smoke
cases verify QEMU E1000/NAT initialization and DNS, respectively, not general physical-network
support. Firefox also requires DLL loading and Windows API support that Nexora does not provide.

On Windows, run these from a WSL2 shell in the checkout; `make vbox` drives the Windows
VirtualBox through `VBoxManage.exe`. The VM logs COM1 to `serial-com1.log` in its VM folder.
`scripts/create-vbox-vm.sh --cpus 2` gives the VM two processors; the script also sets the VM's
TSC mode, which multi-processor guests need on VirtualBox's Hyper-V backend (see
[docs/PROGRESS.md](docs/PROGRESS.md), known issues).

## Layout

`hal/ ke/ mm/ ob/ ps/ io/ ex/ rtl/ init/` kernel (C17 + NASM) · `win32k/` window manager
(C++20) with `win32k/rgn/` region library (Rust) · `drivers/` · `user/` user-mode libraries
(hzsys, hzrt, hzuser, hzui) and programs (C++20, PE32+) · `tools/` host tools · `third_party/`
vendored, pinned dependencies (see [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md)).

How it works: [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md). What was built, milestone by
milestone, and what is known not to work: [docs/PROGRESS.md](docs/PROGRESS.md).
