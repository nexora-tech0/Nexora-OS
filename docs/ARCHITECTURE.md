# Nexora OS architecture

Nexora OS is an original hobby OS for x86-64 UEFI machines (VirtualBox is the reference
target). Its kernel is structured like Windows NT: a layered hybrid kernel with NT-style
component prefixes. The shell is inspired by the look and feel of Windows 11. No Microsoft code,
names, fonts, logos or artwork are used. This file records design **decisions**; the current
state lives in [PROGRESS.md](PROGRESS.md).

## 1. Layers

```
 user mode   ┌──────────────────────────────────────────────────────────────────────────┐
 (ring 3,    │ apps: Explorer, Notepad, Terminal, Settings, Calculator, Task Manager,    │
  PE32+)     │       Paint, Installer            shell: desktop, taskbar, Start, lock    │
             ├──────────────────────────────────────────────────────────────────────────┤
             │ hzui (C++ UI toolkit; renders controls + text into window surfaces)       │
             │ hzuser (window API: CreateWindow/GetMessage/DispatchMessage-style)        │
             │ hzrt (C/C++ runtime)      hzsys (syscall stubs, like ntdll)               │
 ────────────┴──────────────────────── SYSCALL / SYSRET ─────────────────────────────────┤
 kernel      │ Ex  syscall table, events, mutexes, semaphores, sections                   │
 (ring 0)    │ win32k  windows, z-order, message queues, input routing, compositor (C++)  │
             │         region/clip library (Rust, win32k/rgn)                            │
             │ Io  driver/device objects, IRPs, VFS      Ob  objects, handles, namespace │
             │ Ps  processes, threads, PE32+ loader      Mm  PFN db, paging, pool, VADs  │
             │ Ke  traps, IRQL, spinlocks, scheduler, timers, DPCs, bug checks           │
             │ drivers/  bootvid, serial, ps2, pci, ahci, fat32, rtc, acpi power, ...    │
             │ Hal GDT/IDT/TSS, ACPI tables, LAPIC/IOAPIC, timers, port I/O, COM1         │
             │ Rtl strings, formatting, time      init/  phase-0 startup, loader block   │
             └──────────────────────────────────────────────────────────────────────────┘
 firmware    UEFI (OVMF / VirtualBox EFI)  →  Limine 11.4.1  →  hzkrnl.elf + ramdisk.tar
```

## 2. Languages

| Language | Where | Rules |
|---|---|---|
| C17 (freestanding) | kernel, HAL, executive, drivers | `-ffreestanding -fno-stack-protector -mno-red-zone -mcmodel=kernel -mgeneral-regs-only`, frame pointers kept, `-Werror` |
| NASM x86-64 | entry, ISR stubs, context switch, SYSCALL entry | one `.asm` per concern, `default rel` |
| C++20 | win32k, UI toolkit, shell, apps | no exceptions, no RTTI, no thread-safe statics; kernel C++ uses `rtl/cxxrt.cpp` |
| Rust (stable, `no_std`) | `win32k/rgn`, the region/clipping library | `x86_64-unknown-none`, `panic = "abort"`, C ABI only, panics become `RUST_PANIC` stop screens |

**One Rust static library.** Every Rust `staticlib` embeds its own copy of `core` and
`compiler_builtins`, and there can be only one `#[panic_handler]`. All kernel Rust therefore lives
in (or is a dependency of) the single crate `win32k/rgn`. If a second kernel Rust component
appears, it becomes a module or path dependency of that crate, not a second `.a` file.

**No floating point in ring 0.** `-mgeneral-regs-only` means kernel code never touches x87/SSE
state, so M3's context switch saves only general registers for kernel threads, and user FPU
state is saved lazily/XSAVE on switch. Consequences:
- Kernel-drawn text (boot splash, boot log, stop screen) uses glyph bitmaps rasterized **at
  build time** by `tools/fontgen` (host C + stb_truetype). The kernel links no TrueType
  rasterizer.
- The win32k compositor uses integer/fixed-point math only: alpha blending, box-blur for the
  Mica-like material, rounded-corner coverage.
- TrueType rasterization of UI text happens **in user mode** (see §8).

## 3. Repository layout

```
nexora-os/
├── Makefile  Dockerfile  rust-toolchain.toml  README.md  THIRD_PARTY_NOTICES.md
├── scripts/      versions.env (pins), setup-toolchain.sh, build.sh, run-qemu.sh,
│                 smoke-test.py, create-vbox-vm.sh
├── boot/         limine.conf
├── docs/         ARCHITECTURE.md, PROGRESS.md
├── include/      kernel-wide headers, one per component (hal.h, ke.h, rtl.h, bootinfo.h, ...)
├── init/         entry.asm (KiSystemStartup), init.c (phase 0), bootinfo.c (Limine), inbv.c
│                 (boot display, stop screen), diag.c (hz.test=...), ramdisk.c, kernel.ld
├── hal/          gdt.c idt.c trap.asm syscall.asm iospace.c acpi.c pic.c apic.c timer.c serial.c
│                 cpu.c rtc.c power.c halp.h
├── ke/           ki.h dispatch.c thread.c wait.c timer.c dpc.c ketest.c trap.c spinlock.c time.c
│                 symbols.c debug.c bugcheck.c ctxswap.asm
├── mm/           mi.h pfn.c paging.c sysva.c vad.c stack.c pool.c zero.c mminit.c mmtest.c
│                 usercopy.asm usercopy.c mmsvc.c
├── ob/           obp.h object.c handle.c namespace.c obsvc.c
├── ps/           psp.h pe.h peimage.c peload.c process.c thread.c pssvc.c pstest.c
├── io/           iop.h iomgr.c (objects, IRPs, mounting, drive letters) iofile.c (kernel file API)
│                 iosvc.c (file services) iopipe.c (anonymous pipes, M9) iotest.c
├── fs/           ustarfs/ (read-only USTAR, the live C:), fat32/ (fatp.h fat32.c fatdir.c fattable.c)
├── ex/           worker.c event.c sysinfo.c power.c svctable.c
├── rtl/          string.c format.c time.c cxxrt.cpp (string.c and format.c also go into hzrt)
├── win32k/       w32p.h window.cpp (windows, z-order, surfaces) queue.cpp (messages, timers)
│                 input.cpp (cursor, hit test, routing) compose.cpp (compositor) usersvc.cpp
│                 (services) w32init.cpp w32test.cpp; rgn/ Rust region crate
├── drivers/      one directory per driver: bootvid pci disk ahci i8042 ramdisk
├── user/         include/ (hzsys.h, hzuser.h, hzmath.h, hzcore.h, hzui.h, hzfile.h, hzapps.h);
│                 lib/hzsys (syscall stubs), lib/hzrt (CRT, heap, hzcore, hzfile, hzapps),
│                 lib/hzuser (win32k stubs, windows, drawing, fonts), lib/hzui (painter element
│                 window controls textbox listview menu editor dialogs); shell/ (shell.exe: shell
│                 taskbar flyouts); apps/<name>/ (one program per directory: browser calc calendar
│                 clock files notepad paint photos settings setup stopwatch sysinfo taskmgr
│                 terminal)
├── tools/        host/build tools (fontgen, mksym.py)
├── rootfs/       files copied verbatim into the system volume (C:\ layout); Nexora/Apps/*.app
│                 are the program manifests (M9)
├── assets/       original artwork (logo SVG; wallpapers are procedural)
└── third_party/  limine-protocol/limine.h, stb/stb_truetype.h, inter/*.ttf, jetbrains-mono/ (vendored)
```

Every directory listed in `KERNEL_SRC_DIRS` is compiled into the single kernel image, so adding a
`.c`/`.cpp`/`.asm` file there is enough to build it.

## 4. Boot

- **Loader:** Limine 11.4.1 (binary tag pinned to its commit in `scripts/versions.env`). UEFI
  only. The ISO is a hybrid (El Torito EFI image + GPT/MBR protective label) built with xorriso.
  The installed system (M10) puts Limine's `BOOTX64.EFI` at `\EFI\BOOT\` on the ESP.
- **Protocol:** Limine base revision **6** (current, non-deprecated). All requests sit between
  the start/end markers in `.limine_requests`, placed first in the image in their own RW
  segment. If the loader does not acknowledge revision 6, we stop with
  `BOOT_PROTOCOL_MISMATCH`.
- **Kernel image:** static, non-relocatable ELF64 at `0xffffffff80000000` (the `-mcmodel=kernel`
  window). The linker script creates R, RX and RW segments aligned to 4 KiB (W^X), keeps
  `.init_array` for C++ constructors, and discards unwind tables. Symbols: `__KernelStart`,
  `__TextStart/End`, `__InitArrayStart/End`, `__KernelEnd`.
- **Entry:** `KiSystemStartup` (NASM) switches to a 64 KiB boot stack in `.bss`, zeroes `rbp`
  (the stack-walk terminator) and calls `KiInitializeSystem`.
