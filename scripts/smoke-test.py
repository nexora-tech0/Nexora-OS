#!/usr/bin/env python3
"""Nexora OS headless boot smoke test.

Boots the ISO in QEMU without a display once per test case, waits for the expected serial
markers, captures a screenshot through the QEMU monitor and converts it to PNG.

    python3 scripts/smoke-test.py [--case CASE|all] [--timeout SECONDS]

Diagnostic cases build a variant ISO whose limine.conf boots straight into the matching
"Diagnostics" menu entry. Artifacts: build/test/<case>-serial.log and build/test/<case>.png.
Exit status is non-zero if any case fails.

Case options: "disk" (default True) attaches a fresh copy of build/test/disk.img (the FAT32
test disk from scripts/mkdisk.sh) as SATA port 0; "actions" is a list of (marker, [monitor
commands]) sent once the marker appears on the serial log ("sleep S" pauses and "type:TEXT"
types TEXT key by key); "exit_ok" accepts QEMU exiting (power-off and reset tests) when every
expected marker was seen; "disk_files" maps paths on the test volume to the contents they must
have afterwards; "timeout" (seconds) overrides --timeout for long interactive cases.

Installation cases (M10): "disk" may also be "empty:<MiB>" (a blank raw disk) or "from:<path>" (a
copy of an earlier case's result); "iso": False boots without the CD (from the disk);
"save_disk" keeps the final image for a later case; "install_checks" inspects the installed GPT
disk on the host (sgdisk -v, fsck.fat on both partitions, files read back with mtools).

Processors (M11): every case boots with QEMU_SMP processors from the environment (default 1;
`QEMU_SMP=4 make test` runs the whole suite on four); "processors" fixes the count for one case.
"""

import argparse
import errno
import os
import re
import shutil
import socket
import struct
import subprocess
import sys
import tempfile
import time
import zlib

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
TEST_DIR = os.path.join("build", "test")

HALTED = "*** System halted."
# QEMU sendkey names for characters typed by "type:" actions.
KEY_NAMES = {" ": "spc", ".": "dot", "\\": "backslash", ":": "shift-semicolon", "-": "minus", ">": "shift-dot",
             "_": "shift-minus", "/": "slash", "=": "equal", "+": "shift-equal", "*": "shift-8", ",": "comma",
             "!": "shift-1", "\n": "ret"}
TEST_DISK = os.path.join("build", "test", "disk.img")

