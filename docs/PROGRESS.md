# Nexora OS progress

Status at a glance: **M12 complete, version 1.1.0** (2026-10-08). The roadmap (M0–M11) ended
with 1.0.0; M12 added Blockyard, a block-building game, and the window manager's mouse lock. What
could come next is listed under "Beyond 1.0" below.

Resume checklist for a new session:
1. Read `docs/ARCHITECTURE.md` (decisions) and this file (state, next steps, known issues).
2. Build and test: `make iso && make test` (native), or `scripts/build.sh iso` (Docker if
   available). `make test` must print `all cases passed` (35 cases), on one processor and with
   `QEMU_SMP=4`. It builds the FAT32 test disk (`make testdisk`) and attaches a fresh copy per
   case. The `install` case installs on a blank disk and keeps it as `build/test/installed.img`;
   `diskboot`, `diskboot2` and `diskboot3` start from it (and from each other's results) without
   the ISO, so run them in that order, and after changing user-mode code start again at
   `install` (an installed disk carries the programs it was installed with).
   The normal boot ends on the shell's lock screen (any key unlocks; the taskbar appears and the
   welcome program starts); `hz.desktop=off` on the kernel command line keeps the boot screen
   instead.
3. VirtualBox check: `make vbox`, then start the VM (or `scripts/create-vbox-vm.sh --headless`).
   COM1 is logged to `<VM folder>/serial-com1.log`. Boot the normal entry and at least the
   page-fault, stack-overflow, thread-overflow and pool diagnostic ISOs (`build/test/<case>.iso`,
   built by `make test`), and let one boot run for a minute (the status line must keep ticking).
   The normal boot must show `Process self-tests: 12/12 passed` (user mode, loader, faults).
   For the file system tests in VirtualBox attach the test disk once:
   `bash scripts/create-vbox-vm.sh build/nexora-os.iso --test-disk build/test/disk.img`; the boot
   must then show `Volume: D: ... FAT32 'HZTEST'` and `File system self-tests: 16/16 passed`.
   Every boot must show `Window manager self-tests: wintest.exe exit 0x00000000, uitest.exe exit
   0x00000000, apptest.exe exit 0x00000000, 0 windows and 0 surfaces left: OK`; then try the
   desktop with the mouse and keyboard (unlock, drag, resize, the caption buttons, Tab and typing
   in the welcome window, the logo key and Start search, the taskbar buttons, Alt+Tab, quick
   settings and wallpapers) and the programs (with the test disk attached, Notepad can save to
   D:, the terminal can run `dir`, `type` and `hello`, Task Manager can end Clock).
   Installing in VirtualBox: `bash scripts/create-vbox-vm.sh build/nexora-os.iso --wipe-disk
   --start` (an empty 16 GB system disk), run "Install Nexora OS" from Start on disk 0, power off,
   then `bash scripts/create-vbox-vm.sh --boot-disk --start` starts the installed system (first-
   boot setup on the first start, the lock screen with the user's name afterwards). Check one and
   two processors (`--cpus 2`); the script sets the TSC mode multi-processor guests need on
   VirtualBox's Hyper-V backend (known issues below).
4. Screenshots of a running system: `scripts/smoke-test.py --case desktop` leaves
   `build/test/desktop.png`, `--case shell` leaves `build/test/shell.png` and `--case apps`
   leaves `build/test/apps.png` (Notepad over Files); for more, boot with
   QEMU's monitor and use `screendump` (a scratch helper did this during M7–M8: boot, wait for a
   serial marker, send `sendkey`/`mouse_move`/`mouse_button` and dump). Note: QEMU's
   `sendkey keys hold_ms` waits hold_ms between every key event, not only before the releases.

## Milestones

| | Milestone | State |
|---|---|---|
| M0 | Toolchain, Docker, Makefile, Limine, ISO, "Nexora OS" on screen + serial | ✅ done |
| M1 | HAL: GDT/IDT/TSS, exceptions, stop screen, serial logger, ACPI, APIC + timer | ✅ done |
| M2 | Memory manager + self-tests | ✅ done |
| M3 | Scheduler, threads, synchronization, DPCs | ✅ done |
| M4 | Object manager, handles, syscalls, user mode, PE32+ loader, ramdisk, first user program | ✅ done |
| M5 | I/O manager, PCI, PS/2, AHCI, VFS + FAT32 | ✅ done |
| M6 | win32k: framebuffer, Rust region lib, compositor, windows, queues, input, fonts | ✅ done |
| M7 | C++ UI toolkit | ✅ done |
| M8 | Shell | ✅ done |
| M9 | Apps | ✅ done |
| M10 | Installer + first-boot setup; boot from disk | ✅ done |
| M11 | Polish, performance, SMP (version 1.0.0) | ✅ done |
| M12 | Blockyard, a block game; mouse lock (version 1.1.0) | ✅ done |

## Done

### M12: Blockyard, a block-building game (version 1.1.0)
- **Window manager:** a mouse lock for games (`HZ_WA_MOUSE_LOCK`). While it is held:
  - the cursor is hidden and parked;
  - motion arrives as `HZ_WM_MOUSEDELTA`, and buttons and the wheel go to the window;
  - it ends with `HZ_WM_MOUSEUNLOCKED` on deactivation, hiding, the logo key or Alt+Tab.

  wintest checks it (12 checks: refused for an inactive window, the hidden and parked cursor,
  deltas, buttons, release by activation and by request).
- **Blockyard** (`user/apps/blockyard`, pinned in Start with a new cube icon; ARCHITECTURE §38):
  - **World:** an original voxel sandbox of 256 x 64 x 256 blocks generated from a seed (hills,
    mountains, beaches, water, snow, trees), saved to the documents folder and loaded at the
    next start.
  - **Renderer:** a CPU raycaster with procedural 16 x 16 textures (no image files), face shading,
    ambient occlusion, fog, see-through glass, leaves and water, a sky with sun and clouds, and
    a block outline. Rows are rendered by one worker thread per processor.
  - **Play:** first-person walking, jumping, swimming and flying with collision; breaking and
    placing 9 block types (hotbar, 1-9, wheel, middle-click picking); a pause screen with the
    controls; F3 details; adjustable resolution and view distance.
- **Tests:**
  - wintest 215 checks; apptest counts ten installed programs.
  - A new smoke case `blockyard` (35 cases), on 2 processors: Start, mouse capture, a block
    broken and a brick placed, flying, Esc, then Alt+F4 saves to D: and a second start loads
    the saved world. fsck runs on the test disk afterwards, as always.