- **Loader block:** `init/bootinfo.c` is the only file that includes `limine.h`. It copies the
  responses into `KeLoaderBlock` (`LOADER_BLOCK`, NT's "loader parameter block"), including
  **copies of every string**, because responses live in bootloader-reclaimable memory that the
  memory manager will reclaim in M2.
- **Ramdisk:** USTAR archive loaded as a module with string `ramdisk`. It mirrors the system
  volume: `Nexora\System\...` (see §9). `init/ramdisk.c` reads it (case-insensitive, `\` or `/`).
- **Install image (M10):** the two Live entries also load `install.tar` (string `install`): the
  boot loader, kernel and symbols laid out as on an ESP (`EFI\BOOT\BOOTX64.EFI`, `boot\...`),
  which Setup copies (§35). Installed systems boot from the menu Setup writes to the ESP, with
  `hz.system=<partition GUID>` naming the system partition.
- **Symbols:** a second module with string `symbols` (`/boot/hzkrnl.sym`) is generated after
  linking by `tools/mksym.py` from `llvm-nm -n -C`. It holds code symbols only, demangled for C++ and
  Rust, with Rust hashes stripped. Using a module avoids a two-pass link. `KeLoadSymbols` rejects
  a table that does not resolve `KeLoadSymbols` to itself, which catches a stale `.sym`.
- **Command line:** `hz.system=<GUID>` (M10) makes the GPT partition with that unique GUID the
  system volume (C:). `hz.log=trace|warning` sets the log level. `hz.test=<name>` runs a diagnostic
  from `init/diag.c`: `bugcheck rustpanic pagefault divide invalidop gpf doublefault assert irql`
  (M1), `stackoverflow poolcorruption doublefree writero nxexec` (M2), `threadoverflow waitirql
  mutexowner` (M3), `smap shutdown reboot` (M4/M5; the power tests run after a complete boot).
  `hz.input=echo` logs every keyboard and mouse event. The *Diagnostics* directory in
  `limine.conf` has one entry per test, plus a verbose-log entry.
  Limine macros (`${KERNEL}`, `${RAMDISK}`, `${SYMBOLS}`) keep the entries short.

## 5. Memory layout (virtual)

| Range | Use | Milestone |
|---|---|---|
| `0x0000000000000000 – 0x00007fffffffffff` | user space (per process): first 64 KiB never mapped; images at their preferred base (`0x140000000` for our programs) or relocated; other ranges first-fit from `0x10000000` | M4 |
| HHDM base (Limine-chosen, `0xffff800000000000` today) | direct map of all RAM | M0 (read from loader block, never hard-coded) |
| `0xffffc00000000000 – +256 GiB` (PML4 slot 384) | **system VA**: multi-page pool blocks, kernel stacks (with guard pages), `MmMapIoSpace`, kernel VADs; later sections and win32k surfaces | M2 |
| `0xfffffe0000000000 – +1 GiB` (PML4 slot 508) | HAL I/O-space window: uncached MMIO (LAPIC, I/O APICs, HPET) | M1 |
| `0xffffffff80000000 – ` | kernel image | M0 |

The PFN database has no VA region of its own: it is one physically contiguous run reached
through the direct map (see §18). The first 64 KiB of user space is never mapped.

The framebuffer is mapped write-combining (PAT entry 5) in the direct map. Boot video therefore never
reads it back: antialiased drawing blends against a known background colour instead.

**MMIO is not in the direct map.** Limine's HHDM (base revision 6), and ours after M2, cover only
RAM, ACPI, framebuffer and "reserved-mapped" ranges. Mapping MMIO holes as write-back would invite
speculative accesses to devices. Device registers are mapped explicitly:
- **Before the memory manager:** `HalMapIoSpace` (hal/iospace.c) maps into the HAL window (PML4
  slot 508), UC (PCD|PWT = PAT 3) and NX, taking page-table pages from a 16-page static pool. The
  M2 page tables copy slot 508 unchanged, so those mappings survive the CR3 switch.
- **After it:** `MmMapIoSpace(phys, size, MM_CACHE_UNCACHED | MM_CACHE_WRITECOMBINE)` in system
  VA. RAM must only be mapped write-back (cache 0); mixed-type aliases are unsupported on x86.

## 6. Debugging and crashes

- **COM1** at 115200 8N1 is the primary log. QEMU puts it on stdio; VirtualBox writes
  `serial-com1.log` in the VM folder. Presence is decided by the scratch register (synchronous
  everywhere); the loopback echo is not used for that because VirtualBox delivers it
  asynchronously. Without a UART, output is dropped, so a VM without COM1 cannot hang the kernel.
- **Log API:** `DbgPrintEx(level, ...)` with ERROR/WARNING/INFO/TRACE (`DbgPrint` = INFO). Serial
  lines get an uptime stamp `[    1.234567]` once the TSC is calibrated, and `WARNING:`/`ERROR:`
  tags. A spinlock taken at HIGH_LEVEL serializes output from threads and ISRs. Messages up to
  INFO also go to the on-screen boot log (`InbvDisplayString`). Bug checks bypass the lock and
  write to COM1 directly.
- **Bug checks:** `KeBugCheckEx(code, p1..p4)`, `KeBugCheckWithMessage`, `HZ_ASSERT`,
  `KeBugCheckTrap` (from exceptions, with the `KTRAP_FRAME`) and `KeBugCheckRustPanic` all funnel into
  one routine. It disables interrupts, raises to HIGH_LEVEL and guards against nesting (a second
  bug check logs both codes to serial and halts). It walks the `rbp` chain, starting at the
  faulting RIP and interrupted RBP for traps; every language is built with frame pointers. Each
  frame is checked against the live page tables before it is read, so a corrupt chain ends the
  walk instead of faulting. The kernel is built with `-fno-optimize-sibling-calls`, so tail calls
  do not erase frames from traces. Return addresses are symbolized through `address - 1`. The routine
  writes a full report (registers, CR2/CR3, symbolized trace) to COM1 and paints the stop
  screen: logo, stop code, message, parameters, a register grid for traps, and the stack trace.
  Monospace text uses JetBrains Mono. Layout is compact below 900 px height, so 1024×768 fits.
  Codes live in `include/bugcodes.h`, and the numbering is our own.
- **Serial markers** such as `[HZ] BOOT OK` and `*** STOP 0x...` are a stable interface used by
  `scripts/smoke-test.py`.

## 7. Conventions

- NT-style names: `Hal*`, `Ke*`, `Mm*`, `Ob*`, `Ps*`, `Io*`, `Ex*`, `Rtl*`, `Inbv*`/`Vid*`
  (boot display/video), `Bi*` (boot info), `Rd*` (ramdisk), `W32k*`/`Rgn*`
  (win32k). Internal routines use the `Ki`/`Mi`/`Iop`/`Halp` style (`p` = private).
- Types: `<stdint.h>` fixed-width types; `STATUS` (NT-style signed status, negative = error,
  test with `HZ_SUCCESS`). Structs are `UPPER_CASE` typedefs with `_UPPER_CASE` tags.
- Headers in `include/` are the public interface of a component; every header is valid C and C++
  (`HZ_EXTERN_C_BEGIN/END`).
- Every milestone ends with `make test` passing and a VirtualBox boot.

## 8. Graphics and windowing (decisions; details in §26–§34; installation in §35)

- **Split of responsibilities.** win32k (kernel, C++) owns windows, z-order, per-thread message
  queues, input routing, activation/capture, window surfaces and **composition**. Window
  *content* (caption, controls, text, icons) is rendered in user mode into the window's surface,
  a section mapped into both the process and system space. This mirrors how modern NT moved
  rendering and font rasterization out of the kernel (DWM, DirectWrite, font driver host). It is
  also forced by the no-FP kernel rule and it limits the damage a font-parsing bug can do.
- **Top-level windows only.** There are no child windows: a window is one surface, and the
  toolkit (M7) draws its controls inside it and does its own hit testing. This keeps win32k
  small (no window tree, no per-child clipping) and matches how modern toolkits work anyway.
  Popups (menus, tooltips) are separate top-level windows with `HZ_WS_POPUP`.
- **No display driver object.** The compositor owns the Limine/GOP framebuffer directly
  (`KeLoaderBlock.Framebuffer`); there is no `\Device\Display0`, because nothing else may draw
  on the screen once the desktop is shown and GOP offers no mode switching after boot. A real
  display driver (mode setting, VirtualBox SVGA) would sit behind the compositor's two calls
  (back buffer size, flush rectangle).
- **Non-client area by metrics.** The kernel hit-tests the caption (32 px), its buttons (46 px
  each from the right: close, maximize, minimize) and the resize borders (6 px) from fixed
  metrics in `uapi/hzuser.h`, moves windows itself, tells the owner which caption button is hot
  or pressed, and leaves drawing the caption to hzuser. Resizing, maximizing and restoring are
  requests (`HZ_WM_SIZE_REQUEST`) that the owner applies, because they replace the surface.
- **Controls live in user mode.** The toolkit (hzui, §31) draws every control, does its own hit
  testing, focus and keyboard navigation inside a window, and uses popup windows for menus and
  tooltips. win32k supplies only what one process cannot do alone: explicit mouse capture (a
  menu sees the click outside it), cursor shapes, window states and the caption buttons.
- **Fonts:** Inter (OFL) through stb_truetype in user mode, grayscale AA, with a per-font glyph
  cache. The kernel keeps its build-time glyph bitmaps for boot and stop screens only.
- **Input:** PS/2 keyboard (scan set 1 → virtual keys → US layout) and PS/2 mouse. win32k routes
  keyboard input to the active window's thread and mouse input to the window under the cursor,
  the capture window, or a non-client drag.
- **Theme:** dark. Mica windows show a blurred, tinted wallpaper through transparent pixels;
  ordinary windows are opaque; `HZ_WS_TRANSPARENT` windows (M8) blend per pixel over whatever
  is below them (the lock screen), and `HZ_WS_NOFRAME` windows are square, borderless and
  shadowless (the taskbar).
- **The shell is an ordinary program with one privilege (M8).** `shell.exe` runs in user mode
  like any other process. The process that registers a window with `RegisterShell` becomes the
  shell until that window goes away: it alone may set the work area and the wallpaper and show,
  minimize or restore other processes' windows; it hears about app windows (created, destroyed,
  activated, changed) through `HZ_WM_SHELLNOTIFY`, and the logo key and Alt+Tab are routed to it.
  Without a shell the kernel keeps its own Alt+Tab, so tests and diagnostics need no shell.
- **System clipboard in win32k (M8):** text only, up to 64 KiB, shared by every process.
- **Wallpapers are procedural (M8):** three palettes of the same kernel generator (dusk, blue
  hour, dawn); there is no image decoder yet.

## 9. Volumes and paths

- Live session: the ramdisk is the system volume. Installed system (M10): a GPT disk with a
  100 MB ESP (FAT32: Limine, `hzkrnl.elf`, `hzkrnl.sym`, `ramdisk.tar`, `limine.conf`) and a
  FAT32 system partition mounted as `C:` (read-write). Limine reads only FAT/ISO9660, so
  everything it loads stays on the ESP. The live image is still loaded on installed systems: it
  stays mounted at `\Device\Ramdisk0` without a letter (a read-only copy of the system files),
  and the second menu entry starts it as C: for repairs.
- Layout of `C:`: `\Nexora` (system root, like `%SystemRoot%`), `\Nexora\System` (executables),
  `\Nexora\Apps` (manifests), `\Nexora\Fonts`, `\Nexora\Licenses`, `\Nexora\Settings`
  (`shell.ini`: first-boot flag, user name, wallpaper, recent programs; M10), `\Users\<name>\Documents`.
- Object namespace (M5): `\Device\HarddiskN\DR0` (whole disk; `Partition0` links to it),
  `\Device\HarddiskN\PartitionM`, `\Device\Ramdisk0`, `\Device\Keyboard0`, `\Device\Mouse0`,
  `\Driver\<name>`, `\FileSystem\<name>`, `\??\X:` → storage device symbolic links.
- Drive letters (M5, M10): storage registers as *system* (the `hz.system` partition), *live* (the
  ramdisk) or *hidden* (EFI system partitions, the install image). `C:` is the system partition
  if one was named and mounts, else the live image (with a warning if a system partition was
  named); every other mountable volume gets the next letter from `D:` in discovery order (AHCI
  port order, then partition order). Hidden volumes and a live image that is not C: have no letter
  but mount on demand through their device paths. User paths are `X:\dir\file` (UTF-8,
  case-insensitive) or object paths starting with `\`; there is no current directory yet.

## 10. Drivers

Drivers are compiled into the kernel image (no loadable `.sys` yet). Each driver declares itself
with `IO_DECLARE_BUILTIN_DRIVER(symbol, "\Driver\Name", Entry, Order)`, which places a record in
the `.hzdrivers` section. `IoInitializeDrivers` (Phase 1) calls the entries in Order:
- `IO_ORDER_BUS` (PCI);
- `IO_ORDER_FILE_SYSTEM` (UstarFs, FAT32 register themselves);
- `IO_ORDER_CLASS` (disk);
- `IO_ORDER_DEVICE` (AHCI, NVMe, xHCI bring-up, Ethernet, i8042, ramdisk).

The built-in NVMe driver supports PCI class 01:08:02. It creates admin and I/O queues, identifies
the first active namespace, and submits sector transfers through PRP-based DMA and a bounded
contiguous bounce buffer. MSI is not required for boot storage. The installer and installed OS
share this driver; the `install-nvme` smoke case checks setup discovery, partition creation,
installation, and readback. Multiple namespaces, USB storage, RAID, and physical-machine
validation remain outstanding.

Each entry creates its device objects and fills its IRP dispatch table (§22). Loadable PE
drivers can be added later without changing the driver model. The PCI inventory is exposed to
Settings through `HZ_SYSTEM_PCI_DEVICE_INFORMATION_CLASS`; it reports the actual initialization
state for known drivers. Operational storage support currently includes AHCI/SATA and NVMe; Intel
82540EM and QEMU-tested 82574L Ethernet controller paths are supported; Intel 82579LM is identified
but has no operational driver. The xHCI driver performs firmware handoff, runs polled command/event
rings, and supports up to eight device slots with EP0 enumeration and boot-HID interrupt endpoint
configuration. A QEMU test enumerates a USB keyboard and tablet on separate ports and verifies a
keyboard event reaches the shared input path. USB MSC BOT now provides 512-byte SCSI READ(10) and
WRITE(10) through the block-device layer; `xhci-storage` verifies FAT32 writes and host-side readback
on a QEMU USB 2 disk. USB mouse event handling is implemented but untested. UASP, USB 3.x,
hubs/hotplug, generic HID, Setup integration, and physical hardware coverage remain outstanding.
Windows `.sys` binaries are not loadable: they require the Windows kernel ABI, driver framework,
interrupt, DMA, and security contracts, none of which this I/O manager implements.

## 11. Multiprocessor (designed in from M1, enabled in M11)

From M1 on, per-CPU data is reached through the `GS` base (`KPRCB`), there are no global
"current thread" variables, spinlocks are real atomic locks that raise IRQL, the LAPIC timer is
per CPU, and TLB invalidation goes through one routine. M11 turned these on: every processor
Limine reports (up to `KI_MAX_PROCESSORS` = 8) runs threads. Details in §36.

## 12. User mode (M4+)

- Nexora PE32+ applications are built with `clang --target=x86_64-pc-windows-msvc` (Microsoft x64 ABI) and
  `lld-link /nodefaultlib /dynamicbase /nxcompat /entry:HzProcessStartup`, default image base
  `0x140000000`. The loader relocates them when that range is taken (or when forced, which the
  self-test does). No stack probes (`-mno-stack-arg-probe`): stacks are fully reserved up front.
- A foreign Windows image, when its imports/features are supported, starts at its PE entry point
  with Nexora's custom `HZ_PEB` pointer in `rcx`; this is not a Windows PEB/process-parameter ABI
  and does not satisfy the standard CRT startup contract.
- Nexora's own libraries are linked statically. Foreign Windows images use a small built-in
  import-thunk surface; imported Windows DLLs are not loaded or searched from disk:
  - `hzsys`: one NASM stub per system service (`mov r10, rcx; mov eax, n; syscall; ret`), numbered
    by position in the same order as `enum HZ_SYSTEM_SERVICE`; NASM and a C++ `static_assert`
    check the count.
  - `hzrt`: `HzProcessStartup` (rcx = PEB → split command line → global constructors →
    `main(argc, argv)` → `atexit` handlers → `NtTerminateProcess`), the thread thunk
    (`HzCreateThread`: rip = thunk, rcx = routine, rdx = parameter; the routine's return value
    becomes the exit status), `HzPrint`, `_fltused`, `_purecall`, the heap (M6, §30), the
    out-of-line parts of hzcore (M7, §31), plus `rtl/string.c` and `rtl/format.c` compiled for the
    MS ABI.
  - **Global constructors (M7):** the compiler puts a pointer to each translation unit's
    initializer in `.CRT$XCU`; the linker sorts `.CRT$XC*` by name, so hzrt's markers in
    `.CRT$XCA` and `.CRT$XCZ` bracket them. Programs are compiled with
    `-fno-threadsafe-statics` (function-local statics are not guarded against other threads).
  - `hzuser` (M6, §30): the window API, drawing and fonts, linked as an archive
    (`build/user/hzuser.lib`) so programs without windows do not carry stb_truetype; `hzui`
    (M7, §31) is the toolkit archive over it. The window API is our own, not
    Win32-binary-compatible.
- The current Windows import surface is limited to `kernel32.dll`/`kernelbase.dll`:
  `GetStdHandle`, synchronous `WriteFile`, `ExitProcess`, `GetLastError`, `SetLastError`,
  `CloseHandle`, `GetCurrentProcess`, `GetCurrentThread`, `GetCurrentProcessId`,
  `GetCurrentThreadId`, `QueryPerformanceCounter`,
  `GetSystemTimeAsFileTime`, `InitializeSListHead`, `GetModuleHandleA/W` (current image only),
  `GetTickCount`, `GetTickCount64`, `GetCommandLineA`, `GetProcessHeap`,
  `HeapAlloc`, and `HeapFree`. QPC is reported in microseconds (1 MHz); `GetCommandLineA` returns
  the command line in Nexora's custom PEB. The heap shim tracks whole page-backed allocations up
  to 64 MiB; it is not a compatible implementation of the Windows heap manager.
  Unsupported DLLs/APIs fail closed and the terminal/desktop can query a bounded compatibility
  report. This does not provide CRT startup, DLL loading, Windows exception/unwind support, or
  `user32`/`gdi32` compatibility.
- User code may use SSE/x87 (the kernel saves user FPU state with XSAVE/FXSAVE on context
  switch); `user/include/hzmath.h` provides the few math functions there is no libm for.
- The user/kernel ABI is `include/uapi/hzapi.h` (service numbers and argument contracts, access
  rights, `MEM_*`/`PAGE_*`, TEB/PEB, information structures). It uses fixed-width types only, so
  both ABIs (SysV kernel, MS user) lay structures out identically. Names and status values are
  NT-style; the API is not binary compatible with Windows.

## 13. Build and toolchain

- Pinned in `scripts/versions.env`: LLVM 22.1.8 (sha256-verified tarball), Rust 1.98.1 (+
  `x86_64-unknown-none`), Limine 11.4.1 (commit-verified), plus the vendored sources.
- `scripts/setup-toolchain.sh` installs them into `~/.nexora-os-toolchain` **without root**
  (static LLVM binaries, only the needed tools: ~750 MB). The Dockerfile runs the same script,
  so the Docker and native builds use identical compilers.
- `scripts/build.sh` uses Docker when it is available, otherwise the native toolchain.
- Kernel Rust is built with `-C code-model=kernel -C relocation-model=static
  -C force-frame-pointers=yes` and linked as `libhorizon_rgn.a` into `hzkrnl.elf` with
  `--gc-sections`.

## 14. Visual identity

- Name: **Nexora OS**. Logo: the rounded N mark in `assets/branding/nexora-os-logo.svg`,
  rendered in the boot splash by `VidDrawLogo` and as a compact line icon on the taskbar.
- Fonts: Inter 4.1 (Regular, Medium, SemiBold, Display SemiBold vendored) for UI; JetBrains
  Mono 2.304 Regular for register dumps, traces and (M9) the Terminal.
- Boot palette: night `#10122A → #07080F`, text `#F4F5FB`, secondary `#9AA2C8`. Stop screen:
  ember `#2A0E17` with accent `#FF9E5E`. It is deliberately not blue.
- Desktop (M6–M7): the "Nexora dusk" wallpaper, dark Mica windows, and the accent colour
  Nexora orange `#FF9F5A` for selected and primary controls (dark text on it).

## 15. Processor tables and traps (M1)

- **GDT** (`hal/gdt.c`, fixed order for SYSCALL/SYSRET): `0x00` null, `0x08` kernel code,
  `0x10` kernel data, `0x18` user data (RPL 3 → `0x1B`), `0x20` user code 64-bit (→ `0x23`),
  `0x28` TSS (two slots). M4 will program STAR = `[47:32] 0x08`, `[63:48] 0x10`.
- **TSS:** RSP0 = kernel stack for ring 3 entries (`HalSetKernelStack`; the scheduler updates it
  in M3). IST1 = double fault, IST2 = NMI, IST3 = machine check, each with a 16 KiB static stack,
  so those three work even when the current stack is gone. The `hz.test=doublefault` diagnostic
  proves it with RSP = 0x1000.
- **Per-processor block:** `KPRCB` is reached through the GS base (`gs:[0]` = self). It holds the
  CPU number, APIC ID, TSS, interrupt and exception counters and, from M3, the current thread.
  `KERNEL_GS_BASE` is 0 until user mode exists. The trap stub already `swapgs`es when CS has RPL 3.
- **IDT:** 256 interrupt gates (IF cleared on entry) to stubs generated by a NASM `%rep`. `int3`
  is DPL 3.
- **Trap path:** each stub pushes a zero error code where the CPU does not, then the vector.
  `KiTrapCommon` saves the 15 GPRs into a `KTRAP_FRAME` (layout shared with `include/ke.h`,
  checked by a static assert). It links a fake frame `{interrupted rbp, interrupted rip}` so rbp
  walks continue into the interrupted code, and calls `KiDispatchTrap`. The stack is 16-byte
  aligned at the call.
- **Exceptions in kernel mode are fatal** except `int3` (logged at TRACE, execution resumes).
  #PF → `KERNEL_PAGE_FAULT` (CR2, error code, RIP), with a message such as "Page fault writing
  0x… (page not present) in Func+0x12". #DF → `DOUBLE_FAULT`, NMI → `NMI_HARDWARE_FAILURE`,
  #MC → `MACHINE_CHECK_EXCEPTION`, anything else → `UNEXPECTED_KERNEL_TRAP` (vector, error, RIP,
  RSP). User-mode exceptions become process termination in M4.

## 16. IRQL and interrupts (M1)

- **IRQL is CR8** (the APIC task priority), so hardware masks by priority class (`vector >> 4`).
  Levels: PASSIVE 0, APC 1, DISPATCH 2, device 3–11, CLOCK 13, IPI 14, HIGH 15.
  `KeRaiseIrql` / `KeLowerIrql` bug check on the wrong direction (`IRQL_INVALID_TRANSITION`).
- **Vector map:** 0x00–0x1F exceptions · 0x20–0x2E legacy PIC range (remapped, fully masked,
  outputs disconnected) · **0x2F DISPATCH software interrupt** (M3) · 0x30–0xBF device vectors
  (IRQL 3–11; 0x40 is the PIT self-test) · 0xD1 clock · 0xE0–0xEF IPIs (M11) · 0xFE APIC error ·
  0xFF APIC spurious (never EOI'd).
- **SYNCH_LEVEL = 12** guards the dispatcher database: above every device IRQL, below the
  clock. ISRs never take the dispatcher lock; they queue DPCs.
- **Dispatch:** `KiDispatchInterrupt` raises CR8 to the vector's IRQL and re-enables interrupts
  (higher classes may nest). It calls the service routine registered with `KeConnectInterrupt`,
  then disables interrupts, sends EOI and restores the previous IRQL. Vector sharing and IRQ
  allocation come with the I/O manager (M5).
- **Spinlocks** (`ke/spinlock.c`): real atomic TTAS locks that raise IRQL (`KeAcquireSpinLock` →
  DISPATCH; `KeAcquireSpinLockRaiseToLevel` for locks shared with ISRs). A spin beyond 2^28
  iterations is treated as a deadlock (`SPIN_LOCK_TIMEOUT`).
- **Controllers** (`hal/apic.c`, `hal/pic.c`): the 8259s are remapped to 0x20–0x2F and fully
  masked. The Local APIC works in xAPIC (MMIO) or x2APIC (MSR) mode, whichever firmware left
  enabled. SVR has the spurious vector 0xFF, LINT0/1 come from the MADT NMI entries, and the error
  LVT is 0xFE. Each I/O APIC is mapped and all its inputs are masked. Drivers unmask via
  `HalEnableSystemInterrupt(gsi, vector, activeLow, level)` or `HalEnableIsaInterrupt(irq,
  vector)`, which applies MADT interrupt source overrides (IRQ 0 → GSI 2 on both VMs). Delivery is
  fixed mode, physical destination = BSP.
- **ACPI tables** (`hal/acpi.c`): RSDP → XSDT (RSDT for ACPI 1.0) → every table
  checksum-verified. The HAL records the MADT facts, the FADT (PM timer port and width, reset
  register, century register, 8042 flag, SCI) and the HPET base in `HalAcpi` (`include/acpi.h`).
  AML is not interpreted.

## 17. Timers and time (M1)

- **Frequencies, in order of preference:**
  1. **Hypervisor timing leaf** CPUID `0x40000010` (EAX = TSC kHz, EBX = APIC bus kHz; the LAPIC
     timer runs at bus/16). VMware, VirtualBox with the "Minimal" paravirt provider (which
     `create-vbox-vm.sh` sets) and KVM with `vmware-cpuid-freq` publish it. It is exact and needs
     no calibration time.
  2. **Calibration against a reference timer:** HPET (QEMU q35), else the ACPI PM timer
     (VirtualBox, which has no HPET unless `--x86-hpet on`), else PIT channel 2. It measures TSC
     and LAPIC (÷16) ticks over 10 ms windows **until three consecutive windows agree within
     0.5 %** (at most 25 windows) and averages them.
- **Why "until stable":** hypervisors let their virtual clock (PM timer, HPET, LAPIC timer) run
  fast for a while after boot to catch up with real time. In VirtualBox the first calibration
  window measured 0.2–0.7 ms of TSC time for "10 ms" in 5 of 6 boots. A fixed median-of-3 then
  mis-calibrated the TSC by up to 20 % in about 1 boot in 3. With the stable-window rule, 4–6
  windows are used and calibrations agree within 0.1 %.
- **Two clock domains:** the TSC is real time; the LAPIC tick count follows the hypervisor's
  virtual clock. Early after boot they can differ by several percent (VirtualBox: tick rate
  1.02–1.05× the TSC during the first ~0.3 s). Consequences:
  - Uptime, timestamps and stalls use the TSC.
  - **Timer deadlines (M3) are TSC-based; the clock tick only triggers expiry checks**, so a fast
    or slow tick never shifts a due time.
  - The clock self-test compares ticks against the reference timer and logs the TSC-relative
    rate. It retries the 200 ms window up to three times (M6), because a descheduled virtual CPU
    loses coalesced periodic interrupts. The accepted range is 65–110 % of the requested rate:
    some virtual-machine backends deliver periodic interrupts substantially below real time even
    when the system remains usable; a rate outside that range still marks boot degraded.
- **Clock interrupt:** LAPIC timer periodic at **1000 Hz** on vector 0xD1 (CLOCK_LEVEL).
  `KiClockInterrupt` counts ticks, requests the DISPATCH interrupt when the running thread's
  quantum (in TSC cycles, §19) is used up, and queues the timer DPC when the earliest timer is
  due. It takes no locks.
- **Time bases:** `KeQueryTickCount` (clock ticks), `KeQueryUptimeMicroseconds` (TSC since
  kernel entry, `KiBootTsc`), `KeStallExecutionProcessor` / `HalStallExecution` (TSC busy-wait;
  the reference timer before calibration). There is no 128-bit division: the kernel links no
  compiler-rt, so conversions split into quotient and remainder.
- **Boot self-tests** (non-fatal, reported as warnings): `int3` round trip; clock rate vs TSC
  (±10 %, measured against the reference timer); I/O APIC delivery (PIT channel 0 at 1 kHz through ISA IRQ 0 for 50 ms, expect about
  50 interrupts).
- **Lesson from VirtualBox:** emulated timer devices (the PIT, and generally device models) are
  serviced on VM exits. A tight `rdtsc`/`pause` loop causes none, so a busy-waiting guest starves
  them: the PIT self-test saw 3 interrupts instead of 50. Anything that waits for a device
  interrupt must wait with `hlt` or block (M3 scheduler), never spin. Busy stalls are only for
  short hardware delays.

## 18. Memory manager (M2)

- **Bring-up order** (`mm/mminit.c`): PFN database (still on Limine's page tables) → system VA
  allocator → own page tables and CR3 switch → reclaim bootloader memory → pool. It runs right
  after the boot display and symbols, before the HAL maps any device. Global C++ constructors run
  earlier and must not allocate (the pool would bug check with `BAD_POOL_CALLER`, reason 5).
- **PFN database** (`mm/pfn.c`): one 16-byte `MMPFN` per page up to the highest RAM address
  (≈ 8 MiB per 2 GiB), carved from the largest usable range and reached through the direct map.
  States: reserved, free, zeroed, active, page table, pool, kernel image, PFN database, loader
  reclaimable. Free and zeroed pages are doubly linked lists threaded through the entries by
  32-bit index (16 TiB limit). Pages below 1 MiB stay reserved (for SMP trampolines in M11).
  Other allocations:
  - `MmAllocatePhysicalPage(zero)` zeroes on demand (a zero-page thread can refill the zeroed
    list from M3).
  - `MmAllocateContiguousPages(count, maxPhys, alignment)` scans for DMA runs (AHCI in M5).
  - Freeing a page that isn't allocated is a `PFN_LIST_CORRUPT` bug check.
- **Kernel page tables** (`mm/paging.c`): built from scratch, never inherited:
  - kernel image per section (`.text` RX, `.rodata` R+NX, data R/W+NX), with frames looked up in
    the boot tables;
  - direct map of RAM ranges only (same HHDM offset as Limine, 2 MiB pages where aligned, NX,
    global);
  - framebuffer WC;
  - HAL window copied.

  **All 256 kernel-half PML4 entries are allocated up front** (1 MiB), so a new address space
  copies them once and kernel mappings never need cross-address-space synchronization. After the
  switch: CR4.PGE, plus CR4.SMEP and CR4.SMAP when CPUID reports them (QEMU TCG yes, VirtualBox
  no). With SMAP, only the user-copy routines (§21) can touch user pages. PAT entries 0–5 are
  verified to be Limine's layout (WB WT UC- UC WP WC) and reprogrammed if not.
- **Reclaim:** bootloader-reclaimable pages (Limine's page tables, responses and stack) are freed
  after the switch: 27–37 MiB on the reference VMs. Kernel image and modules (ramdisk, symbols)
  and ACPI tables are kept.
- **System VA** (`mm/sysva.c`): sorted free-extent array, first fit, merge on free. Every
  allocation is followed by one unmapped page.
- **Address spaces:** `MMADDRESS_SPACE` = PML4 physical address, spinlock and VAD list.
  - `MmCreateAddressSpace` copies the kernel half.
  - `MmDeleteAddressSpace` releases VADs, then the user-half page tables.
  - The current address space belongs to the running thread (`KTHREAD.AddressSpace`, initially
    its process's), and the context switch loads it into CR3. `MmSwitchAddressSpace` attaches
    the current thread to another space, and the attachment survives preemption.
- **VADs and demand zero** (`mm/vad.c`): `MmAllocateVirtualMemory(as, &base, size,
  MEM_RESERVE|MEM_COMMIT, prot)` records a range; committed pages are materialized on first touch
  by `MmAccessFault` (called from the #PF handler) as zeroed pages with the VAD's protection.
  - Windows semantics (M4): `MEM_COMMIT` with an address commits inside an existing reservation
    and splits its VAD; every piece keeps the reservation's `AllocationBase`, and
    `MmFreeVirtualMemory(base)` releases all pieces together. `MmProtectVirtualMemory` splits the
    same way and rewrites present PTEs; `MmQueryVirtualMemory` reports a piece or a free gap as
    `HZ_MEMORY_BASIC_INFORMATION`. `MEM_IMAGE` marks loader-created image ranges. No-access
    protection on committed pages and decommit-without-release are not supported yet.
  - Protection violations, uncommitted or unknown addresses → `KERNEL_PAGE_FAULT` in kernel mode;
    in user mode the process is terminated (§21).
  - Faults at IRQL ≥ DISPATCH → `IRQL_NOT_LESS_OR_EQUAL`, as in NT.
  - A committed-but-not-yet-touched page is never touched at raised IRQL. Pool memory is always
    resident, so it is safe at DISPATCH.
- **Pool** (`mm/pool.c`, API in `include/ex.h`): `ExAllocatePoolWithTag(NonPagedPool, size,
  tag)`, 16-byte aligned, usable at IRQL ≤ DISPATCH.
  - 13 size classes up to 2048 bytes (header included) in slab pages. Slab metadata (free-list
    head, class, in-use count) lives in the page's PFN entry, and the free list threads through
    the block headers.
  - One empty slab per class is cached; the rest go back to the PFN allocator.
  - Larger blocks are whole pages: one page through the direct map, or several mapped in system
    VA.
  - Each 16-byte header has tag, requested size, class, flags and a checksum that includes the
    header address. A bad checksum → `BAD_POOL_HEADER`; double free, tag mismatch, bad IRQL or
    non-pool pointer → `BAD_POOL_CALLER`, both with a readable message.
  - Freed small blocks are filled with 0xDD. Per-tag statistics are kept
    (`ExDumpPoolUsage`, `hz.log=trace`).
  - C++ `operator new/delete` use the pool (tag `Cxx `). A failed plain `new` →
    `INSUFFICIENT_RESOURCES`; nothrow overloads return NULL.
- **Kernel stacks** (`mm/stack.c`): 32 KiB (8 pages) in system VA plus one guard page below. The
  guard PTE is non-present with the `MI_GUARD_PTE` signature. Overflowing into it faults, the
  CPU cannot push the #PF frame onto the same stack, and the resulting #DF (IST1) checks CR2 →
  `KERNEL_STACK_OVERFLOW`.
- **Lock order:** pool → address space → system VA → PFN. Everything is at DISPATCH_LEVEL. VAD
  descriptors are allocated before taking an address-space lock, to keep the order.
- **Self-tests** (`mm/mmtest.c`, every boot, about 7 ms in VirtualBox and 90 ms under TCG):
  - 512 zeroed pages,
  - a 64-page contiguous DMA run,
  - 6000 random pool operations (1 B–12 KB) with pattern checks and no leaks,
  - 16 demand-zero kernel pages,
  - a private address space with user pages at 0x400000: user copies (SMAP path), fault-safe
    failures, commit splitting, protection change, query, release of a split reservation,
  - a guarded kernel stack,
  - a system-VA mapping,
  - a leak check on allocated pages.

  The boot fails as `DEGRADED` if any test fails.
- **Diagnostics:** `hz.test=stackoverflow | poolcorruption | doublefree | writero` (W^X: write
  to `.rodata`) `| nxexec` (NX: execute from `.data`).

## 19. Threads, scheduler and synchronization (M3)

- **Objects:**
  - `KTHREAD`: dispatcher header (signaled on exit), saved RSP, guarded 32 KiB stack, process,
    current address space, state, priority, cycle-based quantum, wait blocks, private timeout
    timer, owned-mutex list, reference count.
  - `KPROCESS`: header, address space, thread list.
  - `KeSystemProcess` (PID 4) owns all M3 threads.
- **Boot:** after timer calibration, `KeInitializeScheduler` turns the boot context (on the .bss
  boot stack) into this CPU's **idle thread** (priority 0, `sti; hlt` loop, never waits). The
  rest of initialization runs in the **Phase1Init** thread (priority 8). After the self-tests it
  starts **ZeroPage** (priority 1: keeps 2048 pre-zeroed pages) and **Status** (priority 9:
  updates the splash every second), then exits.
- **Ready queues:** 32 FIFO queues plus a 32-bit summary mask. The highest non-empty priority
  runs, round robin within it. Static priorities; no boosts or decay yet.
- **Rescheduling is deferred to the DISPATCH software interrupt** (vector 0x2F, LAPIC self-IPI).
  - Readying a higher-priority thread, or the clock seeing an expired quantum, only *requests*
    it.
  - The APIC holds the request until IRQL < DISPATCH_LEVEL, so a switch can never happen while
    a spinlock is held or at raised IRQL.
  - The handler sends EOI **first** (a thread switched away inside it would otherwise leave
    class 2 in service), then retires DPCs, then preempts. A preempted thread goes to the head
    of its queue; an expired one goes to the tail.
- **Voluntary switches** (wait, yield, terminate) call `KiSwapThread` with the dispatcher lock
  held at SYNCH_LEVEL. It picks the next thread (or idle), releases the lock, drops to
  DISPATCH_LEVEL and calls `KiSwapContext` with interrupts disabled. Invariants:
  - every switch happens at DISPATCH_LEVEL;
  - every thread resumes at DISPATCH_LEVEL;
  - new threads start in `KiThreadStartup`, which lowers to PASSIVE.

  `KiSwapContext` saves only callee-saved registers (kernel code has no FP state) and switches
  RSP. C code updates PRCB.CurrentThread, TSS.RSP0 and CR3 (if the address space differs).
- **Quantum = 10 ms charged in TSC cycles** (as NT has done since Vista), not clock ticks.
  Measured on VirtualBox under Hyper-V (NEM backend): only **19 clock ticks arrive in 200 ms
  while the guest spins**, because the LAPIC timer is serviced on VM exits. Tick-counted quanta
  gave 4 context switches instead of about 20. With cycle accounting, each late tick charges the
  real elapsed time: 21-22 switches on both QEMU and VirtualBox. Per-thread CPU time
  (`KeQueryThreadCpuTimeUs`) comes from the same accounting.
- **Termination and reaping:** `KeTerminateThread`:
  - abandons owned mutexes,
  - signals the thread object,
  - unlinks the thread and parks it on the reaper list,
  - switches away.

  The next thread to run, in `KiSwapThread` or `KiThreadStartup`, frees the dead thread's stack
  and calls the thread's `ReleaseRoutine`, which drops the running reference. Since M4 the
  `KTHREAD` is embedded in an `ETHREAD` object, so that reference is an object-manager reference
  (§20); `KeCreateThread` still makes bare pool threads for the Ke self-tests.
- **Waits** (`ke/wait.c`), NT model:
  - `KeWaitForSingleObject` / `KeWaitForMultipleObjects` (WaitAny/WaitAll, up to 3 objects with
    built-in wait blocks, 64 with a caller array).
  - Timeouts in relative us (NULL = forever, 0 = poll) via the thread's private timer, whose
    wait block wakes the thread with `STATUS_TIMEOUT`.
  - Signalers run `KiWaitTestLocked`, which satisfies waiters FIFO and readies them with
    `STATUS_WAIT_0 + index`, `STATUS_ABANDONED_WAIT_0 + index` or `STATUS_TIMEOUT`.
  - Blocking at IRQL >= DISPATCH → `WAIT_AT_RAISED_IRQL`.
- **Objects:**
  - events (notification / synchronization),
  - semaphores (with limit; `STATUS_SEMAPHORE_LIMIT_EXCEEDED`),
  - mutexes (owner, recursion, abandonment; releasing an unowned mutex → `MUTEX_NOT_OWNED`),
  - timers (notification / synchronization, periodic, optional DPC),
  - threads.
- **DPCs** (`ke/dpc.c`): per-CPU queue with a lock at HIGH_LEVEL (queueable from any ISR),
  drained at DISPATCH_LEVEL by the software interrupt.
- **Timers** (`ke/timer.c`): one list sorted by absolute TSC-uptime due time. The clock compares
  against `KiNextTimerDueUs` and queues the expiration DPC, which signals timers, wakes waiters,
  queues timer DPCs and re-arms periodic timers (skipping missed periods).
- **Lock order:** the Kd log lock and Inbv lock (HIGH) are leaves. Dispatcher (SYNCH) → DPC
  queue (HIGH). Pool → address space → system VA → PFN, all taken without the dispatcher lock
  held. The PFN lock is never held while signaling objects.
- **Boot display** drawing is serialized by an Inbv lock at HIGH_LEVEL (threads can be preempted
  mid-draw; the drawing primitives share clip state). The stop screen skips it.
- **Stop screens** show the running thread (name, id) and the IRQL at the time of the stop.
- **Self-tests** (`ke/ketest.c`, about 0.3 s in VirtualBox, 0.45 s under TCG):
  - create/join 8 threads (WaitAll with a caller array),
  - 1000 event ping-pong round trips (about 0.28 us in VirtualBox, 1.6-2 us under TCG),
  - a semaphore bounded buffer (2000 items) plus limit enforcement,
  - mutex recursion and 4-way contention around a yield,
  - time slicing (cycle-based quantum),
  - immediate priority preemption,
  - timers (sleep, timeout, 5 ms periodic DPC at DISPATCH_LEVEL, timer object),
  - WaitAny/WaitAll/poll,
  - abandoned mutex,
  - a leak check (stacks, pool, thread count).
- **Multiprocessor (M11):** the ready queues stay global under the dispatcher lock; a SwapBusy
  handshake and directed reschedule IPIs make that safe on several processors (§36).

## 20. Object manager (M4)

- **Objects** (`ob/object.c`): every executive object is `[OBJECT_HEADER (64 bytes)][body]`. The
  header holds the pointer count, handle count, type, name and parent directory, a signature and
  a deferred-delete link. The public API passes body pointers only.
  - `ObCreateObject(type, size, name, &obj)` returns the body with one reference. A named object
    is inserted into the namespace and is **permanent**: its directory entry holds a reference.
  - `ObReferenceObject` / `ObDereferenceObject` are atomic. The last dereference runs the type's
    delete procedure and frees the memory. It may happen at DISPATCH_LEVEL (the thread reaper
    drops `ETHREAD` references there); deletion then goes through a list drained by the
    **system worker thread** (`ex/worker.c`, priority 12), because deleting a process needs
    PASSIVE_LEVEL.
  - A bad signature, a negative count or a deletion while handles or a name remain →
    `OBJECT_REFCOUNT_CORRUPT`.
- **Types** are objects of type `Type` (bootstrapped as an instance of itself) under
  `\ObjectTypes`. `OBJECT_TYPE_INITIALIZER`: pool tag, valid access mask (`GENERIC_ALL` maps to
  it), `Waitable` (body starts with a `DISPATCHER_HEADER`), delete/close/parse procedures. Types
  today: Type, Directory, SymbolicLink, Driver, Device, File, Process, Thread, Event.
- **Handles** (`ob/handle.c`): per-process table of `{object, granted access}`, growing by doubling
  (up to 16384), free slots on a list. A handle value is `(index + 1) * 4`, so 0 is never valid
  and misaligned values are rejected. Each handle holds a pointer reference.
  `HZ_CURRENT_PROCESS` (-1) and `HZ_CURRENT_THREAD` (-2) are pseudo handles resolved without the
  table. In user mode the granted access must cover the requested access (`STATUS_ACCESS_DENIED`),
  and the type must match (`STATUS_OBJECT_TYPE_MISMATCH`). The last thread of a process closes
  the whole table (`ObKillHandleTable`).
- **Namespace** (`ob/namespace.c`): case-insensitive, `\`-separated. Directories are sorted
  entry lists under one namespace spinlock; names may contain anything printable but `\`.
  `ObReferenceObjectByName` walks one component at a time:
  - a **symbolic link** restarts the walk at its target with the rest of the path appended (16
    reparses at most);
  - an object whose type has a **parse procedure** gets the remainder (with its leading `\`) and
    returns a new object. The ramdisk device does this to open files.

  Standard directories: `\ObjectTypes`, `\Device`, `\Driver`, `\FileSystem`, `\??`, `\KernelObjects`. In the
  live session `\??\C:` links to `\Device\Ramdisk0`. `hz.log=trace` dumps the tree.
- **Parse context:** `ObReferenceObjectByNameEx(path, type, context)` passes an opaque pointer to
  any parse procedure met on the way; the I/O manager passes its open parameters (access,
  disposition, options) this way (§22).

## 21. Processes, system calls and user mode (M4)

- **Objects** (`include/ps.h`): `EPROCESS` starts with `KPROCESS` and `ETHREAD` with `KTHREAD`, so
  both are waitable: a thread is signaled when it terminates, a process when its last thread
  exits. References: the creator; each thread holds its process; a running thread holds itself
  until the reaper has freed its kernel stack (`ReleaseRoutine`). `PspLock` guards process
  thread lists, counts, terminating/exited flags, the active-process list and PID allocation
  (multiples of 4; System = 4).
- **System process and threads:** `PsInitializeSystem` (phase 0, before the scheduler) creates
  the System `EPROCESS` with the kernel address space and a handle table. `KeInitializeScheduler`
  takes its `KPROCESS`. All kernel threads (Phase1Init, Worker, ZeroPage, Status) are
  `PsCreateSystemThread` threads.
- **SYSCALL** (`hal/syscall.asm`, `ke/syscall.c`):
  - STAR selects kernel CS 0x08 / SS 0x10 and user CS 0x23 / SS 0x1B. FMASK clears IF, TF, DF,
    AC, NT and IOPL.
  - The entry does `swapgs`, saves the user RSP in the KPRCB, loads `KPRCB.KernelStackTop` (= the
    current thread's stack top, set by the context switch with TSS.RSP0) and pushes a complete
    `KTRAP_FRAME` (vector 0xFFFF). Then it enables interrupts and calls `KiSystemServiceHandler`.
  - Arguments 1–4 come from r10, rdx, r8, r9 and 5–8 from the user stack at `rsp + 40` (copied
    with `MmCopyFromUser`). The executive registers the service table (`ex/svctable.c`):
    `{routine(const uint64_t *args), argument count, name}`.
  - Return is through the common `KiTrapRestore` path (pop registers, `swapgs`, `iretq`) rather
    than `sysretq`: one exit path for traps, interrupts and system calls, and no
    non-canonical-RCX hazard. Every register except rax (status) is restored from the frame, so
    nothing from the kernel leaks; rcx/r11 come back as the user RIP/RFLAGS.
  - A service returning at IRQL > PASSIVE → `IRQL_GT_ZERO_AT_SYSTEM_SERVICE`. `KTHREAD.PreviousMode`
    is `UserMode` during a service.
- **User buffers** (`mm/usercopy.asm`): `MmCopyFromUser`, `MmCopyToUser` and
  `MmCopyStringFromUser` check that the whole range lies below `MM_USER_TOP`. The copy loops run
  between `MmpUserCopyStart` and `MmpUserCopyEnd`, inside `stac`/`clac` when SMAP is on. A kernel
  page fault in that window that Mm cannot resolve resumes at `MmpUserCopyFault`, which returns
  `STATUS_ACCESS_VIOLATION`. Every service treats pointers as untrusted, and probes output
  pointers before irreversible actions (creating a thread or process).
- **First entry to ring 3:** a user thread's kernel start routine builds a `KTRAP_FRAME` on its
  own kernel stack: rip = start, rcx/rdx = arguments, rsp = stack top − 40 (MS x64 entry state:
  return-address slot plus home area), RFLAGS = IF, CS 0x23, SS 0x1B. `KiEnterUserMode` jumps
  into `KiTrapRestore` with it.
- **Per-thread user state:**
  - **GS base = TEB.** The context switch writes `MSR_KERNEL_GS_BASE`, and `swapgs` exchanges it
    on every kernel entry and exit from ring 3.
  - **Extended state.** User threads own a 64-byte-aligned XSAVE area (XCR0 = x87|SSE|AVX when
    available; FXSAVE on CPUs without XSAVE). The context switch saves the outgoing thread's area
    and restores the incoming one's. Kernel code never touches FPU/SSE (general-regs-only), so
    kernel-only threads need no area, and a user thread's registers stay live across its own
    kernel entries.
- **User stack:** one reservation of `SizeOfStackReserve` (default 1 MiB, clamped to
  64 KiB–16 MiB) plus a guard page at the bottom that is reserved but never committed. The rest is
  committed demand-zero. A fault in the guard page is reported as `STATUS_STACK_OVERFLOW`.
- **TEB/PEB** (one page each, user memory): TEB = self (`gs:[0]`), PEB pointer, PID, TID, stack
  bounds, TLS slots. PEB = image base and size, PID, parent PID, image name, command line.
  Filled through the copy routines while attached to the process.
- **PE32+ loader** (`ps/peimage.c` validates, `ps/peload.c` maps):
  - Validation: AMD64 PE32+ executable; header, section and directory ranges are checked, with the
    certificate/security directory treated as a file offset; image size is capped at 256 MiB and
    sections must be page-aligned.
  - The image is assembled in a pool buffer: headers, then sections (raw data, zero fill). It is
    reserved as `MEM_IMAGE` at the preferred base, or anywhere if that fails or
    `PS_CREATE_FORCE_RELOCATION` is set. `IMAGE_REL_BASED_DIR64` relocations are applied if it
    moved (anything else is rejected).
  - The buffer is copied in with one `MmCopyToUser`. Headers become read-only and each section
    gets its declared protection; writable-executable sections and entry points outside executable
    sections are rejected. The generated import-thunk page is changed from RW to RX after setup.
  - Only the built-in console imports listed in §12 are resolved. Other DLLs/APIs fail closed.
    TLS directories, delay imports, and non-console Windows subsystems are unsupported.
- **Process creation:** `PsCreateProcess(object path, command line, flags)`: open the file
  (`\??\C:\...`) → `EPROCESS` + address space + handle table + PID → map the image → PEB →
  initial thread at the entry point with rcx = PEB. `NtCreateProcess` accepts `C:\...` paths and
  prefixes `\??\`.
- **Termination** uses the user-return callout, standing in for NT's APCs:
  - `PsTerminateProcess` marks the process and every thread, and calls
    `KeRequestUserReturnCallout` on each. That sets `UserReturnPending` and alerts the thread out
    of any alertable wait. All user-initiated waits are alertable.
  - Every return to ring 3 (system-call exit, and interrupt or exception return with a user frame)
    runs `KiCheckUserReturn`. If the flag is set, the callout `PsExitThread`s the thread in its
    own context.
  - A thread spinning in user mode is caught on its next clock interrupt.
  - The last thread to exit closes the handle table, records the exit status and signals the
    process. The address space goes when the process object is deleted.
- **User exceptions** (`ke/trap.c`): demand-zero faults are resolved and `int3` is logged and
  resumed. Everything else becomes an NT status handed to Ps, which logs it and terminates the
  process with it:
  - #PF → `ACCESS_VIOLATION`, or `STACK_OVERFLOW` in the guard page;
  - #DE → `INTEGER_DIVIDE_BY_ZERO`;
  - #UD → `ILLEGAL_INSTRUCTION`;
  - #GP → `PRIVILEGED_INSTRUCTION` when the opcode (read through the copy path) is cli, sti,
    hlt, in/out, mov cr/dr, wrmsr, ..., otherwise `ACCESS_VIOLATION`;
  - #MF/#XM → `FLOAT_INVALID_OPERATION`;
  - #AC → `DATATYPE_MISALIGNMENT`.

  NMI, #DF and #MC stay fatal. Kernel-mode SMEP/SMAP violations have their own stop codes
  (`SUPERVISOR_MODE_USER_EXECUTE`/`_ACCESS`), with a message naming the address and function.
- **System services** (21; numbers and contracts in `uapi/hzapi.h`):
  - Close, DisplayString (to the log and boot console, prefixed `[image:pid]`);
  - Terminate/Create Process and Thread, Query Information Process/Thread (exit status, IDs);
  - Allocate/Free/Query/Protect VirtualMemory (own process only for now);
  - Create/Set/Reset Event;
  - WaitForSingle/MultipleObjects (up to 64; pool wait blocks beyond 3), DelayExecution,
    YieldExecution;
  - QuerySystemInformation (basic, process list), QueryUptime.
- **Self-tests** (`ps/pstest.c`, every boot): the object accounting after Phase 1 has settled -
  every Process/Thread/Event/File object must be back at its baseline and pool usage must be back
  to the starting value. The process program tests that ran here (hello.exe at its preferred base
  and relocated, usertest.exe's 100+ checks, and crash.exe in seven fault modes plus int3) went
  with the beta apps.
- **Known gaps:**
  - No cross-process memory services.
  - No suspend/resume or APCs.
  - An NMI arriving between `syscall` and `swapgs` would run with the user GS base. NMIs are
    fatal anyway; a paranoid GS check comes with SMP.

## 22. I/O manager (M5)

- **Objects** (`include/io.h`, `io/iomgr.c`):
  - `DRIVER_OBJECT`: a name and a `MajorFunction[]` dispatch table.
  - `DEVICE_OBJECT`: driver, type, flags, sector size and count, a VPB for storage devices, and an
    extension allocated with the device.
  - `FILE_OBJECT`: target device (referenced), VPB, `FsContext` (per file, shared by opens),
    `FsContext2` (per open), current byte offset, granted access, flags, and a mutex.

  All are Ob objects. Named devices and drivers are permanent. Volume devices are unnamed and
  referenced by their VPB. Deleting a file object sends `IRP_MJ_CLOSE` to its driver.
- **IRPs are synchronous.** `IoCallDriver(device, irp)` runs the dispatch routine, which returns
  the final status. A driver that waits for hardware blocks inside its dispatch routine: AHCI waits
  for a DPC-signaled event; the keyboard waits alertably for user callers. Major functions:
  CREATE, CLOSE, READ, WRITE, QUERY_INFORMATION, SET_INFORMATION, DIRECTORY_CONTROL,
  DEVICE_CONTROL, FLUSH_BUFFERS. Drivers without CREATE/CLOSE/FLUSH succeed by default. A
  single-level IRP (no stack locations) is enough while every request completes in its dispatch
  routine. Asynchronous I/O and stacking can add locations later.
- **Buffered I/O only.** IRP buffers are kernel memory. The system services (`io/iosvc.c`) copy
  user buffers through pool bounce buffers in 256 KiB chunks, so drivers and file systems never
  touch user addresses and never meet SMAP.
- **Mounting.** Storage devices (`DO_STORAGE`: partitions, whole disks without a partition table,
  the ramdisk) carry a VPB. The first open below one mounts it (`IoMountVolume`, under one mutex):
  each registered `FILE_SYSTEM.Mount` is tried until one recognizes the volume and creates its
  volume device. A volume nobody recognizes is marked so and never retried.
- **Opening paths.** `IoCreateFile(path, access, disposition, options)` has three outcomes:
  - **A path below a storage device:** Ob walks the path to the device; its parse procedure
    (`IopParseDevice`) gets the remainder and the open parameters (Ob's parse context), mounts
    the device, creates the file object on the volume device, and sends `IRP_MJ_CREATE` with the
    remaining path.
  - **A path that ends at a device (`\Device\Keyboard0`, `\Device\Harddisk0\DR0`):** a raw file
    object on that device.
  - **A storage device with a trailing separator (`D:\`):** the root directory of its volume.

  `GENERIC_READ/WRITE/ALL` are mapped to file rights. Handles get the granted rights, and the
  services check them per operation (read, write, list, delete).
- **Kernel file API** (`io/iofile.c`): `IoReadFile`/`IoWriteFile` (explicit offset or the file
  object's position; the position advances either way, as for NT synchronous files), query/set
  information (position is handled by Io itself), `IoQueryDirectoryFile`, and `IoReadWholeFile`
  (process creation reads images with it). Requests on one file object are serialized by its
  mutex. Lock order: file object → volume.
- **Drive letters.** Drivers register mountable storage with `IoRegisterStorageDevice(device,
  path, boot)`. After the drivers start, `IoInitializeDrivers` mounts each one and links
  `\??\C:` to the boot volume, then `\??\D:`, `\??\E:`, ... to the others.
- **System services:** CreateFile, ReadFile, WriteFile, Query/SetInformationFile (basic, position,
  end-of-file, disposition, rename, volume), QueryDirectoryFile (fixed-size
  `HZ_FILE_DIRECTORY_INFORMATION` entries; `STATUS_NO_MORE_FILES` at the end), and
  DeviceIoControlFile. That brings the total to 30 services.

## 23. Storage: PCI, AHCI, disks and partitions (M5)

- **PCI** (`drivers/pci`): configuration space through ECAM (the ACPI MCFG table; a 1 MiB uncached
  window per bus, mapped on first use), with ports 0xCF8/0xCFC as fallback. The bus is scanned
  recursively through PCI-to-PCI bridges. BARs are sized with decoding disabled, and MSI/MSI-X
  capabilities are recorded.
  - Legacy INTx routing needs ACPI `_PRT` (AML), which Nexora OS does not interpret. Drivers use
    **MSI** (single message to the boot CPU, INTx masked) and fall back to polling.
  - `KeConnectInterruptAnyVector(irql, ...)` hands out a free vector in that IRQL's band.
- **AHCI** (`drivers/ahci`): every SATA AHCI controller (class 01.06.01) is taken over from the
  firmware: BIOS/OS handoff, AHCI enable, and every implemented port with an ATA device stopped
  and reprogrammed (no HBA reset, which keeps boot fast). Ports whose link is down are skipped.
  ATAPI devices, such as the boot DVD, are left alone.
  - Per port: one 4 KiB page below 4 GiB (command list, received FIS, one command table) and a
    64 KiB contiguous bounce buffer, so one PRD entry covers each command and callers may pass
    any kernel buffer.
  - Commands use slot 0, serialized by a per-port mutex: IDENTIFY, READ/WRITE DMA EXT (LBA48,
    512-byte sectors).
  - Completion is signaled by MSI (ISR → DPC → event). The waiting thread also re-checks the
    command-issue register every millisecond, so a missing interrupt costs latency, not
    correctness. Errors and timeouts (5 s) log the task file and restart the port.
- **Disk class** (`drivers/disk`): `DiskCreateDisk(description)` turns a port driver's sector
  transfer routine into `\Device\HarddiskN\DR0` and `Partition1..n`.
  - Partition tables: GPT first, with header and partition-array CRC32 verified, then MBR primary
    partitions (protective and extended entries skipped).
  - Requests must be sector-aligned. They are bounds-checked per partition and split at the port
    driver's maximum transfer.
  - IOCTLs: `IOCTL_DISK_GET_GEOMETRY`, `IOCTL_DISK_GET_PARTITION_INFO` (with the GPT unique GUID
    since M10), and (M10, on DR0) `IOCTL_DISK_GET_INFO` (model, size, partition count, read-only /
    in use / holds C:) and `IOCTL_DISK_UPDATE_PARTITIONS` (needs write access; refused with
    `STATUS_DEVICE_BUSY` while a volume of the disk is mounted, checked under the I/O manager's
    mount lock). An update reads the table again, updates the partition devices in place (device
    objects are never deleted; partitions beyond the new table shrink to zero sectors), creates
    and registers new ones, and clears "unrecognized" so the new volumes can mount.
  - A GPT partition of type `7A1F5C3E-2B4D-4E8A-9F60-485A54455354` ("Nexora OS test scratch") is
    the only place the raw-sector self-test writes.
- **Ramdisk** (`drivers/ramdisk`): the boot module as a read-only 512-byte-sector device,
  `\Device\Ramdisk0`, registered as the live image; the `install` module (M10) as
  `\Device\Ramdisk1`, hidden.

## 24. File systems (M5)

- **UstarFs** (`fs/ustarfs`, read-only): mount reads every USTAR header through the device and
  builds an in-memory index (normalized `\` paths, data offsets, sizes, times). Directories that
  appear only as path prefixes are synthesized. Reads go through whole sectors. Any create, write
  or set-information request returns `STATUS_MEDIA_WRITE_PROTECTED`. This replaces M4's direct
  archive reader: process images on C: are now ordinary file reads.
- **FAT32** (`fs/fat32`, read/write):
  - **Mount** checks the BPB strictly: FAT32 only (no FAT12/16), sector size equal to the
    device's, power-of-two clusters up to 64 KiB, FAT size and root cluster plausible. It
    honours the "mirroring off / active FAT" flag. The free count and next-free hint come from
    FSInfo when plausible, otherwise from a scan of the FAT.
  - **Write-through, no data cache.** A modified FAT sector is written to every FAT copy at once,
    directory entries go straight back to disk, and FSInfo is updated at the end of each
    modifying request. Two one-sector caches (FAT, directory) serve the many small reads. File
    data moves directly between the caller's kernel buffer and the disk in whole sectors, with a
    read-modify-write for partial sectors.
  - **FCB/CCB.** One FCB per open file or directory, found by the position of its directory
    entry, holds size, first cluster, a chain-position cursor (sequential I/O does not re-walk
    the chain) and delete-pending. A CCB per open holds the enumeration cursor and
    delete-on-close. One mutex per volume serializes requests.
  - **Names.** UTF-8 in the API and UTF-16 in VFAT long-name entries (checksummed, assembled in
    order). A name that is valid 8.3 with each part in a single case is stored as a short entry
    only, with the NT lowercase bits, as Windows and mtools do. Other names get long-name entries
    plus a unique `BASIS~N.EXT`. Lookups match long and short names case-insensitively (ASCII).
  - **Operations:**
    - open, overwrite and supersede dispositions;
    - read, and write with gap zero-fill;
    - set end-of-file (truncate frees clusters; extend zero-fills);
    - delete on last close, with directories required to be empty;
    - rename within or across directories (a moved directory's `..` is rewritten; moving a
      directory into itself is refused);
    - enumeration;
    - creation and write timestamps from the system time.
  - **Limits:** files up to 4 GiB − 1 and directories up to 65536 entries. No sharing modes and
    no access-date updates. A failed allocation part-way through a write gives its clusters back.
- **Test disk** (`scripts/mkdisk.sh`, `make testdisk`): a 64 MiB GPT image with a 48 MiB FAT32
  partition labeled `HZTEST` (512-byte clusters, so files and directories span many clusters)
  holding known files, plus the 4 MiB scratch partition. The smoke tests attach a fresh copy per
  case. After the boot case, the harness runs `fsck.fat -n` on the partition and reads the tree
  the kernel left there back with mtools.

## 25. Input, time and power (M5)

- **PS/2** (`drivers/i8042`):
  - The controller is probed directly, because OVMF's FADT does not set the 8042 flag even though
    QEMU has one. It is self-tested and the second port detected. Keyboard bytes arrive in scan
    code set 1 (controller translation). The mouse is put in IntelliMouse mode (4-byte packets
    with a wheel) when it answers ID 3.
  - The ISR (IRQL 5, ISA IRQ 1/12 through the I/O APIC) only moves bytes into per-device rings;
    a DPC decodes them into `HZ_KEY_EVENT` (scan code, E0 flag, break, modifiers, virtual key,
    US-layout character) and `HZ_MOUSE_EVENT` (dx, dy with down positive, wheel, buttons) queues.
    A full queue drops its oldest event.
  - `\Device\Keyboard0` and `\Device\Mouse0` return whole events from reads and block until one
    is available, alertably for user callers. win32k's input threads (M6, §29) are their
    readers; with `hz.input=echo` the driver's echo threads read them instead (each queue has one
    reader, or the events would be split).
- **System time** (`hal/rtc.c`, `ke/time.c`): the CMOS clock is read at boot (BCD or binary,
  12/24 h, century from the FADT register, read until two snapshots agree), with the
  bootloader's time as fallback. It is kept as HZ time (100 ns units since 1601, UTC) at uptime
  zero; `KeQuerySystemTime` adds the TSC uptime. File times are UTC; there are no time zones yet.
- **Power** (`hal/power.c`, `ex/power.c`):
  - `ExShutdownSystem` flushes every mounted volume, then:
    - **power off:** ACPI S5, writing `SLP_TYPx | SLP_EN` to PM1a/PM1b, with the sleep types
      from the DSDT `\_S5` package. That package is found by a byte-pattern scan (no AML
      interpreter), after enabling ACPI mode through `SMI_CMD` if the firmware left it in
      legacy mode.
    - **reset:** the FADT reset register (I/O, memory or PCI config space), the 8042 reset
      pulse, then `0xCF9`, then a triple fault.
  - `NtShutdownSystem(action)` exposes it to user mode. There is no privilege check yet.

## 26. Sections (M6)

- **Section objects** (`mm/section.c`, Ob type `Section`) are shared memory: a section owns
  resident, zeroed physical pages for its whole life (up to 256 MiB; creation keeps 1024 pages
  of memory in reserve). There is no paging file, so there is nothing to page in.
- **Views.** `MmMapViewOfSection` maps all pages of a section into an address space at once, as
  a VAD with `MI_VAD_SECTION` that holds a section reference. Unmapping removes the PTEs without
  freeing the pages and drops the reference, so the pages live until the last view and the last
  handle are gone. Section VADs are excluded from `NtProtectVirtualMemory`, from demand-zero
  faults and from `NtFreeVirtualMemory` (`MEM_MAPPED` in queries; release them with
  `NtUnmapViewOfSection`). `MmMapViewInSystemSpace` maps a section once into system space (the
  compositor reads surfaces there).
- **Services** (31–33): `NtCreateSection(&Handle, Size)`, `NtMapViewOfSection(Section, Process,
  &Base, &ViewSize, Protect)` (current process only, read-only or read/write), and
  `NtUnmapViewOfSection(Process, Base)`.

## 27. Regions (M6)

- `win32k/rgn` (Rust, `no_std` + `alloc` over the non-paged pool, tag `Rgn `) implements regions
  in the canonical y-x banded form used by X11 and pixman: horizontal bands sorted top to bottom,
  sorted non-touching rectangles inside a band, no two adjacent bands with identical intervals.
  Equality is therefore structural, and rectangle counts stay small.
- One generic operation does union, intersect, subtract and xor: cut the plane into slabs at
  every band boundary of both operands, combine the interval lists in each slab, and merge equal
  neighbouring bands.
- The C ABI (`include/w32k.h`) uses opaque `RGN *` handles: create/destroy/set/copy, combine
  with a region or a rectangle, offset, point and rectangle tests, bounds, area, the rectangle
  list (valid until the next change) and equality.
- `RgnSelfTest` (boot, "Runtime: ... Rust OK") runs 10 fixed cases and 200 random property tests
  of every operation against a 32x32 bitmap, checking the canonical form after each.

## 28. Windows, messages and the compositor (M6)

- **Window states (M7):** minimizing hides the window (`HZ_WINDOW_STATE_MINIMIZED`, WM_SIZE with
  `HZ_SIZE_MINIMIZED`); restoring shows and activates it again. Maximizing stores the normal
  rectangle and requests the work area (the whole screen until the taskbar reserves part of it);
  restoring requests the stored rectangle. A program that resizes a maximized window itself
  makes it normal again. Maximized windows are drawn square, without border.
- **Window attributes (M7):** `SetWindowAttribute` sets the client-area cursor shape
  (`HZ_WA_CURSOR`) and the minimum size for interactive resizing (`HZ_WA_MIN_SIZE`).
- **Windows** (`win32k/window.cpp`): handles are `(generation << 16) | slot` (slots 1..1024),
  distinct from kernel handles and rejected once stale. Windows are top-level only, kept in one
  z-ordered list with `HZ_WS_TOPMOST` windows above the rest. Each window owns a surface: a
  section of width × height 32-bit pixels (0xAARRGGBB), mapped into the owning process and into
  system space. A resize allocates a new surface, copies the overlap, bumps the surface
  generation, marks the whole window invalid and posts `HZ_WM_SIZE`. Only the creating thread
  may destroy, show, move, resize, paint or set timers; any thread may post, invalidate, query
  or activate. When a thread with windows exits, `PsExitThread` calls win32k's thread callout,
  which destroys its windows, timers and queue.
- **Messages** (`win32k/queue.cpp`): one FIFO per thread (at most 4096 messages; consecutive
  mouse moves for the same window are coalesced). `GetMessage` returns posted messages first,
  then `WM_QUIT`, then a generated `WM_PAINT` (while a visible window's update region is not
  empty; `BeginPaint` validates it). Revealing a window with pending damage wakes its owner so a
  thread blocked in `GetMessage` can observe the generated paint. The queue waits alertably on
  the thread's event with the earliest timer as the timeout, so a blocked thread can still be
  terminated.
  `WM_CREATE`/`WM_DESTROY` are sent by hzuser, not by the kernel.
- **Painting:** `BeginPaint` returns the update region's bounds and the surface; the application
  draws, and `EndPaint` queues the painted rectangle for composition.
- **Compositor** (`win32k/compose.cpp`):
  - It owns three screen-size pool buffers: the back buffer, the procedural wallpaper ("Nexora
    dusk") and the Mica backdrop (wallpaper box-blurred in 16 px cells and tinted
    `#1C1C24` at 77%).
  - Changes are added to a dirty region (spinlock). A compositor thread (priority 11) swaps it
    out, snapshots the visible windows under WmLock (taking a reference on each surface), and
    redraws each dirty rectangle with the painter's algorithm: wallpaper, then per window its
    shadow, its body and its rounded corners, then the software cursor (12x19 sprite). It
    starts at the topmost window whose opaque area covers the rectangle. A compositor/paint mutex
    is held while surfaces are read and from `BeginPaint` through `EndPaint` while their owners
    write, preventing mixed frames without per-window double buffers. Frames are paced to at most
    one per 16 ms, and dirty rectangles are copied to the framebuffer, which is never read.
  - Integer math only: an 8x8 corner coverage table (4x4 supersampled quarter circle), a
    separable shadow falloff table (24 px, 8 px down; popups 10 px), and a 1 px translucent
    border. The shadow is skipped only under fully opaque body pixels, so it continues under the
    corner cut-outs. Popups are rounded too (M7); maximized windows are not.
  - The cursor is a sprite chosen by shape (arrow, I-beam, hand, four resize arrows; the
    vertical and one diagonal arrow reuse the others' art transposed or mirrored), drawn at its
    hotspot. Its dirty box is fixed around the hotspot so shape changes need no extra bounds.
  - Ordinary windows are opaque; `HZ_WS_MICA` windows blend their pixels over the backdrop by
    their alpha. Inactive windows get a lighter shadow.
  - Until `W32kShowDesktop` (after `[HZ] BOOT`), frames are composed but not flushed, so the boot
    screen stays while the self-test reads composed pixels back (`NtUserFlush`,
    `NtUserGetScreenPixel`). `InbvHandOffDisplay` then stops boot-log drawing.
- **System services:** win32k registers a second service table at `0x1000` (the "shadow table"
  idea from NT) with `KeSetWin32ServiceTable`; the dispatcher picks the table by service number.
  24 services: CreateWindow, DestroyWindow, ShowWindow, SetWindowPos, GetWindowInfo,
  SetWindowText, GetMessage, PeekMessage, PostMessage, PostQuitMessage, InvalidateRect,
  BeginPaint, EndPaint, SetTimer, KillTimer, GetDisplayInfo, GetCursorPos, SetForegroundWindow,
  WindowFromPoint, SendInput, Flush, GetScreenPixel, SetCapture and SetWindowAttribute (since
  M7), RegisterShell, EnumWindows, SetWorkArea, SetClipboardText, GetClipboardText and
  SetWallpaper (since M8), GetShellWindow (M9; 31 in all). Their ABI is `include/uapi/hzuser.h`.
  GetDisplayInfo also reports the work area, the cursor shape on screen and (M9) the current
  wallpaper.
- **Shell support (M8):** `WmShellWindow`/`WmShellProcess` name the registered shell; "app
  windows" have a caption, are not popups or owned (`HZ_WS_OWNED`, M9: dialogs) and do not
  belong to the shell. Programs find the shell window with `GetShellWindow` and post it
  `HZ_WM_SHELLREQUEST` (M9; `HZ_SHELL_REQUEST_WALLPAPER`), so settings that the shell owns stay
  in one place. Creation, destruction,
  activation (of an app window or of none), visibility and state changes, and title changes post
  `HZ_WM_SHELLNOTIFY` to the shell window. `EnumWindows` lists app windows top first (visible or
  minimized). Changing the work area re-requests the size of maximized windows; when the shell
  window is destroyed the work area returns to the whole screen.
- **Wallpaper swap (M8):** `CmpSetWallpaper` builds the new wallpaper and its Mica backdrop in
  fresh buffers, then swaps them while holding the compositor's frame mutex (so no frame mixes
  two wallpapers) and recomposes the screen.
- **Locking:** WmLock (a mutex) protects windows, z-order, activation, capture, queues and
  timers; the compositor's dirty region has a spinlock. Lock order: WmLock → dirty lock → pool.

## 29. Input routing (M6)

- Two system threads (priority 12) read `\Device\Keyboard0` and `\Device\Mouse0` and feed the
  same functions as `NtUserSendInput` (which also accepts absolute mouse positions, for tests).
  All routing runs under WmLock.
- **Mouse:** the cursor moves by the deltas, clamped to the screen. A left press activates the
  window under the cursor (unless `HZ_WS_NOACTIVATE`; pressing the desktop deactivates) and
  hit-tests it:
  - resize border (`HZ_WS_RESIZABLE`, 6 px, none when maximized): a resize drag. Motion stores
    the new rectangle (at least 160x64 and the window's `HZ_WA_MIN_SIZE`) and posts
    `HZ_WM_SIZE_REQUEST` once until the owner applies it with `SetWindowPos(HZ_SWP_PENDING)`;
  - caption (`HZ_WS_CAPTION`, 32 px): a move drag done by the kernel, `HZ_WM_MOVE` at the end;
    a double-click toggles maximize (`HZ_WS_MAXIMIZEBOX`). Dragging a maximized window first
    restores it under the cursor (same relative position along the caption) once the pointer
    has moved 4 px; the move continues on the pending rectangle until the owner applies it;
  - caption buttons (46 px each from the right: close, maximize with `HZ_WS_MAXIMIZEBOX`,
    minimize with `HZ_WS_MINIMIZEBOX`): the action happens on release over the same button.
    The owner hears hover and press changes through `HZ_WM_CAPTIONSTATE` (one window at a time,
    reset when the pointer leaves);
  - client area: button messages in window coordinates with implicit capture until release,
    and double-click detection (500 ms, 4 px; caption and client clicks do not pair).
  Moves go to the capture window or the window under the cursor (`HZ_WM_MOUSELEAVE` when the
  cursor leaves it); the wheel sends 120 per notch.
- **Explicit capture (M7):** `SetCapture` sends every mouse message to one window of the calling
  thread, in its coordinates even outside it; presses neither activate nor hit-test captions,
  releases keep it. It ends with `SetCapture(0)`, when the window is hidden or destroyed, or
  when activation changes (`HZ_WM_CAPTURECHANGED` to the window that loses it).
- **Mouse lock (M12, for games):** `SetWindowAttribute(HZ_WA_MOUSE_LOCK, 1)`, refused unless the
  window is active.
  - The cursor is parked at the window's centre and hidden (`HZ_CURSOR_HIDDEN`; the compositor
    draws nothing).
  - Every motion is posted as `HZ_WM_MOUSEDELTA` (`HZ_MAKE_POINT(dx, dy)`; absolute injected
    input becomes the difference from the parked position).
  - Buttons and the wheel go straight to the window, at the parked point, without hit-testing
    or activation.
  - It ends on request (no message), or with `HZ_WM_MOUSEUNLOCKED` when activation changes, the
    window is hidden or destroyed, or the logo key or Alt+Tab goes to the shell.
- **Cursor shape (M7):** recomputed after every move, press, release and window change: resize
  arrows over borders and during resize drags, the arrow over captions, otherwise the client
  cursor of the window under the pointer (or of the capture window).
- **Keyboard:** key-down/up and character messages go to the active window's thread. Alt+F4
  posts `HZ_WM_CLOSE` to the active window; Alt+Tab activates the bottom-most normal window
  (cycling; minimized windows are restored).
- **Shell keys (M8):** with a shell registered, the logo key pressed and released on its own
  posts `HZ_SHELL_START_KEY` (it is never delivered to applications; combined with another key
  it does nothing yet). Alt+Tab posts `HZ_SHELL_SWITCH_NEXT` (with Shift: backwards) and starts a
  switching state in which every key is swallowed: more Tabs post more NEXT events, Escape
  posts `HZ_SHELL_SWITCH_END` with 0 (cancel), releasing Alt posts it with 1 (switch). The kernel
  does not change activation itself; the shell does.

## 30. hzuser, the user-mode window library (M6)

- **Heap** (`user/lib/hzrt/heap.cpp`, part of hzrt): `malloc`/`calloc`/`realloc`/`free` and C++
  `new`/`delete`. Blocks carry a 16-byte header (magic, class, size) and are 16-byte aligned.
  Power-of-two classes from 32 B to 4 KiB are carved from 64 KiB chunks; larger blocks get their
  own allocation. Freeing an invalid pointer or freeing twice ends the process with a message.
  One spin lock (yielding) serializes threads.
- **Windows** (`user/lib/hzuser/window.cpp`): a per-process table indexed by the handle's slot
  holds each window's procedure, user data, style, activation state and surface. A window is
  created hidden, receives `HZ_WM_CREATE` and a first `HZ_WM_PAINT` synchronously, and only then
  is shown, so its first composed frame already has content. `HzDispatchMessage` tracks
  activation (and repaints the caption) and refreshes the surface on `HZ_WM_SIZE`.
  `HzDefWindowProc` destroys on close, applies resize requests and paints the default
  background.
- **Caption:** `HzBeginPaint` draws the standard caption for `HZ_WS_CAPTION` windows: transparent
  over Mica (opaque `#202020` otherwise), the logo tile, the title (Inter 13 px), and the caption
  buttons (close, and maximize/restore and minimize when the style has them) with their hover
  and pressed backgrounds from `HZ_WM_CAPTIONSTATE` (close turns red). Text is dimmed in
  inactive windows. hzuser also offers `HzShowWindow`, `HzIsMaximized`/`HzIsMinimized`,
  `HzSetCursor`, `HzSetMinimumSize`, `HzSetCapture`/`HzReleaseCapture` and `HzGetScreenRect`.
- **Drawing** (`draw.cpp`): fills, "over" compositing with non-premultiplied alpha that keeps the
  destination alpha (text drawn on a transparent Mica caption stays translucent), and
  anti-aliased rounded rectangles and lines.
- **Fonts** (`font.cpp`): stb_truetype with its math, allocation and string hooks mapped to
  `hzmath.h`, the heap and hzrt. Fonts are read whole from `C:\Nexora\Fonts`; sizes are em
  sizes in pixels. Glyphs below U+0100 are cached per font, others are rasterized on use. The
  pen advances in fractional pixels with kerning. The standard fonts are Inter Regular 14,
  SemiBold 14, SemiBold 28, Regular 13, SemiBold 20 and Regular 12 (the last two since M7); a
  font file is read once and shared by its sizes. Length-bounded measuring and drawing
  (`HzMeasureTextN`, `HzDrawTextN`, and `HzMeasureTextFixed` in 1/64 px for caret positions)
  serve the toolkit.
- **Tests:**
  - The boot check (`W32kRunSelfTests`, `win32k/w32test.cpp`) runs before the desktop is shown and
    verifies the window manager starts with no window and no surface section. The self-test
    programs that used to drive windows, painting and composition here - `wintest.exe` (139 checks
    since M7, which added caption button hover and press, minimize/maximize/restore by buttons,
    commands, double-click, drag and Alt+Tab, cursor shapes and their sprites, minimum sizes and
    explicit capture), `uitest.exe` and `apptest.exe` - were removed with the beta apps.

## 31. hzcore and hzui, the C++ toolkit (M7)

- **hzcore** (`user/include/hzcore.h`, `user/lib/hzrt/hzcore.cpp`): there is no C++ standard
  library, so programs get a small one: `Move`/`Forward`, placement new, `Vector` (growth by
  doubling; pushing one of its own elements is safe), `String` (UTF-8, NUL-terminated, byte
  offsets on code point boundaries, `Format`, case-insensitive search), `UniquePtr`, `Function`
  (type-erased, copyable; plain functions decay to pointers). Allocation failure ends the
  process, as `operator new` does.
- **Shape.** `user/lib/hzui` is an archive (`hzui.lib`) over hzuser. A `Window` owns one tree of
  `Element`s that draw into its surface. Elements own their children. Popups (menus, tooltips,
  the combo box list) are `PopupWindow`s owned by hzui and freed by the message loop after they
  close (`RunMessageLoop`, `PumpMessages`, `DeferDelete`).
- **Layout** is two-pass, like WPF/WinUI: `Measure(available)` returns a desired size (margins,
  padding, fixed `Width`/`Height`, minimum and maximum sizes applied around `MeasureOverride`),
  then `Arrange(slot)` places the element by its alignment (stretch, start, centre, end) and
  calls `ArrangeOverride`. Panels: `StackPanel` (orientation, spacing; hidden children take no
  space), `Grid` (pixel, auto and star tracks, spans, spacing), `ScrollViewer` (vertical).
  Bounds are in window coordinates. Any layout change re-lays out the window before its next
  paint (simple and fast enough for windows of hundreds of elements).
- **Painting** is damage-driven: `Invalidate()` queues the element's bounds (plus room for the
  focus ring), and the window repaints the update rectangle back to front, clipped, skipping
  elements outside it. The `Painter` blends with non-premultiplied "over" (translucent fills on
  Mica stay translucent). Shapes are anti-aliased from signed distances (rounded rectangles,
  inner strokes, circles, vertical gradients); polylines take the minimum distance to all
  segments, so translucent joints are not blended twice. Icons are drawn procedurally from
  lines and circles on a 16x16 grid. Text: one-line with ellipsis, and a `TextLayout` that wraps
  at spaces (or inside long words) and at line feeds.
- **Input.** Pointer events go to the deepest element under the pointer and bubble until one is
  handled; the element that handles a press receives the moves and the release (`IsPressed()`
  is true while the pointer is over it). Hover enter/leave are diffs of the ancestor chains.
  Keyboard events go to the focused element and bubble; unhandled Tab/Shift+Tab move focus in
  tree order over visible, enabled, focusable elements; the focus ring shows only after keyboard
  navigation, and the focused element is scrolled into view (`BringIntoView` up the parents).
  A press on a focusable element focuses it. The window sets the cursor shape from the element
  under the pointer (or the pressed one).
- **Animation.** `Transition` eases a value (cubic ease-out). An element that starts one asks its
  window for frames (`StartAnimation`); a 15 ms window timer calls `Animate()` while any element
  returns true and stops when none does. Hover fades, the toggle knob, scrolling and the
  indeterminate progress bar use it. Windows also run program timers with callbacks.
- **Menus** (`ShowMenu`): a popup sized to its items (check/icon column, text, shortcut,
  separators), kept on screen, holding the explicit capture. The owner forwards keys to it
  (arrows, Home/End, Enter/Space, Escape); a click outside or a capture loss closes it;
  choosing closes it before the item's action runs. **Tooltips** appear after 600 ms of hover on
  an element with `ToolTip` text, below the pointer, and hide on moves to other elements, presses,
  keys and deactivation; after a press they stay hidden until the pointer leaves the element.
- **Controls:** `TextBlock`; `Button` (standard, accent, subtle; optional icon); `ToggleSwitch`;
  `CheckBox`; `RadioButton` (groups among siblings); `Slider` (pointer, wheel, keys, steps);
  `ProgressBar` (determinate, indeterminate); `TextBox` (single line: caret with blink, selection
  by pointer, double-click and Shift+keys, Ctrl word moves and deletes, select all, cut/copy/paste
  through the system clipboard (M8), horizontal scrolling, context menu, I-beam cursor); `ComboBox`
  (opens a menu below it); `ListView` (virtualized fixed-height rows: only visible rows are
  painted and hit-tested; selection, hot row, keyboard, smooth wheel scrolling, custom row
  painting); `ScrollBar` (thin, widens on hover); `ScrollViewer`; `TabView` (a single tab stop:
  arrows switch tabs, Ctrl+Tab cycles, focus follows the selection).
- **Theme** (`hzui::Theme`): dark tokens (text, control fills and strokes, cards, popups, focus
  strokes), radii (4 px controls, 8 px overlays), 32 px control height and the accent colour,
  Nexora orange `#FF9F5A` (the sun of the logo) with dark text on it. Fonts by role: body 14,
  body strong 14, caption 12, subtitle 20, title 28.
- **Tests:** the toolkit's boot tests were removed with the beta apps. `uitest.exe` (104 checks)
  covered hzcore, global constructors, layout math, Painter pixels and controls rendered
  off-screen, and a live window driven by injected input (Tab order, editing, every control,
  menus, the list, tooltips, cursor shapes, maximize, close requests, tab view and scrolling);
  `welcome.exe` was the gallery (Start, Controls, List and About tabs; the clock became
  `clock.exe` in M8) that the `desktop` smoke case typed into, tabbed to a button and dragged,
  all through QEMU's PS/2 devices. The controls remain in hzui and are exercised by the apps.

## 32. The shell (M8)

- **Start-up.** After `[HZ] BOOT` the kernel shows the desktop and starts
  `C:\Nexora\System\shell.exe` (there is no fallback program: the old `welcome.exe` fallback went
  with the beta apps). The shell creates the taskbar (hidden), registers it as the shell window,
  sets the work area to the screen above the 48 px taskbar and shows the lock screen. Unlocking
  shows the taskbar and the desktop.
- **State** (`user/shell/shell.cpp`): the installed programs (manifests, §34), the app windows
  in taskbar order (first seen first; refreshed from `EnumWindows` on every window notification)
  each with its process and program (looked up once, by the process's image name), the active
  app window (from activation notifications), the recently opened programs (up to four, newest
  first: launched from Start or seen opening a window, for example Notepad started by Files;
  kept in memory only), the flyouts, the switcher and the quick settings values.
- **Requests (M9):** `HZ_WM_SHELLREQUEST` on the taskbar window; `HZ_SHELL_REQUEST_WALLPAPER`
  sets the wallpaper and refreshes quick settings if open (Settings uses it).
- **Lock screen:** a full-screen topmost transparent window drawing a gradient scrim, the time
  (Inter SemiBold 104 px, loaded by the shell) and the date over the crisp wallpaper. Any key or
  click slides it away (a 260 ms transition) and unlocks. There is no password yet.
- **Taskbar** (`user/shell/taskbar.cpp`): a frameless, topmost, never-activated Mica strip. The
  middle group holds the Start button (the logo) and one button per app window (its icon; an
  accent pill under the active window, a small grey one under the others; the title as tooltip).
  Clicking a window's button activates it, minimizes it if it is already active, or restores it
  if minimized. On the right: the quick settings button and the clock (time and date, UTC). As
  the taskbar is never activated, clicking it leaves the active window and open flyouts alone.
- **Flyouts** (`user/shell/flyouts.cpp`): activated Mica popups above the taskbar that close when
  they lose activation, on Escape, or when their button is pressed again.
  - **Start** (logo key or Start button): a search box (focused; typing searches every installed
    program, ranked: name starts with the text, name contains it, description contains it;
    Enter starts the best match), the pinned program tiles (five per row, manifest order), the
    recent programs, and a bottom bar with the user ("Guest") and a power menu (restart, shut
    down through `NtShutdownSystem`).
  - **Quick settings:** four toggle tiles (night light, focus, energy saver, airplane mode),
    brightness and volume sliders (visual only: there is no display gamma or audio yet), and the
    three wallpapers (`SetWallpaper`).
- **Switcher:** on the first `SWITCH_NEXT` the shell takes the app windows top first (most
  recently active first), selects the second one and shows a never-activated popup of tiles
  (icon and title, the selection outlined in the accent colour); further events move the
  selection; `SWITCH_END` closes it and, when committing, activates (or restores) the selection.
- **Program icons:** the manifest's glyph (an hzui icon name) in white on a tile of the
  manifest's colour (a slight vertical gradient); windows of programs without a manifest get a
  coloured letter tile. The logo tile is reserved for Start.
- **Logging:** the shell logs its notable steps (`shell: N programs installed`, `unlocked`,
  `start opened`, `launched <program>`, `switched to <title>`, `activated/minimized/restored
  <title>`, `wallpaper <n>`, `wallpaper <n> (requested)`) for the smoke tests.

## 33. Pipes, standard handles and other processes (M9)

- **Anonymous pipes** (`io/iopipe.c`): `NtCreatePipe(&Read, &Write, Size)` (default 16 KiB, at
  most 1 MiB) creates two file objects on `\Device\Pipe` that share one `IOP_PIPE`: a ring
  buffer, a mutex, a notification event signalled on every change, and "reader closed"/"writer
  closed" flags set by `IRP_MJ_CLOSE` of each end (`FsContext2` says which end a file object is).
  A read returns what is there (at least one byte), waiting while the pipe is empty, and
  `STATUS_END_OF_FILE` once it is empty and the writer is gone. A write copies as much as fits,
  waits for room, and fails with `STATUS_PIPE_BROKEN` once the reader is gone. Waits from user
  mode are alertable, so terminating a process blocked on a pipe works. The read end has
  `FILE_READ_DATA`, the write end `FILE_WRITE_DATA` (plus `SYNCHRONIZE`); ReadFile/WriteFile check
  them as for files.
- **Standard handles:** `NtCreateProcessEx(Path, CommandLine, Parameters, &Process, &Id)` takes
  `HZ_PROCESS_PARAMETERS` with up to three of the caller's file handles; each is referenced
  (with its granted access) and a new handle to the same file object is inserted into the child
  before its PEB is built, which records them as `StandardInput/Output/Error`. `NtCreateProcess`
  is `CreateProcessEx` without parameters. hzrt's `HzPrint` writes to standard output (with a
  line feed) when there is one and to the kernel log otherwise; `HzLog` always goes to the kernel
  log; `HzWriteStandard` writes raw bytes.
- **Other processes:** `NtOpenProcess(&Handle, Access, ProcessId)` looks the process up in the
  active list (`PsLookupProcessById`; `STATUS_INVALID_CID` if it is gone). The System process may
  only be opened for `PROCESS_QUERY_INFORMATION | SYNCHRONIZE` (`STATUS_ACCESS_DENIED`
  otherwise), which keeps Task Manager and `kill` from ending it; every user process may open
  every other one (there are no users or security descriptors yet).
- **Services:** CreatePipe, CreateProcessEx and OpenProcess bring the kernel table to 36 (the
  hzrt static assert checks the count and the PEB size, 376 bytes).

## 34. Applications (M9)

- **Libraries.** `hzfile.h` (`hz::fs`, in hzrt): DOS paths (`Resolve` folds `.`/`..` and handles
  `D:x`, `\x` and other drives; `Parent`, `FileName`, `Extension`, `Join`), wildcards
  (`MatchPattern`: `*`, `?`, no case), listing (folders first, then by name), whole-file read and
  write, create/delete/rename/copy, volumes (C: to Z: through the volume information class) and
  formatting (sizes, UTC times, status texts). `hzapps.h` (`hz::apps`, in hzrt): manifests.
- **Manifests.** `C:\Nexora\Apps\<id>.app`, `Key=Value` lines: Name, Description, Icon (an hzui
  icon name), Color (RRGGBB), Path, Opens (extensions), Pinned, Order; Name and Path are required
  and unknown keys are ignored. `hz::apps::Load` reads them all (sorted by Order, then Name; bad
  ones are logged and skipped), `OpenPath` runs an `.exe` or opens a document with the program
  that lists its extension (the path is its only argument), `FindByImage` maps a process image
  to its program. Start, Files and the terminal all use them; adding a program means adding an
  executable and a manifest.
- **Toolkit additions** (hzui): `TextEditor` (lines of UTF-8; one `Replace` primitive under every
  change; edits recorded as start/removed/inserted with the selection before, so undo and redo
  replay them; typing grouped into words; tabs become four spaces, CR is dropped; Enter keeps the
  indentation; smart Home; word moves and deletes; Find without ASCII case; the view paints only
  visible lines and follows the caret both ways; a vertical scroll bar; the caret blinks by
  invalidating only itself), `MenuBar` (menus built when opened, so items reflect the state),
  `Dialog` (an owned window, `HZ_WS_OWNED`, centred over its owner, whose content is disabled
  until it closes; then the owner's focus and activation come back; no nested message loop:
  results arrive in callbacks), `MessageBox` (buttons as "A|B|C", Enter = first, Escape = last,
  closing = -1), `InputBox` (one line; the name is preselected without its extension) and
  `ShowFileDialog` (open/save: volumes, an address box, the folder's content with icons, dates
  and sizes, a name box; folders typed or double-clicked navigate; saving over a file asks).
  Twelve file and app icons on the 16-unit grid. Owner windows ignore their own shortcut keys
  while a dialog is open. The JetBrains Mono role (`FontRole::Mono`, 14 px) is the editor's and
  the terminal's font.
- **Programs** (`user/apps/<name>`, one process each, all single-threaded except the terminal):
  - **Notepad:** File/Edit/View menus, find bar (Ctrl+F, F3), status bar (line, column,
    characters), `*` in the title while modified, asks before New/Open/closing drops changes,
    opens the file on its command line; files up to 4 MiB.
  - **Files:** command bar (back, up, refresh, new folder, rename, delete), address box (Ctrl+L),
    volumes with free space, the folder as a list (icon, name, date, type, size), type-ahead,
    Enter/double-click opens through the manifests, Delete/F2/Backspace keys; changes only on
    writable volumes, each confirmed or named through a dialog.
  - **Terminal:** a console view (output lines, then the prompt and the edited line; history on
    Up/Down; Tab completes file names; Ctrl+C abandons the line or ends the running program;
    3000 lines kept). Built-ins: cd, cls, copy, date, del (patterns), dir (patterns), echo, exit,
    help, history, kill, mem, mkdir, ps, ren, rmdir, start, type, uptime, ver, vol. Other names
    run from the current folder or `C:\Nexora\System`: programs with a manifest are started
    and left alone, others get a pipe as standard output and error, a reader thread posts what
    arrives to the window (`HZ_WM_USER + 1`, freed by the receiver) and, after end of file, the
    exit code (`HZ_WM_USER + 2`; "[exit code N]"). `>`/`>>` capture the output into a file.
    Output lines are mirrored to the kernel log (`terminal| ...`).
  - **Calculator:** the engine (`apps/calc/engine.cpp`) keeps an
    entry, an accumulator, a pending operator and the last operation (for repeated `=`); doubles
    are formatted with 15 significant digits (scientific notation outside 1e-9..1e15), the
    square root is `sqrtsd`; keyboard input.
  - **Settings:** Personalization (wallpapers through the shell), Display, Storage, Time, About;
    live pages refresh every second.
  - **Task Manager:** processes (window title, CPU % over the last second from CPU-time deltas,
    memory, threads, handles; sortable; filter; End task through `NtOpenProcess` +
    `NtTerminateProcess` after a confirmation) and performance (CPU from idle-time deltas and
    memory, 60 s graphs, counters).
- **Logging** for the smoke tests: `notepad:`, `files:`, `terminal:`, `calc:`, `settings:`,
  `taskmgr:` lines, and `hzui: dialog "<title>" opened` when a dialog appears.

## 35. Installation and booting from disk (M10)

- **Setup** (`user/apps/setup`, "Install Nexora OS" in Start on live systems) is an ordinary
  program: everything it writes goes through raw device handles and the file services, so the
  kernel only had to learn to read a partition table again (§23) and to pick C: from
  `hz.system`. `installer.cpp` holds the pure parts: CRC-32, random
  version-4 GUIDs, the layout, GPT sectors, FAT32 geometry and sectors, the boot menu.
- **Layout:** protective MBR; primary GPT at LBA 1-33 and the backup at the end (128 entries);
  ESP (type C12A7328-..., "EFI system partition") of 100 MiB at 1 MiB; the Nexora OS partition
  (basic data type, "Nexora OS") from there to the last 1 MiB boundary. The minimum disk is
  230 MB. Only 512-byte-sector disks are offered.
- **FAT32 formatting:** 32 reserved sectors (boot sector, FSInfo, backups at 6 and 7), two FATs,
  the root directory in cluster 2 with the volume label. Cluster size: the largest of the usual
  table's size (4 KiB up to 8 GiB, 8 KiB to 16 GiB, 16 KiB to 32 GiB, else 32 KiB) or smaller
  that still gives at least 65541 clusters (so FAT32 detection by cluster count holds): the ESP
  gets 1 KiB clusters, a 282 MiB system partition 4 KiB. Bigger clusters mean fewer FAT updates
  when copying (the FAT32 driver writes FAT sectors through). The boot sectors are written last.
- **Steps** (a worker thread; progress and the result are posted to the wizard): write zeros over
  the first MiB, the backup then the primary GPT, `IOCTL_DISK_UPDATE_PARTITIONS`, format both
  partitions, copy the install image to the ESP, copy the raw live image (`\Device\Ramdisk0`) to
  `boot\ramdisk.tar`, write `boot\limine\limine.conf` (the system entry with `hz.system=<GUID>`
  and a read-only "system image" entry), copy `C:\Nexora` to the system partition (Setup's
  manifest rewritten unpinned), write `Nexora\Settings\shell.ini` with `FirstBoot=1`, create
  `\Users`. Disks that hold C:, are read-only or have a mounted volume are not offered.
- **First boot:** the shell reads `shell.ini`; with `FirstBoot=1` on a writable C: it skips the
  lock screen and starts `setup.exe firstboot` (name, wallpaper), which saves the settings,
  creates `C:\Users\<name>\Documents` with a welcome note and posts `HZ_SHELL_REQUEST_SETTINGS`;
  the shell reloads (name in Start and on the lock screen) and starts Welcome. The shell saves
  the wallpaper and recent programs whenever they change (installed systems only).
- **Documents:** `hz::fs::DocumentsFolder()` (the user's documents on an installed system, else the
  first writable volume) is where Files, the terminal and the file dialogs start.
- **Settings files:** `hz::fs::Settings` reads and writes `Key=Value` files (comments, case-
  insensitive keys, order kept).

## 36. Multiprocessor (M11)

- **Bring-up, in two steps** (`ke/mp.c`, `ke/apentry.asm`), because Limine parks the application
  processors (APs) in bootloader-reclaimable memory (its code, page tables, GDT and stacks):
  1. `KeParkApplicationProcessors`, from `MmInitializeSystem` once the kernel's page tables are
     active and before that memory is reclaimed. Each AP is sent to `KiApplicationProcessorEntry`
     through Limine's MP response (`BiStartProcessor`: the argument first, then `goto_address`
     atomically). It sets EFER.NXE, the PAT and CR0.WP, loads the kernel CR3 and its own 32 KiB
     .bss stack, loads the boot processor's GDT and IDT, and spins. Processors beyond 8, or all of
     them with `hz.smp=off`, get argument 0: they leave the bootloader's memory too and halt.
  2. `KeStartApplicationProcessors`, in Phase 1 after the worker thread starts. Each AP gets a
     PRCB, `HAL_PROCESSOR_TABLES` (its own GDT, TSS and three 16 KiB IST stacks) and an idle
     thread, and is released. It copies the boot processor's CR0, CR4 (toggling PGE flushes its
     TLB) and XCR0. `HalInitializeApplicationProcessor` loads its GDT, TSS, GS base, IDT, the
     SYSCALL MSRs and programs its local APIC. It then starts its clock and enters the idle loop.
     QEMU: 4 processors online in about 40 ms.
- **Processor blocks:** `KiProcessorBlock[]` and `KeNumberProcessors`. `KeActiveProcessors` is a
  bit mask. Each `KPRCB` has `Online`, `IpiRequests` and per-CPU counters (idle time, context
  switches, interrupts, TLB shootdowns). These are shown by `HZ_SYSTEM_PROCESSOR_INFORMATION_CLASS`
  and the per-processor bars in Task Manager.
- **One global ready queue, by decision.** The 32 FIFO queues and the summary mask stay global
  under the dispatcher lock (SYNCH_LEVEL). The reasons:
  - With up to 8 CPUs and a desktop workload, contention on one lock is small, and the lock was
    already taken for every wait and signal.
  - A global queue gives strict priority order across CPUs for free.
  - Per-CPU queues would need balancing and stealing.

  Correctness comes from three mechanisms:
  - **Affinity:** `KTHREAD.Affinity` is a mask. `KiFindReadyThread(Prcb)` skips threads that may
    not run on that CPU. New threads inherit the creator's mask; threads created by an idle
    thread get all CPUs. `KeSetAffinityThread` moves the caller at once when its current CPU
    leaves the mask.
  - **Placement** (`KiReadyThreadLocked`): a newly ready thread picks an idle CPU allowed by its
    mask (the current one first). Failing that, it picks the CPU running the lowest-priority
    thread below its own. That CPU gets `PreemptRequested`, plus a self-IPI or a directed IPI on
    the DISPATCH vector 0x2F.
  - **SwapBusy handshake:** a thread that is switching away is still on its stack after the
    dispatcher lock is released. `KiSwapThread` sets `Old->SwapBusy` under the lock.
    `KiSwapContext(old sp*, new sp, &Old->SwapBusy)` clears it right after it has left the old
    stack. `KiSwitchTo` spins on `New->SwapBusy` before restoring its XSAVE area or using its
    stack, and the reaper skips threads that are still SwapBusy.
- **Time:** every CPU runs its own LAPIC timer and charges its own quanta. Only CPU 0 advances the
  tick count and expires timers. An idle CPU's clock interrupt requests a dispatch when runnable
  work exists, as a safety net for a lost wake-up IPI. It looks at the ready queues only under a
  *try* of the dispatcher lock: an unlocked walk can follow a moved thread into another circular
  queue for ever (a hang found in M11). Every clock interrupt also serves pending flush requests.
- **Time when the TSC lies (found in VirtualBox):** with more than one processor and its default
  TSC mode, VirtualBox's Hyper-V backend gives the guest a TSC that advances only while guest code
  runs. It then measures up to a third slow and differs between processors. `create-vbox-vm.sh`
  therefore sets `TM/TSCMode=RealTSCOffset`, and the kernel defends itself as follows.
  - The hypervisor's CPUID timing leaf is no longer trusted blindly: the TSC and LAPIC rates are
    calibrated against the reference timer as well (at most 0.5 s), and a disagreement over 2%
    takes the measured values. A check that does not settle keeps the leaf.
  - In the TSC-tied mode the processors' times still diverged while the LAPIC and ACPI PM timers
    stayed right: a timer due "in 10 ms" by one processor's TSC looked minutes away to the
    other. So, as a last resort, the boot clock check compares the clock rate with the reference timer and with the TSC. When
    the TSC is more than 10% off, `KeUseReferenceClock` switches uptime, and so every timeout and
    timer, to `HalReadReferenceTimeUs`: the HPET or PM timer count extended to 64 bits, shared by
    all processors and never going backwards.
  - The switch is continuous with the TSC uptime before it, and the log says so.
  - Quanta stay TSC-based: they are local deltas, so a wrong rate only changes a slice's length.
  - Timer expiry runs on every processor's clock, each with its own DPC. `KeInsertQueueDpc`
    claims a DPC atomically, so a DPC is never queued on two processors at once.
- **Clock watchdog and remote termination (found in VirtualBox):** under the same backend a
  processor spinning in guest code receives its LAPIC timer only on VM exits (12 ticks in 100 ms
  instead of 100 in the M11 measurement), while IPIs still get through. Three consequences are
  covered:
  - `KeYieldThread` returns without taking the dispatcher lock when nothing at its priority or
    above is ready. A yield loop no longer starves other processors of that lock (Hyper-V also
    deschedules a virtual processor that spins on PAUSE).
  - That early return reads a local APIC register at most once per millisecond of TSC time
    (`HalTouchLocalApic`). VirtualBox runs every virtual timer on the first processor's thread,
    so a yield loop there, which never left guest code, stopped every clock in the machine for
    minutes. The read is a VM exit that lets VirtualBox run them. A loop without system calls on
    processor 0 can still stall VirtualBox's timers; nothing in Nexora OS does that.
  - `KeRequestUserReturnCallout` sends the DISPATCH IPI to the processor running the target
    thread, so termination does not wait for that processor's next tick (a user-mode spin loop
    on an AP was otherwise never terminated).
  - Every processor's clock watches the others. If a busy processor's tick count has not moved
    for 10 ms, the watcher sets that processor's `QuantumEnd` when the TSC says the quantum is
    over, and kicks it with a DISPATCH IPI. The heartbeat counts the kicks, and a following
    line gives each processor's tick total since start.
- **TLB shootdown:** `KeFlushTb(Pml4 or 0, Va, Pages)`.
  - It always flushes locally first: INVLPG, or a full flush via CR4.PGE or CR3 above 32 pages.
  - Senders are serialized by the flush lock. The targets are a snapshot of the online CPUs; for
    a user address space, only the CPUs whose current thread runs in it (read after a SEQ_CST
    fence that pairs with the CR3 switch).
  - The sender sets `KI_IPI_FLUSH` in each target's `IpiRequests`, sends an IPI on vector 0xE1
    and waits for all of them.
  - Deadlock avoidance: anyone waiting (for the flush lock, for acknowledgements, or in any
    spinlock spin loop when there is more than one CPU) services its own requests through
    `KiServiceIpiRequests`. Two CPUs flushing at once, or one spinning with interrupts masked,
    therefore cannot deadlock.
  - A wait that never ends (about 2^28 spins) stops the system with `SPIN_LOCK_TIMEOUT`.
  - Callers: `MiUnmapPage` (only when the old PTE was present) and protection changes in
    `mm/vad.c`.
- **Stop:** the first CPU to bug check takes ownership and freezes the others with an
  all-but-self NMI. Their NMI handler records its trap frame and IRQL in the PRCB
  (`FrozenFrame`, `FrozenIrql`) and halts when `KeIsBugCheckActive()`. The serial report then
  lists every processor's thread, IRQL, interrupt flag and stack, so both sides of a
  cross-processor deadlock are visible. Only one stop screen is drawn.
- **Lock audit (M11):** every place that used "interrupts off" as a lock was replaced by a real
  spinlock. The IDT handler table uses `KiInterruptTableLock` at HIGH_LEVEL. The DPC queues,
  the PFN database, pools, address spaces, the object manager, the I/O manager and win32k
  already used spinlocks or mutexes. Globals written only during Phase 0 stay unlocked.
- **Tests:**
  - With more than one CPU, `ke/ketest.c` adds four tests:
    - parallel CPU-bound threads on every processor;
    - a cross-processor wake-up;
    - migration under affinity changes;
    - a shootdown of a page used on another CPU.
  - Time slicing and priority preemption pin themselves to CPU 0.
  - Results: 14/14 on 2 and 4 CPUs, 10/10 on one.
  - `make test` runs on 1 CPU. `QEMU_SMP=N` runs the whole suite on N, and the `smp` smoke case
    always uses 4.

## 37. Performance, polish and robustness (M11)

- **FAT32 write-back FAT cache:** the one-sector FAT cache is written back, not written through
  (`FatCacheDirty`). It is flushed to every FAT copy when another FAT sector is needed, and at
  the end of every modifying operation (`FatFlushFsInfo` flushes it first). The disk is therefore
  consistent between operations. `FatTransfer` coalesces physically contiguous clusters into
  requests of up to 1 MiB. Results in QEMU:
  - Setup's copy: 8.0 s before, 1.75 s after.
  - File-system self-tests: 3.2 s before, 0.84 s after.

  There is no dirty-volume bit. The only consumer would be the host-side `fsck.fat` in the
  smoke tests, which runs after QEMU is killed, and the volume is consistent between operations
  anyway.
- **Glyph cache:** besides code points below U+0100, each font caches up to 192 other glyphs in
  an open-addressed table (password dots, arrows, the euro sign...).
- **Shutdown:** Start > Power > Restart or Shut down posts `HZ_WM_CLOSE` to every program
  window, so Notepad can ask about unsaved work.
  - The shell waits until the windows are gone. After 15 s it names the programs still open and
    offers "Shut down anyway" or "Cancel".
  - `ExShutdownSystem` then flushes and dismounts the volumes (`IoShutdownSystem`) before ACPI
    S5 or the reset.
- **Crashes and the session:**
  - A process ended by an exception is reported to the shell (`W32kNotifyProcessCrash`, then
    `HZ_SHELL_PROCESS_CRASHED` with pid and status), which shows "Program error" naming the
    program and the cause.
  - The kernel's **Session** thread starts `shell.exe` and starts it again (`shell.exe restart`)
    when it exits. It gives up after more than 5 exits in 60 s.
  - A restarted shell skips Welcome, but shows the lock screen if there is a password.
- **Accounts:** the user name and an optional password live in `shell.ini`.
  - The password is stored as `PasswordSalt` (16 random bytes, hex) and `PasswordHash`, computed
    as 10000 iterations of SHA-256 over (previous digest, salt, password) (`user/include/hzcrypto.h`).
  - Random bytes mix RDRAND (when present), the TSC, the clock, the uptime and the pid through
    SHA-256.
  - The password can be set in first-boot setup and in Settings > Accounts; changing or
    removing it needs the current one. A name change renames `C:\Users\<name>`.
  - With a password, the lock screen shows a masked `TextBox` (`TextBox::Password`: dots, no
    copy) and checks it in constant time. The key that opens the box is not typed into it
    (`Window::DropNextChar`).
  - Start > Power > Lock and the request `HZ_SHELL_REQUEST_LOCK` (the terminal's `lock`) lock
    the session again.
  - This keeps the password out of the files. It is not protection against someone who can
    read the disk.
- **Time zone:** `UtcOffsetMinutes` in `shell.ini` (-12:00 to +14:00, set in 30-minute steps in
  Settings > Time). The taskbar clock, the lock screen, Clock and Settings show local time
  through `hz::fs::ToCivil(Time, Offset)`. The kernel clock and file times stay UTC; there are no
  daylight-saving rules.
- **Text:**
  - `TextEditor::WordWrap` (Notepad: View > Word wrap, Alt+Z) lays lines out as rows, greedily
    at spaces with a hard break inside long words. The rows are cached and rebuilt when the
    text, width, font or mode changes. Caret movement, selection painting, hit testing and
    scrolling work on rows.
  - The terminal wraps every line at its column count (monospaced, measured in 26.6 fixed point
    so rows do not drift).
  - `command | more` captures the output and pages it: Space for the next page, Enter for the
    next line, Q or Escape to stop. It is the only pipe.
- **Files:**
  - Ctrl+X, Ctrl+C and Ctrl+V (and command-bar buttons) copy or move a file or a whole folder.
    The file clipboard belongs to the window.
  - A copy next to its original is named "X - Copy" or "X - Copy (2)".
  - Moves on one volume are renames by path; across volumes they are copy then delete.
  - Delete removes a folder with everything in it after a confirmation that counts it.
  - `hz::fs` gained `CopyTree`, `MoveTree`, `DeleteTree` and `IsInside` (a folder is never
    copied or moved into itself), and `Copy` streams in 256 KiB pieces (no size limit).

## 38. Blockyard, the block game (M12) — removed

`user/apps/blockyard`, an original voxel sandbox (a CPU-only 3D renderer spread over every
processor, a mouse-locked first-person camera and a 4 MB world saved to FAT32), was removed with
the beta apps in the item 19 cleanup. The M12 mouse-lock window attribute it was built on
(`HZ_WA_MOUSE_LOCK`) remains a win32k feature; nothing in the tree uses it now.