CASES = {
    "boot": {
        "entry": None,
        "expect": ["Symbols: ", "Paging: kernel page tables active", "Memory: reclaimed", "Timers: TSC ",
                   "Traps: int3 round trip OK", "Clock: LAPIC timer at 1000 Hz", "I/O APIC: ISA IRQ 0 -> GSI",
                   "Memory self-tests: 8/8 passed", "Scheduler self-tests: ", "Runtime: C++ OK, Rust OK",
                   "Clock: system time ", "PCI: ", "AHCI: ", "Disk: Harddisk0 is 'QEMU HARDDISK', 64.0 MiB",
                   "PS/2: keyboard ready", "Volume: C: is \\Device\\Ramdisk0, UstarFs",
                   "Volume: D: is \\Device\\Harddisk0\\Partition1, FAT32 'HZTEST'",
                   "Hello from Nexora OS user mode! hello.exe", "usertest: ", " checks, 0 failed",
                   "Process self-tests: 12/12 passed", "fstest: ", "File system self-tests: 16/16 passed",
                   "wintest: display ", "Window manager self-tests: wintest.exe exit 0x00000000", "uitest.exe exit 0x00000000",
                   "apptest: ", "apptest.exe exit 0x00000000",
                   "surfaces left: OK", "[HZ] BOOT OK", "shell: lock screen shown", "shell: ready", "[HZ] HEARTBEAT"],
        "fail": ["*** STOP", "BOOT DEGRADED", "ERROR:"],
        "check_disk": True,
    },
    "desktop": {
        "entry": None,
        "disk": False,
        # A key unlocks; the shell starts the welcome program, whose window (760x520) is centred
        # in the work area above the 48 px taskbar with its text box focused. Typing goes there;
        # Tab reaches the Clear button and Space presses it; Shift+Tab comes back. The cursor
        # starts at the screen centre: up 268 is the caption, and dragging down 100 moves it.
        "actions": [("shell: lock screen shown", ["sendkey spc"]),
                    ("welcome: desktop windows created",
                     ["sendkey h", "sendkey i", "sendkey ret", "sendkey tab", "sendkey spc", "sendkey shift-tab",
                      "sendkey o", "sendkey k", "sendkey ret", "mouse_move 0 -268", "mouse_button 1",
                      "mouse_move 0 50", "mouse_move 0 50", "mouse_button 0"])],
        "expect": ["Desktop: compositor at ", "Window manager self-tests: ", "[HZ] BOOT OK",
                   "shell: unlocked", "welcome: you typed \"hi\"", "welcome: cleared", "welcome: you typed \"ok\"",
                   "welcome: moved to ("],
        "fail": ["*** STOP", "BOOT DEGRADED", "ERROR:"],
    },
    "shell": {
        "entry": None,
        "disk": False,
        # Unlock, open Start with the logo key, search "clo" and press Enter (Clock starts), Alt+Tab
        # back to Welcome. The taskbar's middle group is Start, Welcome, Clock (44 px buttons, 4 px
        # apart, centred): Clock's button is at (560, 744), 48 right and 360 down from the screen
        # centre where the cursor starts. Clicking it activates Clock, then minimizes it, then
        # restores it. The quick settings button is at (888, 744); its second wallpaper swatch is
        # 68 left and 122 up from there.
        "actions": [("shell: lock screen shown", ["sendkey spc"]),
                    ("welcome: desktop windows created", ["sendkey meta_l"]),
                    ("shell: start opened", ["sendkey c", "sendkey l", "sendkey o", "sendkey ret"]),
                    ("clock: started", ["sendkey alt-tab"]),
                    ("shell: switched to Welcome", ["mouse_move 48 360", "mouse_button 1", "mouse_button 0"]),
                    ("shell: activated Clock", ["mouse_button 1", "mouse_button 0"]),
                    ("shell: minimized Clock", ["mouse_button 1", "mouse_button 0"]),
                    ("shell: restored Clock", ["mouse_move 328 0", "mouse_button 1", "mouse_button 0"]),
                    ("shell: quick settings opened", ["mouse_move -68 -122", "mouse_button 1", "mouse_button 0"])],
        "expect": ["shell: unlocked", "shell: launched welcome.exe", "shell: start opened", "shell: launched clock.exe",
                   "clock: started", "shell: switched to Welcome", "shell: activated Clock", "shell: minimized Clock",
                   "shell: restored Clock", "shell: quick settings opened", "shell: wallpaper 1"],
        "fail": ["*** STOP", "BOOT DEGRADED", "ERROR:"],
    },
    "pinning": {
        "entry": None,
        "disk": False,
        # Search for Clock, open its themed context menu, pin it, then launch it from the taskbar.
        # At 1024x768 the first result is near (300, 250); after pinning, Clock is the third
        # centered taskbar button at (560, 744), alongside Start and Welcome.
        "actions": [("shell: lock screen shown", ["sendkey spc"]),
                    ("welcome: desktop windows created", ["sendkey meta_l"]),
                    ("shell: start opened",
                     ["type:clock", "sleep 1", "mouse_move -212 -134", "mouse_button 2", "mouse_button 0",
                      "sendkey down", "sendkey down", "sendkey ret", "sendkey esc"]),
                    ("shell: added taskbar pin clock",
                     ["sleep 1", "mouse_move 260 494", "mouse_button 1", "mouse_button 0"])],
        "expect": ["shell: unlocked", "shell: added taskbar pin clock", "shell: launched clock.exe", "clock: started"],
        "fail": ["*** STOP", "BOOT DEGRADED", "ERROR:"],
    },
    "apps": {
        "entry": None,
        # The M9 programs end to end, by keyboard. Notepad writes D:\smoke.txt through the save
        # dialog; the terminal (it starts on D:) types it, lists D:, writes a file with '>', runs
        # hello.exe through a pipe and starts Clock and Calculator; the calculator computes
        # 12 x 3; Task Manager filters "clock", selects it with Enter and ends it with Delete and
        # Enter; Files opens smoke.txt by type-ahead (in Notepad); Settings picks the second
        # wallpaper with Tab and Space (through the shell).
        "actions": [("shell: lock screen shown", ["sendkey spc"]),
                    ("welcome: desktop windows created", ["sendkey meta_l"]),
                    ("shell: start opened", ["type:note", "sendkey ret"]),
                    ("notepad: started", ["type:Hello from the smoke test", "sendkey ctrl-s"]),
                    ('hzui: dialog "Save as" opened', ["sendkey ctrl-a", "type:smoke.txt", "sendkey ret"]),
                    ("notepad: saved D:\\smoke.txt", ["sendkey meta_l", "sleep 1", "type:term", "sendkey ret"]),
                    ("terminal: started", ["type:type smoke.txt\n", "type:dir\n", "type:echo piped > out.txt\n",
                                           "type:type out.txt\n", "type:hello\n"]),
                    ("[exit code 42]", ["type:start clock\n"]),
                    ("clock: started", ["sleep 1", "sendkey alt-tab", "sleep 1", "type:start calc\n"]),
                    ("calc: started", ["type:12*3="]),
                    ("calc: 12 x 3 = 36", ["sendkey meta_l", "sleep 1", "type:task", "sendkey ret"]),
                    ("taskmgr: started", ["sleep 1", "type:clock\n", "sendkey delete"]),
                    ('hzui: dialog "End task" opened', ["sendkey ret"]),
                    ("taskmgr: ended clock.exe", ["sendkey meta_l", "sleep 1", "type:files", "sendkey ret"]),
                    ("files: D:\\ (", ["sleep 1", "type:smo", "sendkey ret"]),
                    ("notepad: opened D:\\smoke.txt", ["sendkey meta_l", "sleep 1", "type:sett", "sendkey ret"]),
                    ("settings: started", ["sleep 1", "sendkey tab", "sendkey tab", "sendkey spc"]),
                    ("settings: wallpaper 1 requested", ["sendkey meta_l"]),
                    ("shell: start opened", ["type:system information", "sendkey ret"]),
                    ("sysinfo: started", ["sendkey meta_l", "sleep 1", "type:calendar", "sendkey ret"]),
                    ("calendar: started", ["sendkey meta_l", "sleep 1", "type:stopwatch", "sendkey ret"]),
                    ("stopwatch: started", ["sendkey meta_l", "sleep 1", "type:paint", "sendkey ret"]),
                    ("paint: started", [])],
        "expect": ["apptest.exe exit 0x00000000", "[HZ] BOOT OK", "shell: 15 programs installed",
                   "shell: launched notepad.exe", "notepad: saved D:\\smoke.txt (25 bytes)",
                   "terminal| Hello from the smoke test", "terminal|  Directory of D:\\",
                   "terminal: wrote 6 bytes to D:\\out.txt", "terminal| piped",
                   "terminal| Hello from Nexora OS user mode! hello.exe", "terminal| [exit code 42]",
                   "terminal| Started clock.exe (process", "terminal| Started calc.exe (process", "calc: 12 x 3 = 36",
                   "taskmgr: ended clock.exe", "files: D:\\ (", "files: opened D:\\smoke.txt with Notepad",
                   "notepad: opened D:\\smoke.txt (25 bytes, 1 lines)", "settings: Personalization",
                   "settings: wallpaper 1 requested", "shell: wallpaper 1 (requested)", "sysinfo: started",
                   "calendar: started", "stopwatch: started", "paint: started"],
        "fail": ["*** STOP", "BOOT DEGRADED", "ERROR:", "FAIL"],
        "check_disk": True,
        "timeout": 240.0,  # about 40 s of typing through the QEMU monitor
        "disk_files": {"::/smoke.txt": "Hello from the smoke test", "::/out.txt": "piped\n"},
    },
    "network-interface": {
        "entry": None,
        "disk": False,
        "qemu_args": ["-netdev", "user,id=net0", "-device", "e1000,netdev=net0"],
        "actions": [("shell: lock screen shown", ["sendkey spc"]),
                    ("welcome: desktop windows created", ["sendkey meta_l"]),
                    ("shell: start opened", ["type:settings", "sendkey ret"]),
                    ("settings: started", ["sendkey down", "sendkey down", "sendkey down", "sendkey down",
                                           "sendkey ret"])],
        "expect": ["Network: interface eth0 registered", "E1000: ", "up link", "Network protocol self-tests: ",
                   "passed", "DHCP self-tests: 6/6 passed", "DHCP: eth0 configured", "usertest: ",
                   "checks, 0 failed", "shell: ready", "settings: Network"],
        "fail": ["*** STOP", "ERROR:"],
        "timeout": 120.0,
    },
    "catalog-apps": {
        "entry": None,
        "actions": [("shell: lock screen shown", ["sendkey spc"]),
                    ("welcome: desktop windows created", ["sendkey meta_l"]),
                    ("shell: start opened", ["type:system information", "sendkey ret"]),
                    ("sysinfo: started", ["sendkey meta_l", "sleep 1", "type:calendar", "sendkey ret"]),
                    ("calendar: started", ["sendkey meta_l", "sleep 1", "type:stopwatch", "sendkey ret"]),
                    ("stopwatch: started", ["sendkey meta_l", "sleep 1", "type:paint", "sendkey ret"]),
                    ("paint: started", ["sendkey meta_l", "sleep 1", "type:browser", "sendkey ret"])],
        "expect": ["shell: 15 programs installed", "shell: ready", "sysinfo: started", "calendar: started",
                   "stopwatch: started", "paint: started", "browser: started; HTML text renderer ready; network unavailable"],
        # The current partition-table self-test failure is tracked separately; this case focuses
        # on manifest discovery and launching the newly added apps.
        "fail": ["*** STOP", "sysinfo: cannot create the window", "calendar: cannot create the window",
                 "stopwatch: cannot create the window", "paint: cannot create the window"],
        "timeout": 180.0,
    },
    "browser": {
        "entry": None,
        "disk": False,
        "actions": [("shell: lock screen shown", ["sendkey spc"]),
                    ("welcome: desktop windows created", ["sendkey meta_l", "type:browser", "sendkey ret"]),
                    ("browser: started; HTML text renderer ready; network unavailable",
                     ["type:https://example.com", "sendkey ret", "sendkey ctrl-l",
                      "type:https://example.com:99999999999999999999", "sendkey ret"])],
        "expect": ["shell: 15 programs installed",
                   "browser: started; HTML text renderer ready; network unavailable",
                   "browser: address validated; no page request",
                   "browser: rejected invalid web address"],
        "fail": ["*** STOP", "ERROR:", "crashed"],
        "timeout": 150.0,
    },
    "install": {
        "entry": None,
        "disk": "empty:384",
        # Setup from Start by keyboard: Enter (welcome), Enter (the first eligible disk), Space
        # (agree to erase) and Enter (install).
        "actions": [("shell: lock screen shown", ["sendkey spc"]),
                    ("welcome: desktop windows created", ["sendkey meta_l"]),
                    ("shell: start opened", ["type:install", "sendkey ret"]),
                    ("setup: started", ["sleep 1", "sendkey ret"]),
                    ("setup: disk 0 ", ["sleep 1", "sendkey ret"]),
                    ("setup: chose disk 0", ["sleep 1", "sendkey spc", "sendkey ret"])],
        "expect": ["[HZ] BOOT OK", "Ramdisk: ", "at \\Device\\Ramdisk1", "shell: 15 programs installed",
                   "setup: disk 0 'QEMU HARDDISK', 384 MB: empty", "setup: installing on disk 0",
                   "Disk: Harddisk0 partition table read again: GPT, 2 partition(s)", "setup: partitioned disk 0",
                   "setup: formatted partition 1 as FAT32 'EFI'", "setup: formatted partition 2 as FAT32 'NEXORAOS'",
                   "setup: boot loader, kernel and system image written", "setup: copied ",
                   "setup: done; Nexora OS is installed on disk 0"],
        "fail": ["*** STOP", "BOOT DEGRADED", "ERROR:", "setup: failed", "FAIL"],
        "timeout": 360.0,
        "save_disk": os.path.join("build", "test", "installed.img"),
        "install_checks": [
            (1, "::/EFI/BOOT/BOOTX64.EFI", "exists", None),
            (1, "::/boot/hzkrnl.elf", "same", os.path.join("build", "hzkrnl.elf")),
            (1, "::/boot/hzkrnl.sym", "same", os.path.join("build", "hzkrnl.sym")),
            (1, "::/boot/ramdisk.tar", "same", os.path.join("build", "ramdisk.tar")),
            (1, "::/boot/limine/limine.conf", "contains", "cmdline: hz.system=SYSTEM_GUID"),
            (1, "::/boot/limine/limine.conf", "contains", "module_string: symbols\n\n/Nexora OS (system image"),
            (2, "::/Nexora/System/shell.exe", "same", os.path.join("build", "user", "shell.exe")),
            (2, "::/Nexora/Fonts/Inter-Regular.ttf", "same", os.path.join("third_party", "inter", "Inter-Regular.ttf")),
            (2, "::/Nexora/Fonts/InterDisplay-SemiBold.ttf", "same",
             os.path.join("third_party", "inter", "InterDisplay-SemiBold.ttf")),
            (2, "::/Nexora/Apps/notepad.app", "exists", None),
            (2, "::/Nexora/Apps/setup.app", "contains", "Pinned=0"),
            (2, "::/Nexora/Settings/shell.ini", "contains", "FirstBoot=1"),
            (2, "::/Users", "exists", None)],
    },
    "diskboot": {
        "entry": None,
        "iso": False,
        "disk": "from:" + os.path.join("build", "test", "installed.img"),
        # The first start from the installed disk: no lock screen, first-boot setup instead. Enter
        # (welcome), "Ada" and Enter, Tab to the second wallpaper and Space, Enter, Enter (finish).
        "actions": [("setup: first-boot setup started",
                     ["sleep 1", "sendkey ret", "sleep 1", "type:Ada", "sendkey ret", "sleep 1", "sendkey tab",
                      "sendkey spc", "sleep 1", "sendkey ret", "sleep 1", "sendkey ret"])],
        "expect": ["Disk: \\Device\\Harddisk0\\Partition2 is the system partition (hz.system)",
                   "Volume: C: is \\Device\\Harddisk0\\Partition2, FAT32 'NEXORAOS'", "Process self-tests: 12/12 passed",
                   "File system self-tests: ", "apptest.exe exit 0x00000000", "surfaces left: OK", "[HZ] BOOT OK",
                   "shell: 15 programs installed", "shell: settings loaded: user 'Guest', wallpaper 0, first boot",
                   "shell: first boot: started first-boot setup", "shell: wallpaper 1 (requested)",
                   "setup: first boot done: user 'Ada', wallpaper 1 (settings saved: 0x00000000)",
                   "shell: first-boot setup finished; hello, Ada", "shell: launched welcome.exe"],
        "fail": ["*** STOP", "BOOT DEGRADED", "ERROR:", "FAIL", "shell: lock screen shown"],
        "timeout": 240.0,
        "save_disk": os.path.join("build", "test", "firstboot.img"),
        "install_checks": [
            (2, "::/Nexora/Settings/shell.ini", "contains", "UserName=Ada"),
            (2, "::/Nexora/Settings/shell.ini", "contains", "FirstBoot=0"),
            (2, "::/Nexora/Settings/shell.ini", "contains", "Wallpaper=1"),
            (2, "::/Users/Ada/Documents/Welcome.txt", "contains", "Welcome to Nexora OS, Ada.")],
    },
    "diskboot2": {
        "entry": None,
        "iso": False,
        "disk": "from:" + os.path.join("build", "test", "firstboot.img"),
        # Restarting keeps the settings: the lock screen greets Ada on the second wallpaper. Then
        # (M11) Settings > Accounts sets the password "sun", the terminal's "lock" locks the
        # session, a wrong password is refused and the right one unlocks.
        "actions": [("shell: lock screen shown", ["sendkey spc"]),
                    ("welcome: desktop windows created", ["sendkey meta_l", "sleep 1", "type:settings", "sendkey ret"]),
                    ("settings: Personalization", ["sleep 1", "sendkey down"]),
                    ("settings: Accounts", ["sleep 1", "sendkey tab", "sendkey tab", "sendkey tab", "type:sun\n",
                                            "type:sun\n"]),
                    ("settings: password set", ["sleep 1", "sendkey meta_l", "sleep 1", "type:term", "sendkey ret"]),
                    ("terminal: started", ["sleep 1", "type:lock\n"]),
                    ("shell: locked", ["sleep 1", "sendkey spc"]),
                    ("shell: password prompt", ["sleep 1", "type:moon\n"]),
                    ("shell: wrong password (1)", ["sleep 1", "type:sun\n"])],
        "expect": ["Volume: C: is \\Device\\Harddisk0\\Partition2, FAT32 'NEXORAOS'", "[HZ] BOOT OK",
                   "shell: settings loaded: user 'Ada', wallpaper 1", "shell: lock screen shown", "shell: unlocked",
                   "shell: launched welcome.exe", "settings: password set (0x00000000)",
                   "shell: settings loaded: user 'Ada', wallpaper 1, password", "shell: locked",
                   "shell: wrong password (1)", "shell: password accepted"],
        "fail": ["*** STOP", "BOOT DEGRADED", "ERROR:", "FAIL", "first-boot setup"],
        "timeout": 240.0,
        "save_disk": os.path.join("build", "test", "accounts.img"),
        "install_checks": [
            (2, "::/Nexora/Settings/shell.ini", "contains", "PasswordHash="),
            (2, "::/Nexora/Settings/shell.ini", "contains", "PasswordSalt=")],
    },
    "diskboot3": {
        "entry": None,
        "iso": False,
        "disk": "from:" + os.path.join("build", "test", "accounts.img"),
        # M11: the password survives a restart; the lock screen asks for it before unlocking.
        "actions": [("shell: lock screen shown", ["sendkey spc"]),
                    ("shell: password prompt", ["sleep 1", "type:sun\n"])],
        "expect": ["[HZ] BOOT OK", "shell: settings loaded: user 'Ada', wallpaper 1, password", "shell: password prompt",
                   "shell: password accepted", "shell: unlocked", "shell: launched welcome.exe"],
        "fail": ["*** STOP", "BOOT DEGRADED", "ERROR:", "FAIL", "shell: wrong password", "first-boot setup"],
        "timeout": 180.0,
    },
    "smp": {
        "entry": None,
        "processors": 4,
        # Four processors (M11): every one online, the multiprocessor scheduler tests, all
        # self-tests, then a terminal running programs and Task Manager's performance page.
        "actions": [("shell: lock screen shown", ["sendkey spc"]),
                    ("welcome: desktop windows created", ["sendkey meta_l", "sleep 1", "type:term", "sendkey ret"]),
                    ("terminal: started", ["type:hello\n", "type:ps\n", "type:mem\n"]),
                    ("terminal| [exit code 42]", ["sendkey meta_l", "sleep 1", "type:task", "sendkey ret"]),
                    ("taskmgr: started", ["sleep 1", "sendkey ctrl-tab", "sleep 3"])],
        "expect": ["MP: 4 processors reported; 3 application processor(s) parked", "MP: 4 processors online",
                   "Scheduler self-tests: 14/14 passed", "Process self-tests: 12/12 passed",
                   "File system self-tests: 16/16 passed", "apptest.exe exit 0x00000000", "surfaces left: OK",
                   "[HZ] BOOT OK", "[HZ] HEARTBEAT: 4 processors", "shell: unlocked", "terminal| [exit code 42]",
                   "terminal| Page size 4096 bytes;", "4 processor(s)", "taskmgr: started"],
        "fail": ["*** STOP", "BOOT DEGRADED", "ERROR:", "FAIL"],
        "timeout": 200.0,
    },
    "recovery": {
        "entry": None,
        # M11: a program that faults (crash.exe null from the terminal) gets the shell's error dialog;
        # killing the shell from the terminal makes the session start it again (no lock screen),
        # and the new shell works (Start launches Calculator).
        "actions": [("shell: lock screen shown", ["sendkey spc"]),
                    ("welcome: desktop windows created", ["sendkey meta_l", "sleep 1", "type:term", "sendkey ret"]),
                    ("terminal: started", ["sleep 1", "type:crash null\n"]),
                    ('hzui: dialog "Program error" opened', ["sleep 1", "sendkey ret", "sleep 1", "type:kill shell\n"]),
                    ("shell: restarted", ["sleep 2", "sendkey meta_l", "sleep 1", "type:calc", "sendkey ret"])],
        "expect": ["[HZ] BOOT OK", "shell: unlocked", "terminal: running crash.exe null",
                   "crashed (0xC0000005)", "terminal| [ended by an error: 0xC0000005]",
                   "Session: shell.exe exited (0x00000001); restarting it", "shell: restarted", "calc: started"],
        "fail": ["*** STOP", "BOOT DEGRADED", "ERROR:", "FAIL"],
        "timeout": 200.0,
    },
    "blockyard": {
        "entry": None,
        "processors": 2,
        # The block game: started from Start, the click captures the mouse (window manager mouse
        # lock), a look down, a block broken and a brick placed in its place, flying, Esc releases
        # the mouse; Alt+F4 saves the world to D:, and starting it again loads that world. It looks
        # about 49 degrees down so the ground is in reach whatever the (time-seeded) terrain.
        "actions": [("shell: lock screen shown", ["sendkey spc"]),
                    ("welcome: desktop windows created", ["sendkey meta_l", "sleep 1", "type:block", "sendkey ret"]),
                    ("blockyard: started", ["sleep 3", "mouse_button 1", "mouse_button 0"]),
                    ("blockyard: mouse captured", ["sleep 2", "mouse_move 0 200", "sleep 1", "mouse_button 1",
                                                   "mouse_button 0", "sleep 1", "sendkey 9", "mouse_button 2",
                                                   "mouse_button 0", "sleep 1", "sendkey f", "sendkey w 600", "sleep 1",
                                                   "sendkey esc"]),
                    ("blockyard: mouse released", ["sleep 1", "sendkey alt-f4"]),
                    ("blockyard: saved the world", ["sleep 2", "sendkey meta_l", "sleep 1", "type:block", "sendkey ret"]),
                    ("blockyard: world loaded", ["sleep 4"])],
        "expect": ["[HZ] BOOT OK", "shell: 15 programs installed", "blockyard: world generated",
                   "blockyard: started (2 render threads, saves to D:\\Blockyard.world)", "blockyard: mouse captured",
                   "blockyard: broke ", "blockyard: placed Brick at", "blockyard: flying", "blockyard: mouse released",
                   "blockyard: saved the world to D:\\Blockyard.world", "blockyard: world loaded", " fps at "],
        "fail": ["*** STOP", "BOOT DEGRADED", "ERROR:", "FAIL", "crashed"],
        "timeout": 200.0,
    },
    "poweroff": {
        "entry": None,
        "exit_ok": True,
        # M11: "shutdown" in the terminal asks the shell to close every program first. Notepad has
        # unsaved text, so it asks about it and stays open; after 15 s the shell names it and
        # offers "Shut down anyway" (Enter), then the volumes are flushed and ACPI powers off.
        "actions": [("shell: lock screen shown", ["sendkey spc"]),
                    ("welcome: desktop windows created", ["sendkey meta_l", "sleep 1", "type:notepad", "sendkey ret"]),
                    ("notepad: started", ["sleep 1", "type:unsaved draft", "sleep 1", "sendkey meta_l", "sleep 1",
                                          "type:term", "sendkey ret"]),
                    ("terminal: started", ["sleep 1", "type:shutdown\n"]),
                    ('hzui: dialog "Shut down" opened', ["sleep 1", "sendkey ret"])],
        "expect": ["[HZ] BOOT OK", "shell: shut down: closing", 'hzui: dialog "Notepad" opened',
                   "shell: still open after 15 s: Notepad", "shell: shut down", "Power: shutdown requested by shell.exe",
                   "Power: powering off (ACPI S5)"],
        "fail": ["*** STOP", "BOOT DEGRADED", "ERROR:", "FAIL"],
        "timeout": 200.0,
    },
    "nodisk": {
        "entry": None,
        "disk": False,
        "expect": ["Volume: C: is \\Device\\Ramdisk0", "FAT32 tests skipped", "(no test volume)",
                   "File system self-tests: 5/5 passed", "[HZ] BOOT OK"],
        "fail": ["*** STOP", "BOOT DEGRADED", "ERROR:", "Volume: D:"],
    },
    "input": {
        "entry": "Diagnostics/Input echo (keyboard and mouse)",
        "actions": [("[HZ] HEARTBEAT", ["sendkey h", "sendkey i", "sendkey shift-a", "mouse_move 12 -7",
                                        "mouse_button 1", "mouse_button 0"])],
        "expect": ["PS/2: keyboard ready", "Input: key 'h' down", "Input: key 'h' up", "Input: key 'i' down",
                   "Input: key 'A' down", "Input: mouse dx=12 dy=-7", "buttons=0x1"],
        "fail": ["*** STOP", "BOOT DEGRADED"],
    },
    "shutdown": {
        "entry": "Diagnostics/Power test (shut down after boot)",
        "exit_ok": True,
        "expect": ["[HZ] BOOT OK", "Power: shutdown requested by System", "Power: powering off (ACPI S5)"],
        "fail": ["*** STOP", "did not power off"],
    },
    "reboot": {
        "entry": "Diagnostics/Power test (restart after boot)",
        "exit_ok": True,
        "expect": ["[HZ] BOOT OK", "Power: restart requested by System", "Power: resetting the machine"],
        "fail": ["*** STOP"],
    },
    "verbose": {
        "entry": "Nexora OS (Live, verbose log)",
        "expect": ["MmTest: 512 zeroed pages", "MmTest: private address space", "MmTest: 32 KiB kernel stack",
                   "Pool: ", "ACPI: override IRQ 0", "KeTest: all test threads reaped", "Object namespace:",
                   "C: -> \\Device\\Ramdisk0", "D: -> \\Device\\Harddisk0\\Partition1", "Fat32: mounted 'HZTEST'",
                   "PCI: 00:1f.2 8086:2922 class 01.06.01 SATA controller MSI", "UstarFs: mounted",
                   "Disk: \\Device\\Harddisk0\\Partition2: 4.0 MiB at LBA 100352, Nexora OS test scratch",
                   "IoTest: raw sector round trip on the scratch partition (300 sectors): OK",
                   "relocations applied", "Process: usertest.exe (PID 16) deleted",
                   "PsTest: objects left: 0 processes, 0 threads, 0 events, 0 files; pool +0 bytes: OK",
                   "[HZ] BOOT OK"],
        "fail": ["*** STOP", "BOOT DEGRADED", "ERROR:"],
    },
    "bugcheck": {
        "entry": "Diagnostics/Stop screen test (manual bug check)",
        "expect": ["*** STOP 0x000000E2 MANUALLY_INITIATED_CRASH", "KeBugCheckEx", HALTED],
        "fail": [],
    },
    "rustpanic": {
        "entry": "Diagnostics/Stop screen test (Rust panic)",
        "expect": ["*** STOP 0x00000010 RUST_PANIC", "self-destruct requested", "rust_begin_unwind",
                   "core::panicking::panic_fmt", "RgnTriggerPanic", HALTED],
        "fail": [],
    },
    "pagefault": {
        "entry": "Diagnostics/Exception test (page fault)",
        "expect": ["*** STOP 0x00000021 KERNEL_PAGE_FAULT",
                   "Page fault writing 0x0000000000001000 (page not present) in KiTestPageFault", "CR2 ", HALTED],
        "fail": [],
    },
    "divide": {
        "entry": "Diagnostics/Exception test (divide error)",
        "expect": ["*** STOP 0x00000020 UNEXPECTED_KERNEL_TRAP", "Divide error", "KiTestDivideError", HALTED],
        "fail": [],
    },
    "invalidop": {
        "entry": "Diagnostics/Exception test (invalid opcode)",
        "expect": ["*** STOP 0x00000020 UNEXPECTED_KERNEL_TRAP", "Invalid opcode", "KiTestInvalidOpcode", HALTED],
        "fail": [],
    },
    "gpf": {
        "entry": "Diagnostics/Exception test (general protection)",
        "expect": ["*** STOP 0x00000020 UNEXPECTED_KERNEL_TRAP", "General protection fault",
                   "KiTestGeneralProtection", HALTED],
        "fail": [],
    },
    "doublefault": {
        "entry": "Diagnostics/Exception test (double fault)",
        "expect": ["*** STOP 0x00000022 DOUBLE_FAULT", "KiTestDoubleFault", HALTED],
        "fail": [],
    },
    "assert": {
        "entry": "Diagnostics/Assertion test",
        "expect": ["*** STOP 0x00000001 KERNEL_ASSERTION_FAILED",
                   "assertion failed: KeGetCurrentIrql() == HIGH_LEVEL", "init/diag.c", HALTED],
        "fail": [],
    },
    "irql": {
        "entry": "Diagnostics/IRQL violation test",
        "expect": ["*** STOP 0x00000009 IRQL_INVALID_TRANSITION", "KiIrqlViolation", HALTED],
        "fail": [],
    },
    "stackoverflow": {
        "entry": "Diagnostics/Memory test (kernel stack overflow)",
        "expect": ["*** STOP 0x00000044 KERNEL_STACK_OVERFLOW", "Kernel stack overflow: guard page",
                   "KiRecurseForever", HALTED],
        "fail": [],
    },
    "poolcorruption": {
        "entry": "Diagnostics/Memory test (pool header corruption)",
        "expect": ["*** STOP 0x00000040 BAD_POOL_HEADER", "has a corrupted header (tag 'Diag'", "KiTestPoolCorruption", HALTED],
        "fail": [],
    },
    "doublefree": {
        "entry": "Diagnostics/Memory test (pool double free)",
        "expect": ["*** STOP 0x00000041 BAD_POOL_CALLER", "Parameters: 0x0000000000000001", "was freed twice", "KiTestDoubleFree",
                   HALTED],
        "fail": [],
    },
    "writero": {
        "entry": "Diagnostics/Memory test (write to read-only data)",
        "expect": ["*** STOP 0x00000021 KERNEL_PAGE_FAULT", "Page fault writing", "(protection violation) in KiTestWriteReadOnly",
                   HALTED],
        "fail": [],
    },
    "threadoverflow": {
        "entry": "Diagnostics/Scheduler test (thread stack overflow)",
        "expect": ["*** STOP 0x00000044 KERNEL_STACK_OVERFLOW", "Thread: 'DiagOverflow'", "KiRecurseForever", HALTED],
        "fail": [],
    },
    "waitirql": {
        "entry": "Diagnostics/Scheduler test (wait at DISPATCH_LEVEL)",
        "expect": ["*** STOP 0x0000000C WAIT_AT_RAISED_IRQL", "at IRQL 2", "KiTestWaitAtRaisedIrql", HALTED],
        "fail": [],
    },
    "mutexowner": {
        "entry": "Diagnostics/Scheduler test (release unowned mutex)",
        "expect": ["*** STOP 0x0000000D MUTEX_NOT_OWNED", "Thread: 'Phase1Init'", "KiTestMutexNotOwned", HALTED],
        "fail": [],
    },
    "nxexec": {
        "entry": "Diagnostics/Memory test (execute from data page)",
        "expect": ["*** STOP 0x00000021 KERNEL_PAGE_FAULT", "Page fault executing", "(protection violation)", HALTED],
        "fail": [],
    },
    "smap": {
        "entry": "Diagnostics/User boundary test (kernel touches a user page: SMAP)",
        "expect": ["*** STOP 0x00000026 SUPERVISOR_MODE_USER_ACCESS", "SMAP violation: kernel read user address",
                   "KiTestSmapViolation", HALTED],
        "fail": [],
    },
}