- **Verified:**
  - **QEMU (clean build):**
    - 35/35 smoke cases with `QEMU_SMP=4`.
    - On one processor, the 34 other cases passed. `blockyard` failed once in a world where the
      original look-down left the ground out of reach. With the corrected case it passed four
      times in a row, in worlds from different (time-based) seeds.
  - **Frame rates:**
    - QEMU, 4 processors (TCG, 333x202): 20-35 fps.
    - VirtualBox 7.2.4 at 1920x1080: 66 fps (the toolkit's frame cap). Frames took 3-5 ms with
      2 render threads and 5-6 ms with one.
    - World generation: 12-16 ms in VirtualBox, about 0.3 s under TCG.
  - **Save and load:** about 124-159 KB in 17-28 ms to D:, loaded in about 20 ms; fsck finds the
    test disk clean afterwards.
  - **Screenshots:** the pause screen with the controls, walking, building (brick and planks),
    flying with F3 details, ambient occlusion on terraces and trees, and the VirtualBox help
    screen at 1920x1080. The README has `docs/images/blockyard.png`.
- **Issues found and fixed during M12:**
  - The game window was created hidden: an explicit style replaced the default one, which
    includes `HZ_WS_VISIBLE`.
  - The HUD flickered (missing text in screenshots): the compositor reads a window's surface
    while it is painted. The game now draws everything into a back buffer and copies it in one
    pass.
  - A heredoc dropped a backslash in the manifest's path.

### M11: multiprocessor, performance, polish, release (version 1.0.0)
- **SMP** (§36 of ARCHITECTURE.md):
  - Two-step bring-up of the application processors through Limine's MP response (parked on the
    kernel's page tables before bootloader memory is reclaimed, started in Phase 1), up to 8.
  - Per-CPU GDT/TSS/IST stacks, PRCB, idle thread, LAPIC and clock.
  - One global ready queue (a recorded decision), with affinity, placement on an idle or
    lowest-priority CPU, and reschedule IPIs; the SwapBusy handshake.
  - TLB shootdown by IPI (`KeFlushTb`) from unmapping and protection changes; the other CPUs are
    frozen by NMI at a bug check; a lock audit.
  - Per-CPU statistics (`HZ_SYSTEM_PROCESSOR_INFORMATION_CLASS`) and per-processor bars in Task
    Manager.
  - Four scheduler self-tests run with more than one CPU (parallel, cross-CPU wake-up,
    migration, shootdown).
  - `hz.smp=off` keeps one CPU, and `QEMU_SMP=N` sets QEMU's count.
- **Performance:**
  - A write-back FAT sector cache and multi-cluster transfers of up to 1 MiB in FAT32 (Setup's
    copy 8.0 s → 1.75 s, the file-system self-tests 3.2 s → 0.84 s under TCG).
  - `hz::fs::Copy` streams in 256 KiB pieces (no size limit).
  - A glyph cache for code points above U+00FF.
- **Polish:**
  - Notepad word wrap (View > Word wrap, Alt+Z; `TextEditor::WordWrap` works on cached rows).
  - Terminal line wrapping and `command | more`, plus the commands `lock`, `shutdown` and
    `restart`.
  - Files: cut, copy and paste of files and whole folders (Ctrl+X/C/V and buttons with new
    icons), "X - Copy" names, moves by rename on one volume, recursive delete with a counted
    confirmation, and Alt+Up/Alt+Left in the list.
  - A user password: optional in first-boot setup, settable and removable in Settings > Accounts
    (which also renames the user and the user's folder). It is stored as a salted, iterated
    SHA-256 hash (`hzcrypto`) and asked for by the lock screen, which Start > Power > Lock (or
    `lock`) shows again.
  - Settings > Time sets the time zone offset that every clock shows.
  - The masked `TextBox::Password` mode and `Window::DropNextChar`.
- **Robustness:**
  - The "Program error" dialog when a program faults.
  - The kernel's Session thread restarts the shell if it dies (with the lock screen if there is
    a password).
  - Restart and Shut down ask every program to close first (Notepad asks about unsaved work) and
    offer "Shut down anyway" after 15 s; the volumes are flushed before power-off.
- **Small fixes:**
  - The live image's files are dated by the release (`SOURCE_DATE_EPOCH`), not 1970.
  - The terminal's help table is aligned.
  - Task Manager's performance page fits its window.
  - `create-vbox-vm.sh --cpus N`.
- **Release:** version 1.0.0; README with a walkthrough and screenshots (`docs/images/`).
- **Tests:**
  - apptest has 182 checks. New ones cover `| more` parsing, editor word wrap, file trees on the
    test volume (including a 700 KB streamed copy, copying into itself refused, a move between
    folders), SHA-256 test vectors (including one million 'a's), the 10000-round password hash
    against Python's hashlib, random salts, civil time with offsets, and offset formatting.
  - New smoke cases bring the total to 34:
    - `smp`: four CPUs, the terminal and Task Manager.
    - `recovery`: the crash dialog, then killing the shell and its restart.
    - `diskboot2` now sets the password in Settings, locks from the terminal, refuses a wrong
      password and accepts the right one; `shell.ini` is checked on the host.
    - `diskboot3`: a restart with the password.
    - `poweroff`: `shutdown` with unsaved Notepad text, then "Shut down anyway" after 15 s, then
      ACPI off.
- **Verified:**
  - **QEMU 6.2 (TCG):** 34/34 smoke cases after a clean build on one processor, and 34/34 with
    `QEMU_SMP=4`.
  - **Stress:** 60 parallel 4-CPU boots through the self-tests with the hang fix, all passing
    (the hang had shown once in about forty 4-CPU suite runs).
  - **Boot time to `BOOT OK`:** about 10 s on one processor, of which 7.4 s is the
    window-manager self-tests, and 13–14 s on four (TCG slows several processors).
  - **Copy speed:**
    - Setup's copy of 11.1 MB took 1.66 s (8.0 s in M10).
    - A 700 KB file is copied and verified by apptest on every boot.
  - **VirtualBox 7.2.4 (Hyper-V/NEM backend, VMSVGA 1920x1080, an empty 16 GB system disk and the
    test disk)**, scripted by scancodes on the final ISO, on one and on two processors:
    - The live boot passed every self-test (14/14 scheduler tests with two processors).
    - Setup installed on disk 0 (11.1 MB copied in 0.48 s on one processor and 1.56 s on two;
      M10: 6.6 s).
    - The first start from the disk ran first-boot setup ("Ada", password "sun", the second
      wallpaper).
    - The third start asked for the password, accepted it and opened Start, followed by a
      60 s soak.
    - Boot to `BOOT OK`: 7.1 s on one processor, 9.5 s on two.
  - **Screenshots:** every new screen (Notepad wrapping, the pager, Files with the new commands,
    the password lock screen, first-boot setup with password boxes, Settings > Accounts and
    Time, Task Manager's per-processor bars) and the README's set in `docs/images/`.
- **Issues found and fixed during M11:**
  - **A rare 4-CPU hang** (SPIN_LOCK_TIMEOUT on the kernel address-space lock, once in about
    forty 4-CPU suite runs).
    - Cause: the clock interrupt of an idle processor walked the ready queues without the
      dispatcher lock. A thread moved to another queue under the cursor sent the walk round that
      queue's circular list for ever. The stuck processor was reaping in its idle thread and held
      the address-space lock, so everyone else waited on it.
    - Fix: the walk now happens only under a try-lock of the dispatcher lock.
    - Stop reports now list every processor's thread, IRQL and stack (recorded by the freeze NMI)
      for the next cross-processor problem.
  - **VirtualBox with 2 CPUs** (QEMU never showed any of this): the process self-test's
    spinning child process hung the system for up to 150 s, and a later boot stalled entirely.
    - The root cause is in the VM. With VirtualBox's default TSC mode on its Hyper-V backend, the
      guest TSC advanced only while guest code ran, so processors' clocks drifted apart and the
      measured TSC rate ranged from right to a third slow. `create-vbox-vm.sh` now sets
      `TM/TSCMode=RealTSCOffset`; with it a 2-CPU boot reaches the shell in 16 s, against a
      stall with the default mode.
    - VirtualBox runs every virtual timer on the first processor's thread. A thread yielding in a
      loop on processor 0 never leaves guest code, and no processor got a tick for 150 s. Such a
      yield now touches the local APIC once a millisecond (a VM exit).
    - The kernel was hardened along the way:
      - The hypervisor's timing leaf is checked against the PM timer (capped at 0.5 s).
      - Uptime falls back to the ACPI PM timer or HPET when the TSC rate is wrong.
      - Every processor's clock expires timers (one DPC each; DPC claims are atomic) and kicks busy
        processors whose clocks stall.
      - Termination IPIs the processor running the thread.
      - A yield with nothing to yield to takes no lock (a yield loop had held the dispatcher lock
        most of the time, and Hyper-V deschedules processors that spin on PAUSE).
      - A self-test line and the heartbeat report each processor's clock ticks.
  - IoTest's leak check raced with another processor's reaper (+696 bytes once). It now waits
    for thread objects and pool usage too; a real leak still shows.
  - The priority self-test failed on 4 CPUs (the woken thread ran elsewhere): it pins itself to
    CPU 0. The boot smoke case no longer expects a fixed self-test count.
  - The key that opened the lock screen's password box was typed into it. Fixed with
    `Window::DropNextChar`.
  - The key that turned a `| more` page was typed at the next prompt. The console now swallows
    it.
  - Word wrap and the terminal rounded each glyph to whole pixels, and rows drifted past the
    right edge. Widths now add up in 26.6 fixed point.
  - `hz::fs::Move` hid `hz::Move` inside the namespace (renamed `MoveTree`).
  - Alt+Up in Files was taken by the list's Up key.
- **Not done (recorded decisions):**
  - No FAT32 dirty flag: the volume is consistent between operations, and the only checker is the
    host's fsck after QEMU is killed.
  - No keyboard repeat setting: repeat comes from the PS/2 keyboard's typematic rate, which
    nothing configures yet.
  - No compositor occlusion culling or per-window Mica caches: the desktop was fast enough under
    TCG once the FAT and font work landed.

### M10: installer, first-boot setup, booting from disk (version 0.11.0)
- **Kernel:**
  - The disk driver records GPT partition unique GUIDs, makes the partition named by
    `hz.system=<GUID>` the system volume and keeps EFI system partitions without a letter. Two
    new IOCTLs on DR0: `IOCTL_DISK_GET_INFO` (model, size, partition count, read-only / in use /
    holds C:) and `IOCTL_DISK_UPDATE_PARTITIONS` (read the table again after Setup wrote one:
    partition devices updated in place or created; refused while a volume of the disk is
    mounted, or without write access).
  - The I/O manager registers storage as system / live / hidden: C: is the system partition when
    one is named and mounts, else the live image; the live image stays mounted without a letter
    on installed systems (`\Device\Ramdisk0`).
  - The ramdisk driver exposes the new `install` boot module as `\Device\Ramdisk1` (hidden).
  - `BiGetOptionValue` for `name=value` command-line options.
- **Boot:** `install.tar` (Limine's `BOOTX64.EFI`, the kernel and symbols as on an ESP) is a
  module of the two Live entries.
- **Setup** (`user/apps/setup`, "Install Nexora OS" pinned first in Start on live systems): a
  five-page wizard (welcome and licenses, disk choice with the reason a disk cannot be used,
  confirmation with a required check box, progress, done with Restart). A worker thread writes the
  GPT (protective MBR, both headers and arrays), has the kernel read it again, formats a 100 MiB
  ESP and the rest as FAT32 (largest cluster size that keeps a valid FAT32 cluster count), copies
  the boot files, the raw live image (`boot\ramdisk.tar`) and a limine.conf naming the system
  partition to the ESP, and `C:\Nexora` to the system partition with `shell.ini` marked for first
  boot. On the installed system's first start `setup.exe firstboot` asks for the user's name and a
  wallpaper and creates `C:\Users\<name>\Documents`.
- **Shell:** settings in `C:\Nexora\Settings\shell.ini` (user name, wallpaper, recent programs),
  saved on change on installed systems; first-boot setup instead of the lock screen on the first
  start; `HZ_SHELL_REQUEST_SETTINGS` to reload; the user's name in Start and on the lock screen.
- **Libraries:** `hz::fs::Settings` (`Key=Value` files), `SettingsPath`, `IsLiveSystem`,
  `DocumentsFolder` (Files, the terminal and the file dialogs start there).
- **Scripts:** `run-qemu.sh none` boots from the disk without a CD; `create-vbox-vm.sh` gained
  `--wipe-disk` (a new empty system disk) and `--boot-disk` (eject the ISO).
- **Tests:**
  - apptest 151 checks (adds CRC-32, GUIDs, the GPT layout and sectors, FAT32 geometry and
    sectors, the boot menu, user names, settings files, long `String::Format` results);
  - fstest: disk information, partition updates refused without write access and while a volume is
    mounted, partition unique GUIDs; the live-image checks go through `\Device\Ramdisk0`, so they
    pass whichever volume is C: (iotest likewise);
  - three new smoke cases (30): `install` (Setup by keyboard on a blank 384 MiB disk, then on the
    host: `sgdisk -v`, `fsck.fat -n` on both partitions, the kernel, symbols, live image, shell
    and a font compared byte for byte, limine.conf naming the system partition's GUID, Setup's
    manifest unpinned, the first-boot marker), `diskboot` (no ISO: C: is the FAT32 partition, all
    self-tests pass, first-boot setup as "Ada" with the second wallpaper, then `shell.ini` and
    `Users\Ada\Documents\Welcome.txt` checked on the host) and `diskboot2` (a restart: the lock
    screen, the saved user and wallpaper).
- **Verified:** QEMU 6.2: 30/30 smoke cases after a clean build (installing takes about 8 s of
  copying under TCG); screenshots of every Setup and first-boot page and of Start with the user's
  name. VirtualBox 7.2.4 (NEM, VMSVGA,
  1920x1080, an empty 16 GB system disk and the 64 MB test disk): the live boot passed every
  self-test; Setup listed the test disk as in use and installed on the 16 GB disk (FAT32 with
  8 KiB clusters, 2.08 million clusters; 39 files, 10.5 MB copied in 6.6 s); with the ISO
  ejected the VM started from the disk (C: = `\Device\Harddisk0\Partition2`, the test disk
  D:, all self-tests including the 16 file system tests), first-boot setup by scancodes saved
  "Ada" and the second wallpaper, and a third start showed "Hello, Ada" on the lock screen with
  that wallpaper; a 60 s soak.
- **Issues found and fixed during M10:**
  - `hz::String::Format` silently cut results at 511 bytes (a fixed stack buffer): the installed
    limine.conf was truncated and Limine stopped at a half-written module path. Longer results are
    now formatted at their full length; apptest checks a 2001-byte result and the whole menu.
  - On an installed system the kernel's live-image test mounted `\Device\Ramdisk0` during the
    file system tests, which the pool leak check reported (+295 KB); the live image is now mounted
    at boot whenever it is not C:.
  - First-boot setup showed a letter tile on the taskbar (no manifest on the installed system):
    Setup now copies its manifest unpinned instead of leaving it out.
  - An apptest length threshold was wrong (a test bug).

### M9: applications (version 0.10.0)
- **Kernel:** anonymous pipes (`io/iopipe.c`: a ring buffer between two file objects on
  `\Device\Pipe`; end of file once the writer is gone, `STATUS_PIPE_BROKEN` once the reader is
  gone, alertable waits); standard handles (`NtCreateProcessEx` hands up to three file handles
  to the child, recorded in its PEB; hzrt's `HzPrint` writes to standard output when there is
  one); `NtOpenProcess` (by process ID; the System process only for query/synchronize), so
  another process can be ended with `NtTerminateProcess`. Services 33–35 (36 in all).
- **win32k:** `GetShellWindow` (service 30, 31 in all) and `HZ_WM_SHELLREQUEST` (programs ask the
  shell to change the wallpaper); `HZ_WS_OWNED` (dialogs: not app windows, so not on the taskbar
  or in Alt+Tab); the current wallpaper in `HZ_DISPLAY_INFO`; JetBrains Mono 14 px as a system
  font.
- **Libraries:** `hz::fs` (`hzfile.h`: paths, wildcards, listing, whole-file I/O, file operations,
  volumes, formatting) and `hz::apps` (`hzapps.h`: program manifests, open-with by extension) in
  hzrt; in hzui the multi-line `TextEditor` (undo/redo, selection, clipboard, find, scrolling
  both ways), `MenuBar`, `Dialog`, `MessageBox`, `InputBox`, `ShowFileDialog`, and twelve new
  icons (folder, document, drive, prompt, calculator, chart, arrow up, pencil, trash, save, open,
  refresh). A text box given its text before the first layout now shows it from the start.
- **Programs** (each in `user/apps/<name>`, described by `C:\Nexora\Apps\<name>.app`):
  Notepad, Files, Terminal (built-in commands, external programs through a pipe, `>`/`>>`,
  history, Tab completion), Calculator (standard mode, keyboard), Settings (personalization,
  display, storage, time, about) and Task Manager (processes with CPU %, filter, sorting, End
  task; CPU and memory graphs).
- **Shell:** programs come from the manifests (Start's pinned grid, search over every program
  ranked by name before description, glyph tiles in the manifest's colours on the taskbar and in
  Alt+Tab, mapped from a window's process image), a Recent list in Start (programs launched or
  seen opening a window), and the wallpaper request.
- **Tests:**
  - usertest 140 checks (pipes: data, end of file, broken pipe, a writer thread with 40000 bytes,
    `hello.exe` with piped output, bad standard handles; `NtOpenProcess` and ending a spinning
    child; the System process protected);
  - wintest 203 checks (GetShellWindow, a request to the shell window, the wallpaper in the
    display information, owned windows not enumerated, unknown style bits);
  - uitest 144 checks (the editor in a live window: typing, indentation, undo/redo by keys,
    selection, clipboard, word delete, the wheel, a click placing the caret, find; the menu bar
    by mouse and keys; message boxes (Enter, Escape, close), the owner disabled and restored,
    input box, file dialog navigation and cancel; text set before layout);
  - the new apptest.exe, 115 checks, run at every boot after uitest (paths, wildcards and
    formatting; manifests, valid and broken, and the installed eight; the calculator engine and
    number formatting; the terminal's parser; the editor's document model);
  - the new `apps` smoke case (27 cases): unlock, Notepad types and saves `D:\smoke.txt` through
    the save dialog, the terminal types it, lists D:, writes `out.txt` with `>`, runs
    `hello.exe` through a pipe (`[exit code 42]`), starts Clock and Calculator; the calculator
    computes 12 x 3; Task Manager filters "clock" and ends it; Files opens `smoke.txt` by
    type-ahead in Notepad; Settings picks a wallpaper through the shell; afterwards fsck.fat and
    mtools check the volume and the two files' contents.
- **Verified:** QEMU 6.2: 27/27 smoke cases after a clean build; screenshots of Start, Notepad
  (find bar, unsaved-changes prompt), the save dialog, the terminal, Calculator, Files, Settings
  (personalization, about) and Task Manager (processes, performance). VirtualBox 7.2.4 (NEM,
  VMSVGA, 1920x1080, test disk attached): 3 boots, all `BOOT OK` with wintest 203/203, uitest
  144/144 and apptest 115/115 and nothing left behind; scancodes unlocked it, started Notepad
  from Start, saved `D:\vbox.txt` through the save dialog, and the terminal typed it, listed D:
  and ran `hello.exe` through a pipe (`[exit code 42]`); Calculator (1234.5 x 2 = 2469), Task Manager and Files started from Start; a 60 s soak.
- **Issues found and fixed during M9:**
  - `strstr` is not exported to user mode (kernel rtl only): usertest uses its own helper.
  - Dialogs appeared as extra taskbar buttons (they were app windows): `HZ_WS_OWNED`.
  - Start search for "files" started Notepad (its description says "text files" and it comes
    first): results are now ranked by name before description.
  - Files' address box was empty: a text box given text before its first layout scrolled the
    text out of view (zero width); fixed in hzui with a uitest check.
  - Found in review: owner windows still received their own shortcuts (Ctrl+S) while a dialog
    was open; the unhandled-key hook is now skipped while the content is disabled.
  - Two uitest editor checks clicked a line scrolled out of view (a test bug).

### M8: the shell (version 0.9.0)
- **win32k for a shell:** `RegisterShell` (one shell process at a time, ending with its window),
  `HZ_WM_SHELLNOTIFY` about app windows of other processes (created, destroyed, activated,
  changed), `EnumWindows`, a work area set by the shell (maximized windows follow it), the shell
  may show/minimize/restore any window, the logo key and Alt+Tab (until Alt is released) go to
  the shell, a system clipboard (`SetClipboardText`/`GetClipboardText`, 64 KiB of text), three
  procedural wallpapers (`SetWallpaper`, swapped between frames). Services 25–30.
- **Compositor:** `HZ_WS_TRANSPARENT` (per-pixel alpha over the windows below) and
  `HZ_WS_NOFRAME` (square, no border, no shadow); wallpaper variants and the frame mutex.
- **shell.exe** (`user/shell`): lock screen with a large clock over the wallpaper; taskbar
  with Start, window buttons (activate, minimize, restore; active indicator; tooltips), quick
  settings button and clock; Start menu with search, pinned programs and a power menu; quick
  settings with toggles, sliders and wallpapers; the Alt+Tab switcher; program launching.
- **Programs:** `clock.exe` (`user/apps/clock`, the clock moved out of welcome), welcome is now
  a single window started by the shell. hzui's clipboard is the system clipboard; six new icons.
- **Kernel start-up:** starts the shell (falling back to welcome).
- **Tests:**
  - wintest grew to 190 checks: shell registration and its rules, notifications about a second
    wintest process's window, enumeration, shell control of that window, work area and
    maximized windows following it, the logo key alone and with another key, Alt+Tab
    events and cancel, title changes, destruction, the clipboard, wallpapers, unregistering,
    transparent and frameless windows.
  - Smoke: `boot` expects the shell's lock screen; `desktop` unlocks first; the new `shell` case
    opens Start with the logo key, searches "clo" and starts Clock, Alt+Tabs back to Welcome,
    activates/minimizes/restores Clock from the taskbar and picks a wallpaper in quick settings,
    all through QEMU's PS/2 keyboard and mouse. 26 cases.
- **Verified:** QEMU 6.2: 26/26 smoke cases after a clean build; screenshots of the lock
  screen, desktop, Start (before and after searching), the switcher, quick settings and the new
  wallpaper. VirtualBox 7.2.4 (NEM, VMSVGA, 1920x1080): 3 boots, all `BOOT OK` with wintest
  190/190 and uitest 104/104 and nothing left behind, ending on the lock screen; scancodes
  unlocked it, opened Start with the logo key (E0 5B), searched "clo" and started Clock with
  Enter, and Alt+Tab switched back to Welcome; a 60 s soak with both clocks updating.
- **Issues found and fixed during M8:**
  - The taskbar showed through the transparent lock screen; it is now hidden until unlocking.
  - Start and the Welcome program had the same icon (the logo); Welcome now has its own.
  - A wintest wallpaper check sampled its reference pixel before the compositor had redrawn the
    area of a window that had just closed (a stale pixel, so the test was flaky); it flushes
    first now.
  - A first screenshot of the switcher was taken too early because of QEMU's per-event hold
    time (not an OS bug; found by logging every key event in win32k temporarily).

### M7: the C++ UI toolkit, hzui (version 0.8.0)
- **win32k additions:** minimize and maximize caption buttons (`HZ_WS_MINIMIZEBOX`,
  `HZ_WS_MAXIMIZEBOX`) with hover and pressed state reported to the owner
  (`HZ_WM_CAPTIONSTATE`); window states (minimize hides, maximize and restore are resize requests,
  caption double-click, dragging a maximized window restores it under the cursor, Alt+Tab
  restores minimized windows, `HZ_SW_MINIMIZE`/`MAXIMIZE`/`RESTORE`, state bits in window info,
  `HZ_SIZE_*` in WM_SIZE); cursor shapes (arrow, I-beam, hand, four resize arrows) chosen from
  borders, drags and the client cursor of the window under the pointer; explicit capture
  (`SetCapture`, `HZ_WM_CAPTURECHANGED`, cancelled by activation changes); per-window minimum
  size; work area and cursor shape in display info. Services 23–24: SetCapture,
  SetWindowAttribute. The compositor draws maximized windows square and borderless, popups
  rounded, and the cursor by shape.
- **hzuser:** the caption with minimize, maximize/restore and close buttons and their hover and
  pressed looks; new calls for window states, cursors, minimum sizes and capture; two more font
  sizes (20 and 12 px), font files shared between sizes, length-bounded text measuring and
  drawing.
- **hzrt:** global constructors (`.CRT$XC*`), `atexit`, `_purecall`; hzcore (`Vector`,
  `String`, `UniquePtr`, `Function`).
- **hzui** (`user/lib/hzui`, about 3700 lines): elements with two-pass layout, StackPanel, Grid,
  ScrollViewer; damage-driven painting with an anti-aliased Painter and procedural icons; text
  layout with wrapping and ellipsis; pointer routing with press capture and hover tracking;
  focus and Tab order with focus rings after keyboard use; transitions and timers; Button,
  ToggleSwitch, CheckBox, RadioButton, Slider, ProgressBar, TextBox, ComboBox, ListView
  (virtualized), ScrollBar, TabView, menus, tooltips; a dark theme with the Nexora orange accent.
- **Programs:** `uitest.exe` (104 checks, run at every boot after wintest); `welcome.exe`
  rewritten as the hzui gallery (Start, Controls, List and About tabs, clock window).
- **Tests:**
  - wintest grew to 139 checks (the window manager additions); `W32kRunSelfTests` runs both
    programs and checks that no window or surface is left.
  - The `desktop` smoke case now also tabs to a button, presses it with Space, comes back with
    Shift+Tab and types again; 25 cases in total.
- **Verified:** QEMU 6.2 (TCG, 1024x768): 25/25 smoke cases after a clean build; screenshots of
  every gallery tab, the focus ring, a tooltip, a menu, the filtered list and a maximized window
  with the close button hovered.
  VirtualBox 7.2.4 (NEM, VMSVGA, 1920x1080): 3 boots, all `BOOT OK` with wintest 139/139 and
  uitest 104/104 (about 4.4 s together) and nothing left behind; scancodes typed into the
  gallery, tabbed to Clear and pressed it, came back with Shift+Tab, typed again and switched
  tabs with Ctrl+Tab (focus ring shown); a 60 s soak with the clock updating.
- **Issues found and fixed during M7:**
  - `hz::Function` built from a plain function name tried to store a function type; functions
    now decay to pointers.
  - The selected tab's title (semibold) was measured with the regular font and got an ellipsis;
    headers now reserve the semibold width.
  - Switching tabs hid the focused text box and cleared the focus, so Ctrl+Tab stopped working;
    focus in the strip or in the hidden page now follows the selected header. Tab from a header
    walked through every other header before the page; the strip is now one tab stop (only the
    selected header is focusable, arrows switch tabs).
  - A window shown with the focus already set never started the text box's caret timer; showing
    a window now notifies the focused element.
  - Test bugs (not product bugs): string offsets off by one, a byte count of 14 for a 13-byte
    string, and a pixel probe placed exactly at the cursor's hotspot (the arrow's tip is black).
- **Not exercised by tests:** caption double-click through the real PS/2 path (QEMU monitor
  commands are ~0.3 s apart, slower than the 500 ms double-click time); SendInput covers it.

### M6: win32k: regions, sections, compositor, windows, queues, input, fonts (version 0.7.0)
- **Sections** (`mm/section.c`): shared memory objects with resident pages; views in user
  address spaces (section VADs, `MEM_MAPPED`) and in system space; services 31–33
  (CreateSection, MapViewOfSection, UnmapViewOfSection); usertest covers them (113 checks).
- **Regions** (`win32k/rgn`, Rust): canonical y-x banded regions with union, intersect, subtract,
  xor, offset, point/rectangle tests, bounds, area and equality through a C ABI; boot self-test
  with 200 random property tests against a bitmap.
- **win32k** (C++20): top-level windows with generation-checked handles, z-order with topmost
  windows, surfaces in sections, show/hide/move/resize, activation; per-thread message queues
  with generated WM_PAINT and WM_TIMER, mouse-move coalescing and alertable waits; thread-exit
  cleanup through a Ps callout; 22 services in a second service table at 0x1000.
- **Compositor:** a thread composing dirty rectangles into a back buffer and copying them to the
  framebuffer: procedural "Nexora dusk" wallpaper, Mica backdrop (blurred, tinted),
  8 px anti-aliased rounded corners, soft shadows (lighter for inactive windows), a 1 px border
  and a software cursor; integer math only. The boot screen is handed over after `[HZ] BOOT`.
- **Input routing:** PS/2 devices and `NtUserSendInput` share one path: hit testing (caption
  move, border resize requests, close button), click activation, implicit capture, double-click,
  mouse leave, wheel, keyboard to the active window, Alt+F4 and Alt+Tab.
- **User mode:** the hzrt heap (`malloc`/`free`/`new`/`delete`); `hzmath.h`; hzuser (window table
  and procedures, message loop, caption drawing, canvas drawing with alpha, Inter text with
  stb_truetype and a glyph cache) as `hzuser.lib`; the fonts in `C:\Nexora\Fonts`.
- **Programs:** `wintest.exe` (92 checks, run at every boot by `W32kRunSelfTests`, which also
  checks that no window or surface section is left) and `welcome.exe` (the desktop: a Mica
  welcome window that echoes typing, a clock window driven by a timer).
- **Tests:**
  - Boot case: `Window manager self-tests: ... OK`, and `welcome: desktop windows created`.
  - New `desktop` case: QEMU types "hi" + Enter into the welcome window and drags it by its
    caption through the PS/2 devices (`welcome: you typed "hi"`, `welcome: moved to (192, 284)`).
    25 cases in total.
- **Verified:**
  - QEMU 6.2 (TCG, 1024x768 GOP): 25/25 smoke cases after a clean build; the boot and desktop
    screenshots show the desktop.
  - VirtualBox 7.2.4 (NEM, VMSVGA, 1920x1080 GOP): 3 boots, all `BOOT OK` with wintest 92/92 and
    no windows or surfaces left (about 310 ms); compositor buffers 23.7 MiB; typing through
    `keyboardputscancode` reaches the welcome window (`welcome: you typed "hi"`); a 60 s soak
    with the clock window updating every second.
- **Issues found and fixed during M6:**
  - The shadow was skipped for the whole window rectangle, so the unshadowed wallpaper showed as
    a light square under each rounded corner; it is now skipped only under fully opaque pixels.
  - The smoke harness parsed QEMU's PPM header with `split()`, which also ate leading pixel
    bytes that happen to be whitespace values (the black boot screen never had any), producing
    truncated PNGs of the desktop; it now slices after the header and waits for the whole file.
  - wintest left the cursor where its last injected drag ended, so the desktop smoke case's
    relative mouse moves started from the wrong place; wintest now restores the cursor.
  - The `hz.input=echo` diagnostic and win32k both read the PS/2 queues, which would split the
    events between them; win32k now leaves the devices to the diagnostic when it is enabled.
  - The boot clock check measured 660 and 685 Hz in 2 of 25 TCG boots: when the host delays
    QEMU's virtual CPU, periodic LAPIC interrupts that fell due meanwhile coalesce (the HPET and
    the TSC agreed that 200 ms had passed). A misprogrammed timer is wrong every time, so the
    window is now retried up to three times before the check fails.
  - A wintest check expected creation to enforce the interactive minimum size (160x64); popups
    must be allowed smaller, so the minimum applies to interactive resizing only (documented in
    `uapi/hzuser.h`) and the test checks empty and oversized rectangles instead.

### M5: I/O manager, PCI, PS/2, AHCI, VFS + FAT32 (version 0.6.0)
- **I/O manager** (`io/`): driver, device and file objects; synchronous IRPs with nine major
  functions; built-in driver table in `.hzdrivers` (ordered bus → file systems → class → devices);
  storage devices with VPBs mounted on first use by the registered file systems; drive letters
  (`\??\C:` = ramdisk, `D:`... for other volumes); raw device opens; the kernel file API
  (`IoCreateFile`, `IoReadFile`, ..., `IoReadWholeFile`); Ob parse context for open parameters.
- **System services** (30 in total, `hzsys` stubs and prototypes updated): CreateFile, ReadFile,
  WriteFile, Query/SetInformationFile (basic, position, end-of-file, disposition, rename, volume),
  QueryDirectoryFile, DeviceIoControlFile, ShutdownSystem, QuerySystemTime. Buffered I/O through
  pool bounce buffers.
- **Drivers:** PCI (ECAM/ports, bridges, BAR sizing, MSI), AHCI (firmware takeover, LBA48 DMA,
  MSI + polling fallback, error recovery), disk class (GPT with CRCs, MBR, partition devices,
  IOCTLs), ramdisk block device, PS/2 keyboard + IntelliMouse (set 1 decoding, US layout,
  `\Device\Keyboard0`, `\Device\Mouse0`, `hz.input=echo`).
- **File systems:** UstarFs (read-only C:; process images are now read through the I/O path) and
  FAT32 read/write (long names, short-name generation with NT case bits, create/overwrite,
  write with gap zero-fill, truncate/extend, delete-on-close and dispositions, rename across
  directories with `..` fix-up, enumeration, timestamps, FSInfo).
- **Time and power:** CMOS RTC → system time (HZ time, UTC); ACPI S5 power off (DSDT `\_S5`
  scan, ACPI-mode switch) and reset (FADT reset register, 8042, 0xCF9, triple fault);
  `ExShutdownSystem` flushes volumes first.
- **Ke:** `KeConnectInterruptAnyVector` (vector allocation per IRQL band), `KeQuerySystemTime`.
- **Rtl:** UTF-8/UTF-16 conversion, CRC-32, calendar → time conversion.
- **Tests:**
  - `File system self-tests: 16/16 passed` (`io/iotest.c`): C: reads/listing/write refusal; raw
    300-sector round trip on the scratch partition; FAT32 known files (8.3, long name, 300 KB
    pattern read in odd chunks), create + 100000-byte unaligned writes, truncate/extend
    zero-fill, rename, delete rules, 40-file directory growth, free space restored, a kept tree
    for host checks; `fstest.exe` (61 checks from user mode, including 1 MiB transfers and raw
    disk reads); file object and pool leak check.
  - `scripts/mkdisk.sh` / `make testdisk`: the GPT + FAT32 test disk. The smoke boot case runs
    `fsck.fat -n` on it afterwards and reads the kernel's files back with mtools.
  - New smoke cases: nodisk, input (QEMU `sendkey`/`mouse_move` through the echo threads),
    shutdown, reboot (QEMU must exit cleanly). 24 cases in total.
- **Verified:**
  - QEMU 6.2 (TCG, q35 with ICH9 AHCI + MSI, i8042): 24/24 smoke cases; `fsck.fat` clean after
    the kernel's and fstest's writes.
  - VirtualBox 7.2.4 (NEM backend; PIIX chipset without MCFG, so PCI uses ports; IntelAhci without
    MSI, so completion is polled): with the test disk attached as SATA port 2 (`--test-disk`),
    5 boots (3 normal, 2 verbose) all `BOOT OK` with Memory 8/8, Scheduler 10/10, Process 12/12,
    File system 16/16 (about 4 s; re-runs on the persistent disk clean up after themselves), the
    blank 16 GB disk reported as "no partition table", the DVD (ATAPI) left alone; keyboard
    input through `VBoxManage controlvm ... keyboardputscancode` reaches the echo thread; the
    shutdown diagnostic powers the VM off by itself (ACPI S5); page-fault stop screen; 60 s soak
    clean.
- **Issues found and fixed during M5:**
  - `D:\` opened the raw partition instead of the volume root: the object manager consumes the
    trailing separator before it reaches the device, so `IoCreateFile` now treats a storage
    device reached through a path ending in `\` as "the root of its volume" (NT semantics:
    `\\.\D:` is the device, `D:\` the root directory).
  - The PS/2 configuration byte was read while both ports were disabled, so writing it back with
    the IRQ bits set switched both device clocks off again (no input at all). The final write now
    clears the clock-disable bits of the ports in use.
  - `sgdisk -q` silently writes nothing on this host's version; `scripts/mkdisk.sh` runs it
    without `-q`.
  - VirtualBox's AHCI has no MSI, and with polled completion every command waited for a clock
    tick (late under NEM): the file system tests took 6-8 s. Without MSI the driver now spins on
    the command-issue register for up to 2 ms before sleeping (about 4 s).
  - A verbose VirtualBox boot failed KeTest once: the 5 ms periodic timer fired 7 times in 52 ms
    (bound: 8-12). Late ticks make the timer skip missed periods by design; the lower bound is
    now 5, which still proves that it repeats.

### M4: object manager, system calls, user mode, PE32+ loader (version 0.5.0)
- **Object manager** (`ob/`): 64-byte `OBJECT_HEADER` in front of every object, typed objects
  with pool tags, access masks, delete/close/parse procedures; atomic reference counts with
  deferred deletion on the new **worker thread** when the last reference drops at DISPATCH_LEVEL;
  per-process **handle tables** (`(index+1)*4`, granted access, growth, pseudo handles -1/-2);
  **namespace** with directories, symbolic links and parse procedures (`\ObjectTypes`, `\Device`,
  `\Driver`, `\??`, `\KernelObjects`; `\??\C:` → `\Device\Ramdisk0`). `OBJECT_REFCOUNT_CORRUPT`.
- **I/O lite** (`io/iomgr.c`): Device and File types; the ramdisk device resolves paths into
  file objects over the archive bytes.
- **Processes and threads** (`ps/`): `EPROCESS`/`ETHREAD` objects embedding `KPROCESS`/`KTHREAD`
  (waitable); the System process (PID 4) and `PsCreateSystemThread` for every kernel thread;
  `PsCreateProcess` from an object path; user threads with XSAVE area, 1 MiB stack + guard page,
  TEB (GS base) and PEB; termination through the **user-return callout** + alertable waits
  (replaces APCs); exit status, process signaling, handle-table rundown, deferred deletion.
- **System calls:** SYSCALL entry building a full `KTRAP_FRAME` (`hal/syscall.asm`), dispatcher
  with stack arguments 5–8 (`ke/syscall.c`), 21 services (`ex/svctable.c`; contracts in
  `include/uapi/hzapi.h`), exit through `iretq`. `IRQL_GT_ZERO_AT_SYSTEM_SERVICE`.
- **User boundary:** `MmCopyFromUser`/`ToUser`/`StringFromUser` with fault recovery
  (`mm/usercopy.asm`), **SMAP** enabled when available, SMEP/SMAP stop codes; user exceptions
  become NT statuses and terminate only the process (`ACCESS_VIOLATION`, `STACK_OVERFLOW`,
  `INTEGER_DIVIDE_BY_ZERO`, `PRIVILEGED_INSTRUCTION`, `ILLEGAL_INSTRUCTION`, ...); user `int3` is
  logged and continues.
- **Extended state:** CR0/CR4/XCR0 set up for x87/SSE(/AVX); XSAVE (or FXSAVE) per user thread
  on context switch; `MSR_KERNEL_GS_BASE` per thread.
- **Virtual memory:** commit inside reservations (VAD splitting with a shared `AllocationBase`),
  release of a whole split reservation, `MmProtectVirtualMemory` (splits), `MmQueryVirtualMemory`,
  `MEM_IMAGE` ranges.
- **PE32+ loader** (`ps/peimage.c`, `ps/peload.c`): full bounds-checked validation, image built in
  a kernel buffer, DIR64 relocations (forced-relocation test), per-section protections, imports
  and TLS rejected.
- **User runtime and programs** (`user/`): `hzsys` NASM stubs, `hzrt` (startup with
  `main(argc, argv)`, thread thunk, `HzPrint`, string/format from `rtl/`), `hello.exe` (the first
  user program: prints via `NtDisplayString`, exits 42), `usertest.exe` (101 checks: bad
  pointers/handles/service numbers, events and waits, VM, threads and their termination
  while spinning or blocked, 8-thread WaitAll, SSE/x87 (and AVX when present) state across
  context switches, system information, child processes and remote termination), `crash.exe`
  (null, kernel address, cli, divide, ud2, stack overflow, NX, int3).
- **Self-tests:** `Process self-tests: 12/12 passed` every boot (`ps/pstest.c`), including a leak
  check (no process/thread/event/file objects left, pool back to the same byte count). The Mm
  address-space test now covers user copies, commit splitting, protection and queries.
- **Diagnostics:** `hz.test=smap` (kernel reads a user page directly →
  `SUPERVISOR_MODE_USER_ACCESS`). 22 smoke cases; the boot case checks the hello output, the
  usertest result and 12/12, the verbose case checks the namespace dump, relocations, process
  deletion and the leak line.
- **Verified:**
  - QEMU 6.2 (TCG, `-cpu max`: SMEP+SMAP, XSAVE without AVX): 22/22 smoke cases.
  - VirtualBox 7.2.4 (NEM backend; XSAVE with AVX, no SMEP/SMAP reported): 3× normal boot and
    the verbose boot all `BOOT OK` with Memory 8/8, Scheduler 10/10, Process 12/12, usertest
    101 checks / 0 failed **with the AVX state test**, leak line `0 ... pool +0 bytes: OK`; the
    user-mode tests take about 0.25 s. pagefault and threadoverflow stop with the right code and
    thread; `smap` runs but cannot stop (the CPU reports no SMAP, logged as a warning). 60 s soak
    clean (status every second, ~1000 interrupts/s idle).
- **Issues found and fixed during M4:**
  - `\??` could not be created: the namespace name check rejected `?`. Object names now only
    exclude `\` and control characters (file-name rules belong to file systems).
  - The Mm address-space self-test failed after the VAD rewrite: it committed at a fixed address
    with `MEM_COMMIT` alone, which now (Windows semantics) means "commit inside an existing
    reservation". The test now uses `MEM_RESERVE | MEM_COMMIT`.
  - KeTest's leak check expected exactly 2 threads; the worker thread made it 3. It now compares
    against the count before the tests.
  - Info-class constants shared names with the structure typedefs in `hzapi.h`
    (`HZ_SYSTEM_BASIC_INFORMATION`); they are now `*_CLASS`.

### M3: threads, scheduler, synchronization, DPCs (version 0.4.0)
- **Threads** (`ke/thread.c`): `KeCreateThread` (guarded stack, initial frame into
  `KiThreadStartupStub`), `KeTerminateThread`, reaper, reference counting. `KPROCESS` /
  `KeSystemProcess`. The boot context becomes the **idle thread**; init continues in
  **Phase1Init**.
- **Scheduler** (`ke/dispatch.c`, `ke/ctxswap.asm`):
  - 32 priority queues with a summary bitmap;
  - preemption and rescheduling through the **DISPATCH software interrupt** (vector 0x2F, LAPIC
    self-IPI, EOI before switching);
  - **10 ms quanta charged in TSC cycles**;
  - `KeYieldThread`, `KeSetPriorityThread`, per-thread CPU time.
- **Synchronization** (`ke/wait.c`): `KeWaitForSingleObject` / `KeWaitForMultipleObjects`
  (WaitAny/WaitAll, timeouts, polling, caller wait-block arrays). Events, semaphores, mutexes
  (recursion, abandonment), timers and threads are all waitable.
- **DPCs** (`ke/dpc.c`) and **timers** (`ke/timer.c`): TSC-based due times; the clock only
  queues the expiration DPC. Periodic timers with DPCs. `KeDelayExecutionThread`.
- **Memory manager integration:** current address space per thread (preemption-safe attach);
  **zero-page thread** keeping 2048 pre-zeroed pages (`mm/zero.c`).
- **System threads:** ZeroPage (priority 1) and Status (priority 9, splash every second,
  `[HZ] HEARTBEAT` after 3 s, trace line every 30 s).
- **Stop screens** name the thread and IRQL. Boot-display drawing is serialized (Inbv lock).
- **Self-tests:** 10/10 scheduler tests every boot (`ke/ketest.c`).
- **Diagnostics:** threadoverflow, waitirql, mutexowner. 20 smoke cases in total.
- **Verified:**
  - QEMU 6.2 (TCG): 20/20 smoke cases; 1000 Hz ticks; time slicing gives 21-23 switches per
    200 ms.
  - VirtualBox 7.2.4 (NEM backend): 6× normal boot `BOOT OK` with 10/10 scheduler tests (event
    round trip ~0.28 us); threadoverflow, waitirql, mutexowner, stackoverflow, pagefault,
    doublefree and rustpanic stop with the right code and thread; 60 s soak clean (status every
    second, ~1000 interrupts/s while idle, 2048 pages pre-zeroed).
- **Issue found and fixed during M3:** time slicing failed in VirtualBox (4 context switches in
  200 ms instead of ~20).
  - **Diagnosis:** a tick count showed **19 clock ticks in 200 ms** while the guest spins, versus
    200 in QEMU. VirtualBox runs on Hyper-V's platform API here (NEM, "AMD-V is not available"),
    and the emulated LAPIC timer is serviced on VM exits.
  - **Fix:** quanta are charged in TSC cycles (as NT has done since Vista), so a late tick still
    charges real time: 21-22 switches in VirtualBox.

### M2: memory manager (version 0.3.0)
- **PFN database** (`mm/pfn.c`): 16-byte entries for all RAM, free and zeroed lists, zero on
  demand, contiguous DMA allocation, double-free detection (`PFN_LIST_CORRUPT`).
- **Own page tables** (`mm/paging.c`):
  - W^X kernel image (RX text, R+NX rodata, RW+NX data), NX, global pages, SMEP when available;
  - direct map of RAM only, with 2 MiB pages;
  - WC framebuffer, HAL MMIO window carried over;
  - all kernel PML4 slots preallocated for sharing;
  - PAT verified.
- **Loader memory reclaimed** after the CR3 switch (27–37 MiB).
- **System VA allocator** (`mm/sysva.c`, 256 GiB, trailing guard gaps). **Address spaces**:
  create, switch, delete. **VADs** with reserve/commit and **demand-zero page faults**
  (`mm/vad.c`); faults at raised IRQL → `IRQL_NOT_LESS_OR_EQUAL`. **`MmMapIoSpace`**.
- **Guarded 32 KiB kernel stacks** (`mm/stack.c`); overflow → #DF → `KERNEL_STACK_OVERFLOW`.
  `KiCallOnStack` (`ke/ctxswap.asm`).
- **Pool** (`mm/pool.c`, `include/ex.h`): tagged slab/large allocator with checksummed headers,
  `BAD_POOL_HEADER` / `BAD_POOL_CALLER` with readable messages, free fill, per-tag stats. C++
  `new/delete` on the pool.
- **Boot self-tests:** 8/8 every boot (`mm/mmtest.c`). The splash shows free memory.
- **Diagnostics:** stackoverflow, poolcorruption, doublefree, writero, nxexec. There is also a
  `verbose` smoke case that boots the trace-log entry.
- **Build:** `-fno-optimize-sibling-calls` in kernel C/C++, so tail calls keep their frames in
  stack traces.
- **Verified:**
  - QEMU 6.2 (TCG): 17/17 smoke cases.
  - VirtualBox 7.2.4: 2× normal boot (`BOOT OK`, 8/8 memory tests in 7 ms, boot in about 250 ms)
    plus stackoverflow, poolcorruption, doublefree, writero, nxexec and pagefault, each with the
    right stop code and message.
- **Issues found and fixed during M2:**
  - A name clash between the `MiPfnDatabase` enum value and variable.
  - The pool-corruption trace lost its caller to a tail call; fixed by the flag above, not by
    loosening the test.
  - The verbose case expected a per-tag pool line although nothing was allocated at that point
    (no leaks, correctly). Now a summary line always prints.
  - **Intermittent timer mis-calibration in VirtualBox** (a boot ended `DEGRADED`: TSC calibrated
    at 3534 instead of ~4400 MHz, clock measured 746 Hz). Instrumented windows showed the virtual
    clock racing ahead in the first 1–2 windows after boot (catch-up), which defeated the
    median of 3. Fixes:
    - calibrate until three windows agree within 0.5 %;
    - use the hypervisor timing leaf when present, with the VirtualBox VM set to the "minimal"
      paravirt provider (`HZ_VBOX_PARAVIRT` overrides);
    - the clock self-test compares ticks with the reference timer instead of the TSC.

    Verified with 6+3 VirtualBox boots on each path plus QEMU.

### M1: HAL (version 0.2.0)
- **Processor tables** (`hal/gdt.c`, `hal/idt.c`, `hal/trap.asm`): SYSRET-ordered GDT, TSS with IST
  stacks for #DF/NMI/#MC, `KPRCB` through GS base, 256-vector IDT, uniform `KTRAP_FRAME` with a
  fake frame linking stack walks into interrupted code, `swapgs` prepared for user mode.
- **Traps** (`ke/trap.c`): kernel exceptions → typed bug checks with human-readable messages
  ("Page fault writing 0x… (page not present) in KiTestPageFault+0xe"); `int3` logs and resumes.
  Interrupt dispatch raises IRQL (CR8), re-enables interrupts, calls the connected routine,
  EOIs and restores. `KeConnectInterrupt` / `KeDisconnectInterrupt`.
- **IRQL and spinlocks** (`include/ke.h`, `ke/spinlock.c`): CR8-based IRQL with violation bug
  checks, TTAS spinlocks with a deadlock timeout.
- **Stop screen v2** (`init/inbv.c`, `ke/bugcheck.c`): register grid for traps, CR2/CR3,
  symbolized stack traces (return addresses via `addr-1`), page-table-checked stack walking,
  nested-bug-check report, compact layout for ≤900 px screens, JetBrains Mono for dumps.
- **Kernel symbols** (`tools/mksym.py`, `ke/symbols.c`): `hzkrnl.sym` boot module, 399 code
  symbols including demangled C++ and Rust, validated against the running kernel.
- **Logger** (`ke/debug.c`): levels, uptime stamps, WARNING/ERROR tags, HIGH_LEVEL lock,
  `hz.log=trace|warning`.
- **HAL I/O space** (`hal/iospace.c`): uncached NX MMIO mappings in PML4 slot 508.
- **ACPI** (`hal/acpi.c`, `include/acpi.h`): RSDP/XSDT/RSDT with checksums; MADT (CPUs, I/O
  APICs, overrides, LAPIC NMIs, x2APIC entries), FADT (PM timer, reset register, century, 8042),
  HPET.
- **APIC** (`hal/apic.c`, `hal/pic.c`): 8259s masked, LAPIC xAPIC/x2APIC, error/spurious
  vectors, I/O APIC routing with ISA overrides.
- **Timers** (`hal/timer.c`, `ke/time.c`): HPET > PM timer > PIT reference, TSC + LAPIC
  calibration (median of 3), 1000 Hz periodic clock, uptime from kernel entry, TSC stalls.
- **Boot self-tests:** int3 round trip, clock rate vs TSC, I/O APIC delivery through
  IRQ 0 → GSI 2. The splash status line shows live uptime and interrupt count from the idle loop
  (`hlt`), and serial gets a one-time `[HZ] HEARTBEAT` after 3 s.
- **Diagnostics** (`init/diag.c`, Limine *Diagnostics* menu, `make test` cases): bugcheck,
  rustpanic, pagefault, divide, invalidop, gpf, doublefault, assert, irql. There is also a
  "Live, verbose log" menu entry.
- **Verified:**
  - QEMU 6.2 + OVMF (TCG), 10/10 smoke cases, with HPET as the reference timer.
  - VirtualBox 7.2.4, 2× normal boot plus all 9 diagnostics, with the ACPI PM timer as reference
    (no HPET). The correct stop code and symbolized frames appear on serial and screen;
    `BOOT OK` has no warnings. It reaches the idle loop about 205 ms after kernel entry.
- **Bugs found and fixed during M1:**
  - The TSS struct was 8 bytes too long (a reserved field plus `Ist[8]`); caught by a static
    assert.
  - Symbolization dropped every `__`-prefixed function (e.g. `__rustc::rust_begin_unwind`), so
    Rust panic frames named the wrong function. Only linker markers (`__[A-Z]…`) are filtered now.
  - The I/O APIC self-test saw 3/50 PIT interrupts in VirtualBox because it busy-waited, and
    VirtualBox services emulated devices on VM exits. It now waits with `hlt` (see ARCHITECTURE §17).

### M0: toolchain and first boot (version 0.1.0)
- Pinned toolchain (LLVM 22.1.8, Rust 1.98.1, Limine 11.4.1) with a no-root installer and a
  Dockerfile that uses the same script. Makefile targets `iso run test vbox toolchain clean`.
- Kernel linking C17 + NASM + C++20 + Rust; Limine base revision 6 loader block; COM1; boot
  splash with procedural logo and Inter text (build-time glyph baking); bug checks; PE32+
  user toolchain (`hello.exe` in the ramdisk); USTAR ramdisk; headless smoke test; VirtualBox
  VM script.
- Fixed in M0: the COM1 presence probe raced VirtualBox's asynchronous loopback.

## Beyond 1.0 (ideas, not planned)

- Per-CPU ready queues with balancing, if lock contention ever shows on more than 8 CPUs.
- A real cache manager (directory and data sectors), asynchronous I/O, NVMe and USB HID.
- Users and security (access checks on objects and files), DLLs, and an AML interpreter.
- Keyboard layouts and repeat settings, daylight saving rules, a light theme and DPI scaling.

## Known issues and limitations

- **Docker path not exercised yet.** The development machine has no Docker. The Dockerfile and
  `scripts/build.sh` docker mode are written but untested; the native path uses the same pinned
  toolchain.
- QEMU in the reference WSL2 distro runs with **TCG** (the user is not in the `kvm` group).
- **VirtualBox on the Hyper-V (NEM) backend with more than one processor** (this host's
  configuration):
  - It needs `TM/TSCMode=RealTSCOffset`, which `create-vbox-vm.sh` sets.
  - A user thread spinning without system calls on processor 0 would still stall VirtualBox's
    virtual timers (they run on that processor's thread). Nothing in Nexora OS does that.
  - Roughly one 2-CPU boot in seven pauses for about a minute before the kernel's first timer
    line (seen before and after the M11 timer changes), then completes normally. The cause was
    not found; it looks like it is in the hypervisor.
  - One CPU has none of these problems.
- **WSL interop can disappear** (`VBoxManage.exe: cannot execute binary file: Exec format error`)
  after systemd's `systemd-binfmt` runs; `wsl --shutdown` or a WSL restart brings it back.
  Workaround: run `bash scripts/create-vbox-vm.sh` from Git Bash. Permanent fix:
  `echo ':WSLInterop:M::MZ::/init:PF' | sudo tee /usr/lib/binfmt.d/WSLInterop.conf`.
- The HAL I/O window uses a 16-page static page-table pool (enough for ~28 MiB of scattered MMIO).
  Device drivers map through `MmMapIoSpace` (PCI ECAM maps 1 MiB per bus it scans).
- In VMs the tick count runs a few percent fast for the first ~0.3 s (hypervisor clock
  catch-up), and under VirtualBox's NEM backend ticks arrive late while the guest spins (19 per
  200 ms). Anything measuring time must use the TSC (`KeQueryUptimeMicroseconds`); quanta and
  timers already do.
- Scheduler limits (fine for now):
  - one global ready queue for every CPU (up to 8; by decision, see ARCHITECTURE §36); only CPU 0
    keeps the tick count and expires timers;
  - static priorities (no boosts or decay);
  - no APCs: thread termination uses the user-return callout plus alertable waits; all I/O is
    synchronous, so no completion mechanism is needed yet;
  - thread and process IDs are not recycled.
- Virtual memory: the VAD list is linear (fine for now); no decommit without release; no
  no-access protection on committed pages; the VM services work on the caller's own process only.
- User mode limits (M4):
  - no DLLs or import resolution, no TLS directory (global constructors run since M7);
  - no structured exception handling: every unhandled user exception terminates the process;
  - no handle inheritance or duplication, `NtCreateThread` only in the caller's process;
  - an NMI between `syscall` and `swapgs` would run with the user GS base (NMIs only freeze the
    other processors at a bug check, where that no longer matters);
  - SMEP/SMAP are not reported by the CPU under VirtualBox's NEM backend, so `hz.test=smap` only
    stops the system in QEMU (TCG `-cpu max`); the user-copy path works either way.
- I/O limits (M5):
  - synchronous IRPs only, no IRP stack locations or filter drivers, no asynchronous/overlapped
    I/O, no cache manager (FAT32 reads and writes go to the disk every time);
  - AHCI only (no NVMe, legacy IDE or ATAPI reads; the boot DVD is ignored after boot), LBA48 and
    512-byte logical sectors only, one command at a time per port through a 64 KiB bounce buffer;
  - no INTx routing (needs AML `_PRT`): devices without MSI are polled;
  - PS/2 only (no USB HID), US layout, no keyboard LED updates;
  - FAT32: no sharing modes, no access dates, no dirty-volume bit (by decision, M11), ASCII-only
    case folding; only the FAT sector is cached (write-back), not directory or data sectors;
  - no security: raw disk opens and `NtShutdownSystem` are open to every process;
  - the kernel's time is UTC; the clocks show a fixed offset chosen in Settings (no daylight
    saving rules).
- Window manager limits (M6–M7):
  - top-level windows only (by design); no window icons (the shell draws program icons from the
    manifests); owned windows (`HZ_WS_OWNED`) are only kept off the shell's lists: they do not
    move, minimize or close with their owner;
  - explicit capture belongs to one window at a time and is not reference-counted;
  - the compositor is software only and redraws whole dirty rectangles per layer (no per-window
    caching of shadows; large Mica windows cost the most); one framebuffer mode chosen by the
    firmware (QEMU: 1024x768); no VSync, so a frame can tear;
  - the compositor reads a surface while its owner may be painting it (no double buffering);
    a frame can show a half-painted window;
  - keyboard focus is the active window (no focus within a window in win32k; that is the
    toolkit's job); US layout only; no key repeat beyond what the keyboard sends (no setting);
  - text: no bidirectional text, no font fallback (missing glyphs draw Inter's .notdef), no
    hinting, no subpixel positioning of glyph bitmaps; beyond 192 cached code points above
    U+00FF, glyphs are rasterized on every use;
  - the heap never returns small-block chunks to the system;
  - no security: any process may post to, activate or query any window.
- Toolkit limits (M7):
  - any layout change re-lays out the whole window (fine for hundreds of elements);
  - the text box has no undo (the multi-line `TextEditor` has); no IME or dead keys (US
    layout); the editor has no tab stops (tabs become four spaces), no bidirectional text and
    keeps whole lines in memory (fine for the 4 MiB Notepad limit); word wrap rebuilds all rows
    after an edit (fine at that size);
  - no keyboard navigation inside menus beyond arrows/Home/End/Enter/Escape (no access keys or
    submenus); combo boxes open below themselves only;
  - ScrollViewer scrolls vertically only; ListView rows have a fixed height;
  - no DPI scaling (sizes are pixels; 96 DPI assumed);
  - a light theme is not provided.
- Shell limits (M8):
  - one user; the password (M11) keeps casual users out but nothing else: every process can
    read `shell.ini` and the disk, and there are no access checks; the live system has no
    password ("Guest");
  - quick settings toggles, brightness and volume are visual only (no display gamma, no audio);
    settings, the password, the time zone and the Recent list are saved only on installed
    systems (the live image is read-only);
  - the logo key does nothing in combination with other keys yet; there are no notifications
    (toasts) and no desktop icons;
  - if the shell dies, the Session thread restarts it (up to 5 times a minute); the crash dialog
    cannot name a windowless program that is already gone ("A program (process N)");
  - the clipboard holds text only (64 KiB).
- Blockyard limits (M12):
  - It is a creative sandbox: no survival, crafting, creatures, day and night, or lighting
    beyond face shading and ambient occlusion.
  - The world is a fixed 256 x 64 x 256; water does not flow (breaking next to it fills the hole
    once); there is one world per documents folder.
  - On the live system without a writable volume it cannot save.
  - It renders at most 66 frames per second (hzui's animation timer) and slows with large
    windows (a ray per frame pixel).
- Application limits (M9):
  - Notepad: no printing, no encodings other than UTF-8 (CRLF is read, LF is written), files up
    to 4 MiB;
  - Files: no multiple selection, no drag and drop, no thumbnails; copies run on the window's
    thread without a progress dialog (the window does not respond during a long copy); the file
    clipboard belongs to one Files window;
  - Terminal: `| more` is the only pipe; no input redirection, no environment variables or
    scripts; programs get standard output and error but no standard input; one current
    directory for all drives;
  - Task Manager: CPU time of exited threads is not counted; the process list holds at most 128
    entries; memory is resident pages only;
  - every program opens every other one (no security): Task Manager and `kill` can end any user
    process, including the shell (only the System process is protected).
- Installation limits (M10):
  - Setup erases a whole disk (no dual boot, no keeping partitions or files), UEFI and GPT only,
    512-byte sectors only; it runs only from the live image, there is no upgrade path (installing
    again erases), and it cannot be cancelled once copying starts;
  - a partition table can only change while no volume of the disk is mounted (there is no
    unmount); the new volumes get no drive letter until the next start;
  - FAT32 on the system disk has no dirty flag or journal: the FAT is consistent after every
    operation, but power loss during one can leave lost clusters (fsck finds them); there are no
    file permissions;
  - an installed system still loads the 4 MB live image as a fallback (kept mounted without a
    letter); the ESP's Limine and kernel are updated only by installing again.
- Pool: no paged pool (there is no paging file); a freed page may be reused immediately, so
  double-free detection is reliable only while the slab stays alive (one empty slab per class is
  cached).
- The on-screen boot log is drawn while holding the log lock at HIGH_LEVEL during boot; once the
  desktop is shown the boot display is handed over and the log goes to serial only.
- Kernel text covers printable ASCII only (glyphs 32–126).
- Power off uses a byte-pattern scan of the DSDT for `\_S5` (no AML interpreter); firmware that
  computes the sleep types in AML would need the fallback (the machine halts with a message).
- `make vbox` requires the VM to be powered off. `--recreate` deletes the VM **including its disk**.
- The product ISO's Limine menu contains the Diagnostics entries (intended during development).

## Reference development environment (for reproducing results)

- Windows 11 host, WSL2 Ubuntu 22.04 (QEMU 6.2, OVMF, nasm, xorriso, mtools, python3 from apt),
  pinned toolchain in `~/.nexora-os-toolchain`, VirtualBox 7.2.4 on Windows driven through
  `VBoxManage.exe` (path conversion via `wslpath` or `cygpath`).
- The checkout path contains spaces (`.../Web Dev Projects/...`); the Makefile uses only relative
  paths for this reason. Keep it that way.