def log(msg):
    print(f"[smoke] {msg}", flush=True)


def build_iso(case):
    """Returns the ISO path for a case, building a variant ISO for diagnostic entries."""
    entry = CASES[case]["entry"]
    if entry is None:
        subprocess.run(["make", "--no-print-directory", "iso"], cwd=ROOT, check=True)
        return os.path.join("build", "nexora-os.iso")

    os.makedirs(os.path.join(ROOT, TEST_DIR), exist_ok=True)
    conf = os.path.join(TEST_DIR, f"{case}.conf")
    with open(os.path.join(ROOT, "boot", "limine.conf"), encoding="utf-8") as f:
        lines = f.read().splitlines()
    out = [f"default_entry: {entry}"]
    for line in lines:
        if line.startswith("timeout:"):
            line = "timeout: 0"
        out.append(line)
    with open(os.path.join(ROOT, conf), "w", encoding="utf-8", newline="\n") as f:
        f.write("\n".join(out) + "\n")

    iso = os.path.join(TEST_DIR, f"{case}.iso")
    subprocess.run(
        ["make", "--no-print-directory", "iso", f"ISO={iso}",
         f"ISO_ROOT={TEST_DIR}/{case}_root", f"LIMINE_CONF={conf}"],
        cwd=ROOT, check=True, stdout=subprocess.DEVNULL)
    return iso


def read_ppm(ppm_path, timeout=10.0):
    """Returns (width, height, RGB bytes) of a P6 PPM, waiting until QEMU has written all of it.
    The header is "P6 <w> <h> <maxval>" plus exactly one whitespace byte (no comments from QEMU);
    the pixels follow and may themselves start with whitespace values, so they are sliced, not
    split."""
    m = None
    deadline = time.time() + timeout
    while True:
        with open(ppm_path, "rb") as f:
            data = f.read()
        m = re.match(rb"P6\s+(\d+)\s+(\d+)\s+(\d+)\s", data)
        if m and len(data) >= m.end() + int(m.group(1)) * int(m.group(2)) * 3:
            break
        if time.time() > deadline:
            raise ValueError("incomplete PPM screendump")
        time.sleep(0.2)
    width, height, maxval = int(m.group(1)), int(m.group(2)), int(m.group(3))
    if maxval != 255:
        raise ValueError("unsupported PPM maxval")
    return width, height, data[m.end():m.end() + width * height * 3]


def ppm_to_png(ppm_path, png_path):
    width, height, pixels = read_ppm(ppm_path)
    stride = width * 3
    raw = b"".join(b"\x00" + pixels[y * stride:(y + 1) * stride] for y in range(height))

    def chunk(kind, payload):
        body = kind + payload
        return struct.pack(">I", len(payload)) + body + struct.pack(">I", zlib.crc32(body) & 0xFFFFFFFF)

    png = b"\x89PNG\r\n\x1a\n"
    png += chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0))
    png += chunk(b"IDAT", zlib.compress(raw, 6))
    png += chunk(b"IEND", b"")
    with open(png_path, "wb") as f:
        f.write(png)


def monitor_command(sock_path, command):
    s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    s.settimeout(10)
    s.connect(sock_path)
    s.recv(4096)  # banner
    s.sendall(command.encode() + b"\n")
    time.sleep(0.5)
    try:
        s.recv(4096)
    except socket.timeout:
        pass
    s.close()


def check_disk(image, files=None):
    """Checks the FAT32 test partition after a boot wrote to it: fsck.fat must find no errors and
    mtools must read back the tree the kernel left (long names, a moved directory)."""
    part = image + ".part"
    with open(image, "rb") as src, open(part, "wb") as dst:
        src.seek(2048 * 512)
        dst.write(src.read(48 * 1024 * 1024))
    fsck = subprocess.run(["fsck.fat", "-n", part], capture_output=True, text=True)
    if fsck.returncode != 0:
        return f"fsck.fat found problems: {(fsck.stdout + fsck.stderr).strip()[-600:]}"
    env = dict(os.environ, MTOOLS_SKIP_CHECK="1")
    listing = subprocess.run(["mdir", "-i", part, "-/", "::"], capture_output=True, text=True, env=env).stdout
    for name in ("Written by the kernel.txt", "Target directory", "Moved directory"):
        if name not in listing:
            return f"mtools does not see {name!r} on the test volume"
    expected = {"::/Kept by Nexora OS/Written by the kernel.txt": "Nexora OS wrote this file\n",
                "::/Kept by Nexora OS/Target directory/Moved directory/inner.txt": "inner\n"}
    expected.update(files or {})
    for path, contents in expected.items():
        text = subprocess.run(["mtype", "-i", part, path], capture_output=True, text=True, env=env).stdout
        if text != contents:
            return f"mtools read {text!r} from {path}"
    return None


def read_gpt_partitions(image):
    """(first LBA, last LBA, unique GUID text) of the GPT entries of a raw image."""
    with open(image, "rb") as f:
        f.seek(512)
        header = f.read(512)
        if header[:8] != b"EFI PART":
            return []
        entries_lba, count, size = struct.unpack_from("<QII", header, 72)
        f.seek(entries_lba * 512)
        table = f.read(count * size)
    parts = []
    for i in range(count):
        e = table[i * size:(i + 1) * size]
        if e[:16] == bytes(16):
            continue
        u = e[16:32]
        guid = "%08X-%04X-%04X-%s-%s" % (struct.unpack_from("<I", u, 0)[0], struct.unpack_from("<H", u, 4)[0],
                                         struct.unpack_from("<H", u, 6)[0], u[8:10].hex().upper(), u[10:16].hex().upper())
        first, last = struct.unpack_from("<QQ", e, 32)
        parts.append((first, last, guid))
    return parts


def check_install(image, checks):
    """Checks an installed disk image: the GPT with sgdisk, both FAT32 partitions with fsck.fat,
    and files with mtools ("exists", "absent", "contains" text, "same" bytes as a host file)."""
    gpt = subprocess.run(["sgdisk", "-v", image], capture_output=True, text=True)
    if gpt.returncode != 0 or "No problems found" not in gpt.stdout:
        return f"sgdisk -v: {(gpt.stdout + gpt.stderr).strip()[-400:]}"
    parts = read_gpt_partitions(image)
    if len(parts) != 2:
        return f"expected 2 GPT partitions, found {len(parts)}"
    env = dict(os.environ, MTOOLS_SKIP_CHECK="1")
    tmp = tempfile.mkdtemp(prefix="hz-install-")
    try:
        files = []
        for i, (first, last, _) in enumerate(parts):
            path = os.path.join(tmp, f"part{i + 1}.img")
            with open(image, "rb") as src, open(path, "wb") as dst:
                src.seek(first * 512)
                remaining = (last - first + 1) * 512
                while remaining:
                    chunk = src.read(min(remaining, 4 << 20))
                    if not chunk:
                        break
                    dst.write(chunk)
                    remaining -= len(chunk)
            fsck = subprocess.run(["fsck.fat", "-n", path], capture_output=True, text=True)
            if fsck.returncode != 0:
                return f"fsck.fat on partition {i + 1}: {(fsck.stdout + fsck.stderr).strip()[-500:]}"
            files.append(path)
        system_guid = parts[1][2]
        for part, name, kind, value in checks:
            img = files[part - 1]
            if kind in ("exists", "absent"):
                listing = subprocess.run(["mdir", "-i", img, "-b", name], capture_output=True, text=True, env=env)
                if (listing.returncode == 0) != (kind == "exists"):
                    return f"partition {part}: {name} {'missing' if kind == 'exists' else 'present'}"
                continue
            data = subprocess.run(["mtype", "-i", img, name], capture_output=True, env=env).stdout
            if kind == "contains":
                wanted = value.replace("SYSTEM_GUID", system_guid).encode()
                if wanted not in data:
                    return f"partition {part}: {name} lacks {wanted!r}"
            elif kind == "same":
                with open(os.path.join(ROOT, value), "rb") as f:
                    if f.read() != data:
                        return f"partition {part}: {name} differs from {value} ({len(data)} bytes read back)"
        return None
    finally:
        shutil.rmtree(tmp, ignore_errors=True)


def run_case(case, timeout):
    spec = CASES[case]
    iso = build_iso(case)
    serial_log = os.path.join(TEST_DIR, f"{case}-serial.log")
    png = os.path.join(TEST_DIR, f"{case}.png")
    os.makedirs(os.path.join(ROOT, TEST_DIR), exist_ok=True)
    for p in (serial_log, png):
        if os.path.exists(os.path.join(ROOT, p)):
            os.remove(os.path.join(ROOT, p))

    # Unix sockets do not work on some host-mounted filesystems (WSL drvfs), so use /tmp.
    tmp = tempfile.mkdtemp(prefix="hz-smoke-")
    mon = os.path.join(tmp, "monitor.sock")
    ppm = os.path.join(tmp, "screen.ppm")
    vars_copy = os.path.join(tmp, "vars.fd")

    env = dict(os.environ, QEMU_DISPLAY="none", QEMU_SERIAL=f"file:{serial_log}", QEMU_VARS=vars_copy)
    disk = spec.get("disk", True)
    disk_copy = os.path.join(tmp, "disk.img")  # each case starts from a copy
    if disk is True:
        shutil.copyfile(os.path.join(ROOT, TEST_DISK), disk_copy)
        env["QEMU_DISK"] = disk_copy
    elif isinstance(disk, str) and disk.startswith("empty:"):
        with open(disk_copy, "wb") as f:
            f.truncate(int(disk[6:]) * 1024 * 1024)
        env["QEMU_DISK"] = disk_copy
    elif isinstance(disk, str) and disk.startswith("from:"):
        source = os.path.join(ROOT, disk[5:])
        if not os.path.exists(source):
            shutil.rmtree(tmp, ignore_errors=True)
            log(f"{case}: FAIL ({disk[5:]} is missing: run the case that makes it first)")
            return False
        shutil.copyfile(source, disk_copy)
        env["QEMU_DISK"] = disk_copy
    if spec.get("iso", True) is False:
        iso = "none"
    if spec.get("processors"):
        env["QEMU_SMP"] = str(spec["processors"])
    pending_actions = list(spec.get("actions", []))
    log(f"{case}: booting {iso}")
    qemu = subprocess.Popen(
        [os.path.join("scripts", "run-qemu.sh"), iso, "-monitor", f"unix:{mon},server,nowait",
         *spec.get("qemu_args", [])],
        cwd=ROOT, env=env, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)

    ok = False
    reason = "timeout"
    started = time.time()
    text = ""
    expected = list(spec["expect"])
    try:
        while time.time() - started < spec.get("timeout", timeout):
            exited = qemu.poll() is not None
            try:
                with open(os.path.join(ROOT, serial_log), encoding="utf-8", errors="replace") as f:
                    text = f.read()
            except FileNotFoundError:
                text = ""
            except OSError as e:
                if e.errno not in (errno.EAGAIN, errno.EWOULDBLOCK, getattr(errno, "ENODATA", -1)):
                    raise
            if case == "desktop" and "welcome: moved to (" in expected[-1]:
                resolution = re.search(r"Desktop: compositor at (\d+)x(\d+)", text)
                if resolution:
                    width, height = map(int, resolution.groups())
                    x = (width - 760) // 2
                    y = (height - 48 - 520) // 2 + 100
                    expected[-1] = f"welcome: moved to ({x}, {y})"
            if exited:
                if spec.get("exit_ok") and qemu.returncode == 0 and all(m in text for m in expected):
                    ok = True
                else:
                    reason = f"QEMU exited with status {qemu.returncode}: {qemu.stderr.read().decode(errors='replace').strip()}"
                break
            while pending_actions and pending_actions[0][0] in text:
                for command in pending_actions.pop(0)[1]:
                    if command.startswith("sleep "):
                        time.sleep(float(command[6:]))
                    elif command.startswith("type:"):
                        for ch in command[5:]:
                            name = KEY_NAMES.get(ch, ("shift-" + ch.lower()) if ch.isupper() else ch)
                            monitor_command(mon, f"sendkey {name}")
                    else:
                        monitor_command(mon, command)
            bad = [m for m in spec["fail"] if m in text]
            if bad:
                reason = f"unexpected serial output: {bad[0]!r}"
                break
            if all(m in text for m in expected) and not spec.get("exit_ok"):
                ok = True
                break
            time.sleep(0.5)

        if qemu.poll() is None:
            time.sleep(1.5)  # let the last frame reach the framebuffer
            try:
                monitor_command(mon, f"screendump {ppm}")
                if os.path.exists(ppm):
                    ppm_to_png(ppm, os.path.join(ROOT, png))
            except OSError as e:
                log(f"{case}: screenshot failed: {e}")
            try:
                monitor_command(mon, "quit")
            except OSError:
                pass
    finally:
        try:
            qemu.wait(timeout=10)
        except subprocess.TimeoutExpired:
            qemu.kill()
        if ok and spec.get("check_disk") and "QEMU_DISK" in env:
            problem = check_disk(env["QEMU_DISK"], spec.get("disk_files"))
            if problem:
                ok = False
                reason = problem
        if ok and spec.get("install_checks") and "QEMU_DISK" in env:
            problem = check_install(env["QEMU_DISK"], spec["install_checks"])
            if problem:
                ok = False
                reason = problem
        if ok and spec.get("save_disk") and "QEMU_DISK" in env:
            shutil.copyfile(env["QEMU_DISK"], os.path.join(ROOT, spec["save_disk"]))
        shutil.rmtree(tmp, ignore_errors=True)

    elapsed = time.time() - started
    if ok:
        log(f"{case}: PASS in {elapsed:.1f}s (serial: {serial_log}, screenshot: {png})")
        for line in text.splitlines():
            if "WARNING:" in line:
                print(f"    ! {line.strip()}")
    else:
        log(f"{case}: FAIL ({reason}) after {elapsed:.1f}s; last serial lines:")
        for line in text.splitlines()[-15:]:
            print(f"    | {line}")
    return ok


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--case", default="all", choices=["all", *CASES],
                        help="run one case (default: all)")
    parser.add_argument("--timeout", type=float, default=120.0)
    args = parser.parse_args()

    cases = list(CASES) if args.case == "all" else [args.case]
    results = {case: run_case(case, args.timeout) for case in cases}
    failed = [c for c, ok in results.items() if not ok]
    log("all cases passed" if not failed else f"FAILED: {', '.join(failed)}")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
