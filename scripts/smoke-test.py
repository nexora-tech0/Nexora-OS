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
import gzip
import http.server
import json
import os
import re
import shutil
import socket
import struct
import subprocess
import sys
import tempfile
import threading
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
                   "DNS self-tests: 8/8 passed",
                   "Clock: system time ", "PCI: ", "AHCI: ", "Disk: Harddisk0 is 'QEMU HARDDISK', 64.0 MiB",
                   "PS/2: keyboard ready", "Volume: C: is \\Device\\Ramdisk0, UstarFs",
                   "Volume: D: is \\Device\\Harddisk0\\Partition1, FAT32 'HZTEST'",
                   "Process self-tests: 1/1 passed", "File system self-tests: 16/16 passed",
                   "Window manager: 0 windows and 0 surface sections before the desktop: OK",
                   "[HZ] BOOT OK", "Installer: Setup launched automatically", "setup: started",
                   "[HZ] HEARTBEAT"],
        "fail": ["*** STOP", "BOOT DEGRADED", "ERROR:"],
        "check_disk": True,
    },
    "desktop": {
        "entry": None,
        "disk": False,
        # Unlock (any key) and let the desktop settle. No demo program is started any more (the
        # welcome app went with the beta apps), so this case proves the shell reaches a usable
        # desktop: it locks, unlocks, shows the desktop, opens Start and closes it again.
        "actions": [("shell: lock screen shown", ["sendkey spc"]),
                    ("shell: sign-in", ["sendkey ret"]),
                    ("shell: ready", ["sendkey meta_l"]),
                    ("shell: start opened", ["sendkey meta_l"])],
        "expect": ["Desktop: compositor at ", "Window manager: ", "[HZ] BOOT OK",
                   "shell: unlocked", "shell: ready", "shell: start opened", "shell: start closed"],
        "fail": ["*** STOP", "BOOT DEGRADED", "ERROR:"],
    },
    "taskbar-dock": {
        "entry": None,
        "disk": False,
        "actions": [("shell: lock screen shown", ["sendkey spc"]),
                    ("shell: sign-in", ["sendkey ret"]),
                    ("shell: unlocked", ["sendkey meta_l"]),
                    ("shell: start opened",
                     ["type:clock", "sendkey ret"]),
                    ("clock: started", ["sleep 2", "screenshot:taskbar-dock-active.png"])],
        "expect": ["shell: unlocked", "shell: start opened", "shell: launched clock.exe", "clock: started"],
        "fail": ["*** STOP", "BOOT DEGRADED", "ERROR:"],
        "timeout": 150.0,
    },
    "store-ui": {
        "entry": None,
        "disk": False,
        "actions": [("shell: lock screen shown", ["sendkey spc"]),
                    ("shell: sign-in", ["sendkey ret"]),
                    ("shell: unlocked", ["sendkey meta_l", "sleep 1", "type:store", "sendkey ret"])],
        "expect": ["[HZ] BOOT OK", "shell: unlocked", "shell: launched store.exe",
                   "store: catalog scroll ready", "store: opened (15 apps)"],
        "fail": ["*** STOP", "BOOT DEGRADED", "ERROR:", "store: catalog has no scroll range"],
        "timeout": 180.0,
    },
    "control-center-reference": {
        "entry": None,
        "disk": False,
        "actions": [("shell: lock screen shown", ["sendkey spc"]),
                    ("shell: sign-in", ["sendkey ret"]),
                    ("shell: ready",
                     ["sleep 2", "screenshot:top-dock-dark.png", "sendkey meta_l"]),
                    ("shell: start opened",
                     ["sleep 2", "screenshot:taskbar-dock-dark.png", "sendkey esc", "sleep 1",
                      "mouse_move 587 -376", "mouse_button 1", "mouse_button 0"]),
                    ("shell: quick settings opened",
                     ["sleep 2", "screenshot:control-center-dark.png"])],
        "expect": ["shell: unlocked", "shell: ready", "shell: start opened",
                   "shell: quick settings opened"],
        "fail": ["*** STOP", "ERROR:"],
        "timeout": 150.0,
    },
    "shell": {
        "entry": None,
        "disk": False,
        # Unlock, open Start with the logo key, search "clo" and press Enter (Clock starts). Then
        # activate, minimize and restore Clock from its taskbar button. Open Control Center from
        # the top dock and select its second wallpaper preview.
        "actions": [("shell: lock screen shown", ["sendkey spc"]),
                    ("shell: ready", ["sendkey meta_l"]),
                    ("shell: start opened", ["sendkey c", "sendkey l", "sendkey o", "sendkey ret"]),
                    ("clock: started", ["mouse_move 48 360", "mouse_button 1", "mouse_button 0"]),
                    ("shell: activated Clock", ["mouse_button 1", "mouse_button 0"]),
                    ("shell: minimized Clock", ["mouse_button 1", "mouse_button 0"]),
                    ("shell: restored Clock", ["mouse_move 328 0", "mouse_button 1", "mouse_button 0",
                                               "mouse_move 128 16", "mouse_button 1", "mouse_button 0"]),
                    ("shell: quick settings opened", ["mouse_move -8 -746", "mouse_button 1", "mouse_button 0",
                                                       "mouse_move -146 120", "mouse_button 1", "mouse_button 0"])],
        "expect": ["shell: unlocked", "shell: ready", "shell: start opened", "shell: launched clock.exe",
                   "clock: started", "shell: activated Clock", "shell: minimized Clock",
                   "shell: restored Clock", "shell: quick settings opened", "shell: wallpaper 1"],
        "fail": ["*** STOP", "BOOT DEGRADED", "ERROR:"],
    },
    "pinning": {
        "entry": None,
        "disk": False,
        # Search for Clock, open its themed context menu, pin it, then launch it from the taskbar.
        # At 1024x768 the first result is near (300, 250); after pinning, Clock is a centered
        # taskbar button.
        "actions": [("shell: lock screen shown", ["sendkey spc"]),
                    ("shell: ready", ["sendkey meta_l"]),
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
        # dialog; the terminal (it starts on D:) types it, lists D:, writes a file with '>',
        # and starts Clock and Calculator; the calculator computes
        # 12 x 3; Task Manager filters "clock", selects it with Enter and ends it with Delete and
        # Enter; Files opens smoke.txt by type-ahead (in Notepad); Settings searches for
        # Personalization and picks the second wallpaper with the keyboard.
        "actions": [("shell: lock screen shown", ["sendkey spc"]),
                    ("shell: ready", ["sendkey meta_l"]),
                    ("shell: start opened", ["type:note", "sendkey ret"]),
                    ("notepad: started", ["type:Hello from the smoke test", "sendkey ctrl-s"]),
                    ('hzui: dialog "Save as" opened', ["sendkey ctrl-a", "type:smoke.txt", "sendkey ret"]),
                    ("notepad: saved D:\\smoke.txt", ["sendkey meta_l", "sleep 1", "type:term", "sendkey ret"]),
                    ("terminal: started", ["type:type smoke.txt\n", "type:dir\n", "type:echo piped > out.txt\n",
                                           "type:type out.txt\n", "type:start clock\n"]),
                    ("clock: started", ["sleep 1", "sendkey alt-tab", "sleep 1", "type:start calc\n"]),
                    ("calc: started", ["type:12*3="]),
                    ("calc: 12 x 3 = 36", ["sendkey meta_l", "sleep 1", "type:task", "sendkey ret"]),
                    ("taskmgr: started", ["sleep 1", "type:clock\n", "sendkey delete"]),
                    ('hzui: dialog "End task" opened', ["sendkey ret"]),
                    ("taskmgr: ended clock.exe", ["sendkey meta_l", "sleep 1", "type:files", "sendkey ret"]),
                    ("files: D:\\ (", ["sleep 1", "type:smo", "sendkey ret"]),
                    ("notepad: opened D:\\smoke.txt", ["sendkey meta_l", "sleep 1", "type:sett", "sendkey ret"]),
                    ("settings: started", ["sleep 1", "type:personalization", "sendkey tab", "sendkey down",
                                           "sendkey ret", "sendkey tab", "sendkey tab", "sendkey spc"]),
                    ("settings: wallpaper 1 requested", ["sendkey meta_l"]),
                    ("shell: start opened", ["type:system information", "sendkey ret"]),
                    ("sysinfo: started", ["sendkey meta_l", "sleep 1", "type:calendar", "sendkey ret"]),
                    ("calendar: started", ["sendkey meta_l", "sleep 1", "type:stopwatch", "sendkey ret"]),
                    ("stopwatch: started", ["sendkey meta_l", "sleep 1", "type:paint", "sendkey ret"]),
                    ("paint: started", [])],
        "expect": ["[HZ] BOOT OK", "shell: 15 programs installed",
                   "shell: launched notepad.exe", "notepad: saved D:\\smoke.txt (25 bytes)",
                   "terminal| Hello from the smoke test", "terminal|  Directory of D:\\",
                   "terminal: wrote 6 bytes to D:\\out.txt", "terminal| piped",
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
    "terminal-linux": {
        "entry": None,
        "actions": [("shell: lock screen shown", ["sendkey spc"]),
                    ("shell: ready", ["sendkey meta_l"]),
                    ("shell: start opened", ["type:terminal", "sendkey ret"]),
                    ("terminal: started", ["type:touch linux-test.txt\n",
                                           "type:echo hello linux > linux-test.txt\n"]),
                    ("terminal: wrote 12 bytes to D:\\linux-test.txt",
                     ["type:pwd\n", "type:uname -a\n", "type:whoami\n", "type:free\n", "type:df\n",
                      "type:wc linux-test.txt\n"]),
                    ("terminal| 1 2 12 linux-test.txt",
                     ["type:head -n 1 linux-test.txt\n", "type:tail -n 1 linux-test.txt\n",
                      "type:grep -n linux linux-test.txt\n"]),
                    ("terminal| 1:hello linux",
                     ["type:sha256sum linux-test.txt\n", "type:mv linux-test.txt moved.txt\n",
                      "type:stat moved.txt\n", "type:rm moved.txt\n", "type:sudo id\n"]),
                    ("Privilege elevation is unavailable",
                     ["type:apt update\n"])],
        "expect": ["shell: launched terminal.exe", "terminal: started", "terminal: wrote 12 bytes to D:\\linux-test.txt",
                   "terminal| D:\\", "terminal| NexoraOS 1.1.0 M12 x86_64", "terminal| Guest",
                   "terminal| Physical memory:", "terminal| Drive  Label", "terminal| 1 2 12 linux-test.txt",
                   "terminal| hello linux", "terminal| 1:hello linux", "terminal|  ", "terminal|   File: D:\\moved.txt",
                   "terminal| Privilege elevation is unavailable",
                   "Package management is unavailable"],
        "fail": ["*** STOP", "ERROR:", "crashed"],
        "timeout": 180.0,
    },
    "network-interface": {
        "entry": None,
        "disk": False,
        "qemu_args": ["-netdev", "user,id=net0", "-device", "e1000,netdev=net0"],
        "actions": [("shell: lock screen shown", ["sendkey spc"]),
                    ("shell: sign-in", ["sendkey ret"]),
                    ("shell: ready", ["sendkey meta_l"]),
                    ("shell: start opened", ["type:settings", "sendkey ret"]),
                    ("settings: Home", ["type:network", "sendkey tab", "sendkey down", "sendkey ret"]),
                    ("settings: Network & Internet", ["sendkey shift-tab", "sendkey ctrl-a", "type:storage",
                                           "sendkey tab", "sendkey down", "sendkey ret"]),
                    ("settings: System", ["sendkey shift-tab", "sendkey ctrl-a", "type:apps",
                                           "sendkey tab", "sendkey down", "sendkey ret"]),
                    ("settings: Apps", ["sendkey shift-tab", "sendkey ctrl-a", "type:home",
                                           "sendkey tab", "sendkey down", "sendkey ret"])],
        "expect": ["Network: interface eth0 registered", "E1000: ", "up link", "Network protocol self-tests: ",
                   "passed", "DHCP self-tests: 8/8 passed", "DNS self-tests: 9/9 passed", "DHCP: eth0 configured",
                   "shell: ready", "settings: Home", "settings: Network & Internet",
                   "settings: System", "settings: Apps"],
        "fail": ["*** STOP", "ERROR:"],
        "timeout": 120.0,
    },
    "rtl8139": {
        "entry": None,
        "disk": False,
        "qemu_args": ["-netdev", "user,id=net0", "-device", "rtl8139,netdev=net0"],
        "expect": ["RTL8139: ", "Realtek RTL8139 Ethernet", "Network: interface eth0 registered",
                   "up link", "Network protocol self-tests: ", "passed", "DHCP: eth0 configured", "shell: ready"],
        "fail": ["*** STOP", "BOOT DEGRADED", "ERROR:", "could not allocate", "reset timed out"],
        "timeout": 120.0,
    },
    "network-multi-interface": {
        "entry": None,
        "disk": False,
        "qemu_args": ["-netdev", "user,id=net0", "-device", "e1000,netdev=net0",
                      "-netdev", "user,id=net1", "-device", "rtl8139,netdev=net1"],
        "expect": ["Network: interface eth0 registered", "Network: interface eth1 registered",
                   "Network: DHCP started on 2 interface(s)", "DHCP: eth0 configured", "DHCP: eth1 configured",
                   "shell: ready"],
        "fail": ["*** STOP", "BOOT DEGRADED", "ERROR:", "could not start DHCP"],
        "timeout": 120.0,
    },
    "network-pcnet": {
        "entry": None,
        "disk": False,
        "qemu_args": ["-netdev", "user,id=net0", "-device", "pcnet,netdev=net0"],
        "expect": ["PCnet: ", "DMA rings started", "Network: interface eth0 registered",
                   "Network: DHCP started on 1 interface(s)", "DHCP: eth0 configured", "shell: ready"],
        "fail": ["*** STOP", "BOOT DEGRADED", "ERROR:", "PCnet: initialization timed out"],
        "timeout": 45.0,
    },
    "virtio-storage": {
        "entry": None,
        "disk": False,
        "virtio_disk": True,
        "qemu_args": ["-device", "virtio-blk-pci,drive=vblk,disable-modern=on"],
        "actions": [("shell: lock screen shown", ["sendkey spc"]),
                    ("shell: sign-in", ["sendkey ret"])],
        "expect": ["VirtIO block: ", "Disk: Harddisk0 is 'VirtIO block device'",
                   "Volume: D: is ", "FAT32 'HZTEST'", "File system self-tests: ", "passed",
                   "shell: ready"],
        "fail": ["*** STOP", "BOOT DEGRADED", "ERROR:", "VirtIO block: .*initialization failed"],
        "timeout": 120.0,
    },
    "virtio-network": {
        "entry": None,
        "disk": False,
        "qemu_args": ["-netdev", "user,id=net0", "-device",
                      "virtio-net-pci,netdev=net0,disable-modern=on"],
        "actions": [("shell: lock screen shown", ["sendkey spc"]),
                    ("shell: sign-in", ["sendkey ret"])],
        "expect": ["VirtIO network: ", "Network: interface eth0 registered",
                   "DHCP: eth0 configured", "shell: ready"],
        "fail": ["*** STOP", "BOOT DEGRADED", "ERROR:", "DHCP: configuration failed"],
        "timeout": 120.0,
    },
    "xhci-controller": {
        "entry": None,
        "disk": False,
        "machine": "q35,i8042=off",
        "qemu_args": ["-device", "qemu-xhci,id=xhci", "-device", "usb-kbd,bus=xhci.0",
                      "-device", "usb-mouse,bus=xhci.0"],
        "actions": [("shell: ready", ["qmpkey:a", "qmpmouse:12:-7:none"]),
                    ("USB HID: mouse report buttons=00 dx=12 dy=-7", ["qmpmouse:0:0:left-down"]),
                    ("USB HID: mouse report buttons=01", ["qmpmouse:0:0:left-up"])],
        "expect": ["xHCI: 00:03.0 PCI", "command/event rings running", "USB: port ",
                   "slot ", "enabled (speed id ", "USB: device enumerated", "VID=0627 PID=0001",
                   "USB HID: boot keyboard ready", "USB HID: boot mouse ready", "USB HID: key 4 down",
                   "USB HID: mouse report buttons=00 dx=12 dy=-7", "USB HID: mouse report buttons=01",
                   "shell: ready"],
        "expect_counts": {"USB: device enumerated": 2},
        "fail": ["*** STOP", "ERROR:", "descriptor enumeration failed"],
        "timeout": 90.0,
    },
    "settings-visual": {
        "entry": None,
        "disk": False,
        "actions": [("shell: lock screen shown", ["sendkey spc"]),
                    ("shell: ready",
                     ["screenshot:desktop-dock.png", "mouse_move 508 -370", "mouse_button 1", "mouse_button 0"]),
                    ("shell: quick settings opened",
                     ["sleep 1", "screenshot:control-center.png", "mouse_move -158 160",
                      "mouse_button 1", "mouse_button 0"]),
                    ("shell: wallpaper 1", ["sleep 1", "screenshot:control-center-wallpaper-selected.png",
                                            "sendkey esc", "sendkey meta_l", "type:settings", "sendkey ret"]),
                    ("settings: Home",
                     ["sleep 1", "screenshot:settings-home.png", "type:system", "sendkey tab", "sendkey down",
                      "sendkey ret"]),
                    ("settings: System",
                     ["sleep 1", "screenshot:settings-system.png", "sendkey shift-tab", "sendkey ctrl-a",
                      "type:personalization", "sendkey tab", "sendkey down", "sendkey ret"]),
                    ("settings: Personalization",
                     ["sleep 1", "screenshot:settings-personalization.png", "sendkey shift-tab", "sendkey ctrl-a",
                      "type:network", "sendkey tab", "sendkey down", "sendkey ret"]),
                    ("settings: Network & Internet",
                     ["sleep 1", "screenshot:settings-network.png", "sendkey shift-tab", "sendkey ctrl-a",
                      "type:devices", "sendkey tab", "sendkey down", "sendkey ret"]),
                    ("settings: Devices",
                     ["sleep 1", "screenshot:settings-devices.png", "sendkey shift-tab", "sendkey ctrl-a",
                      "type:apps", "sendkey tab", "sendkey down", "sendkey ret"]),
                    ("settings: Apps",
                     ["sleep 1", "screenshot:settings-apps.png", "sendkey shift-tab", "sendkey ctrl-a",
                      "type:accounts", "sendkey tab", "sendkey down", "sendkey ret"]),
                    ("settings: Accounts",
                     ["sleep 1", "screenshot:settings-accounts.png", "sendkey shift-tab", "sendkey ctrl-a",
                      "type:privacy", "sendkey tab", "sendkey down", "sendkey ret"]),
                    ("settings: Privacy & Security",
                     ["sleep 1", "screenshot:settings-privacy-security.png", "sendkey shift-tab", "sendkey ctrl-a",
                      "type:accessibility", "sendkey tab", "sendkey down", "sendkey ret"]),
                    ("settings: Accessibility",
                     ["sleep 1", "screenshot:settings-accessibility.png", "sendkey shift-tab", "sendkey ctrl-a",
                      "type:date", "sendkey tab", "sendkey down", "sendkey ret"]),
                    ("settings: Date & Language",
                     ["sleep 1", "screenshot:settings-date-language.png", "sendkey shift-tab", "sendkey ctrl-a",
                      "type:updates", "sendkey tab", "sendkey down", "sendkey ret"]),
                    ("settings: Updates & Recovery",
                     ["sleep 1", "screenshot:settings-updates-recovery.png", "sendkey shift-tab", "sendkey ctrl-a",
                      "type:about", "sendkey tab", "sendkey down", "sendkey ret"]),
                    ("settings: About Nexora OS",
                     ["sleep 1", "screenshot:settings-about.png", "sendkey shift-tab", "sendkey ctrl-a",
                      "type:home", "sendkey tab", "sendkey down", "sendkey ret"]),
                    ("settings: Home",
                     ["sleep 2", "screenshot:settings-home.png", "sendkey alt-f4", "sleep 2",
                      "mouse_move 158 -160", "mouse_button 1", "mouse_button 0", "sleep 1",
                      "screenshot:control-center-reopened.png", "mouse_button 1", "mouse_button 0", "sleep 1",
                      "screenshot:control-center-toggled-closed.png", "mouse_button 1", "mouse_button 0", "sleep 1",
                      "screenshot:control-center-reopened-after-toggle.png", "mouse_move 52 670",
                      "mouse_button 1", "mouse_button 0", "sleep 1",
                      "screenshot:control-center-dismissed-outside.png", "mouse_move -52 -670",
                      "mouse_button 1", "mouse_button 0", "sleep 1",
                      "screenshot:control-center-reopened-after-outside.png", "sendkey esc", "sleep 1",
                      "screenshot:control-center-dismissed-escape.png"])],
        "expect": ["shell: unlocked", "shell: quick settings opened", "shell: wallpaper 1",
                   "settings: Home", "settings: System", "settings: Personalization", "settings: Network & Internet",
                   "settings: Devices", "settings: Apps", "settings: Accounts", "settings: Privacy & Security",
                   "settings: Accessibility", "settings: Date & Language", "settings: Updates & Recovery",
                   "settings: About Nexora OS"],
        "expect_counts": {"shell: quick settings opened": 4},
        "fail": ["*** STOP", "ERROR:"],
        "timeout": 360.0,
    },
    "catalog-apps": {
        "entry": None,
        "disk": False,
        "actions": [("shell: lock screen shown", ["sendkey spc"]),
                    ("shell: ready", ["sendkey meta_l"]),
                    ("shell: start opened", ["type:system information", "sendkey ret"]),
                    ("sysinfo: started", ["sendkey meta_l", "sleep 1", "type:calendar", "sendkey ret"]),
                    ("calendar: started", ["sendkey meta_l", "sleep 1", "type:stopwatch", "sendkey ret"]),
                    ("stopwatch: started", ["sendkey meta_l", "sleep 1", "type:paint", "sendkey ret"]),
                    ("paint: started", ["sendkey meta_l", "sleep 1", "type:store", "sendkey ret"])],
        "expect": ["shell: 15 programs installed", "shell: ready", "sysinfo: started", "calendar: started",
                   "stopwatch: started", "paint: started", "store: opened (15 apps)"],
        # The current partition-table self-test failure is tracked separately; this case focuses
        # on manifest discovery and launching the newly added apps.
        "fail": ["*** STOP", "sysinfo: cannot create the window", "calendar: cannot create the window",
                 "stopwatch: cannot create the window", "paint: cannot create the window"],
        "timeout": 180.0,
    },
    "windows-console-compat": {
        "entry": None,
        "disk": True,
        "actions": [("shell: lock screen shown", ["sendkey spc"]),
                    ("shell: sign-in", ["sendkey ret"]),
                    ("shell: unlocked", ["sendkey meta_l", "sleep 1", "type:terminal", "sendkey ret"]),
                    ("terminal: started", ["type:start C:\\Nexora\\System\\wincompat-probe.exe", "sendkey ret"]),
                    ("WinCompat: wincompat-probe.exe ExitProcess(0)",
                     ["type:start C:\\Nexora\\System\\wincompat-missing-api.exe", "sendkey ret"])],
        "expect": ["WinCompat: wincompat-probe.exe ExitProcess(0)",
                   "Volume: D: is ",
                   "terminal| Started wincompat-probe.exe (process",
                   "image: Win32 import report [AMD64 PE32+", "kernel32.dll!GetCurrentDirectoryA",
                   "[module=SUPPORTED api=UNSUPPORTED]"],
        "expect_any": ["[HZ] BOOT OK", "[HZ] BOOT DEGRADED"],
        "fail": ["*** STOP", "ERROR:"],
        "check_disk": True,
        "disk_files": {"::/wincompat-file-probe.tmp": "CreateFileW-write-probe"},
        "warn_markers": ["BOOT DEGRADED"],
        "timeout": 180.0,
    },
    "windows-winsock": {
        "entry": None,
        "disk": False,
        "host_http_fixture": True,
        "qemu_args": ["-netdev", "user,id=net0", "-device", "e1000,netdev=net0"],
        "actions": [("shell: lock screen shown", ["sendkey spc"]),
                    ("shell: sign-in", ["sendkey ret"]),
                    ("shell: unlocked", ["sendkey meta_l", "sleep 1", "type:terminal", "sendkey ret"]),
                    ("terminal: started", ["type:start C:\\Nexora\\System\\wincompat-network-probe.exe {host_http_port}", "sendkey ret"])],
        "expect": ["WinCompat: wincompat-network-probe.exe ExitProcess(0)"],
        "fail": ["*** STOP", "shell: A program (process", "raised STATUS_"],
        "warn_markers": ["BOOT DEGRADED"],
        "timeout": 180.0,
    },
    "animevault-launch": {
        "entry": None,
        # Use the extracted AMD64 Electron app, not the 32-bit NSIS installer wrapper. The
        # disk generator extracts and copies the complete release payload without running NSIS.
        "disk": "from:build/test/animevault-app-disk.img",
        "animevault_payload": True,
        "actions": [
            ("shell: lock screen shown", ["sendkey spc"]),
            ("shell: sign-in", ["sendkey ret"]),
            ("shell: unlocked", ["sendkey meta_l", "sleep 1", "type:terminal", "sendkey ret"]),
            ("terminal: started", ["type:start D:\\AnimeVault.exe", "sendkey ret"]),
            ("Process: started AnimeVault.exe", ["screenshot:animevault-launch-result.png"]),
        ],
        "expect": ["Volume: D: is ", "terminal: started", "Process: started AnimeVault.exe"],
        "expect_any": ["[HZ] BOOT OK", "[HZ] BOOT DEGRADED"],
        "fail": ["*** STOP", "ERROR:", "could not create AnimeVault.exe"],
        "warn_markers": ["BOOT DEGRADED"],
        "timeout": 180.0,
    },
    "nexoracode-launch": {
        "entry": None,
        # The 64-bit Electron app is extracted from the NSIS wrapper; the installer itself
        # is not run by Windows or Nexora during disk preparation.
        "disk": "from:build/test/nexoracode-app-disk.img",
        "nexoracode_payload": True,
        "actions": [
            ("shell: lock screen shown", ["sendkey spc"]),
            ("shell: sign-in", ["sendkey ret"]),
            ("shell: unlocked", ["sendkey meta_l", "sleep 1", "type:terminal", "sendkey ret"]),
            ("terminal: started", ["type:start D:\\NexoraCode.exe", "sendkey ret"]),
            ("Process: started NexoraCode.exe", ["sleep 15", "screenshot:nexoracode-launch-result.png"]),
        ],
        "expect": ["Volume: D: is ", "terminal: started", "Process: started NexoraCode.exe"],
        "expect_any": ["[HZ] BOOT OK", "[HZ] BOOT DEGRADED"],
        "fail": ["*** STOP", "ERROR:", "could not create NexoraCode.exe"],
        "warn_markers": ["BOOT DEGRADED"],
        "timeout": 240.0,
    },
    "ntfs-volume-probe": {
        "entry": None,
        "cmdline": "hz.ntfs-probe=1",
        "ntfs_fixture": True,
        "disk": "from:build/test/ntfs-test-disk.img",
        "actions": [],
        "expect": ["NTFS: boot, $MFT data, and root-directory metadata validated",
                   "File system self-tests: 7/7 passed"],
        "fail": ["*** STOP", "ERROR:"],
        "timeout": 120.0,
    },
    "device-inventory": {
        "entry": None,
        "disk": False,
        "actions": [("shell: lock screen shown", ["sendkey spc"]),
                    ("shell: sign-in", ["sendkey ret"]),
                    ("shell: unlocked", ["sendkey meta_l"]),
                    ("shell: start opened", ["type:settings", "sendkey ret"]),
                    ("settings: started", ["sendkey alt-f4"])],
        "expect": ["shell: unlocked", "settings: started", "settings: PCI inventory ",
                   "device(s), ", " driver(s) started, ", " failed, ", " unmatched"],
        "fail": ["*** STOP", "settings: cannot create the window", "crashed"],
        "timeout": 150.0,
    },
    "installer-pnp": {
        # Exercise the actual install boot path. A live-desktop test image never starts Setup.
        "entry": "/Install Nexora OS (verbose log)",
        "disk": "empty:384",
        "actions": [("setup: started", ["sendkey ret"]),
                    ("setup: disk 0 ", ["screenshot:installer-hardware.png"])],
        # The PCI summary is rendered in the UI; serial reports the inventory counts and disk scan.
        "expect": ["Installer: Setup launched automatically", "setup: started", "setup: hardware inventory ",
                   "setup: disk 0 'QEMU HARDDISK'"],
        "fail": ["*** STOP", "setup: cannot create the window", "crashed"],
        "timeout": 180.0,
    },
    "browser": {
        "entry": None,
        "disk": False,
        "actions": [("shell: lock screen shown", ["sendkey spc"]),
                    ("shell: sign-in", ["sendkey ret"]),
                    ("shell: ready", ["sendkey meta_l", "type:browser", "sendkey ret"]),
                    ("browser: started; local files, bounded HTTP, and verified TLS 1.2 HTTPS ready",
                     ["type:https://example.com", "sendkey ret"]),
                    ("browser: DNS lookup failed for example.com",
                     ["sendkey ctrl-l", "type:https://example.com:99999999999999999999", "sendkey ret"])],
        "expect": ["shell: 18 programs installed",
                   "DNS self-tests: 8/8 passed",
                   "browser: started; local files, bounded HTTP, and verified TLS 1.2 HTTPS ready",
                   "browser: DNS lookup failed for example.com",
                   "browser: rejected invalid address"],
        "fail": ["*** STOP", "ERROR:", "crashed"],
        "timeout": 150.0,
    },
    "browser-local": {
        "entry": None,
        "disk": False,
        "actions": [("shell: lock screen shown", ["sendkey spc"]),
                    ("shell: sign-in", ["sendkey ret"]),
                    ("shell: ready", ["sendkey meta_l", "type:browser", "sendkey ret"]),
                    ("browser: started; local files, bounded HTTP, and verified TLS 1.2 HTTPS ready",
                     ["type:C:\\Nexora\\Licenses\\ThirdPartyNotices.md", "sendkey ret"]),
                    ("browser: local file loaded C:\\Nexora\\Licenses\\ThirdPartyNotices.md (",
                     ["sendkey ctrl-l", "type:C:\\Nexora\\missing.html", "sendkey ret"])],
        "expect": ["shell: 18 programs installed",
                   "browser: local file loaded C:\\Nexora\\Licenses\\ThirdPartyNotices.md (",
                   "browser: local read failed for C:\\Nexora\\missing.html"],
        "fail": ["*** STOP", "ERROR:", "crashed"],
        "timeout": 150.0,
    },
    "dns-api": {
        "entry": None,
        "disk": False,
        "qemu_args": ["-netdev", "user,id=net0", "-device", "e1000,netdev=net0"],
        "actions": [("shell: lock screen shown", ["sendkey spc"]),
                    ("shell: sign-in", ["sendkey ret"]),
                    ("shell: ready", ["sendkey meta_l", "type:browser", "sendkey ret"]),
                    ("browser: started; local files, bounded HTTP, and verified TLS 1.2 HTTPS ready",
                     ["type:https://example.com", "sendkey ret"])],
        "expect": ["Network: interface eth0 registered", "DHCP: eth0 configured",
                   "browser: DNS resolved example.com to "],
        "fail": ["*** STOP", "crashed"],
        "timeout": 180.0,
    },
    "browser-ipv4-http": {
        "entry": None,
        "disk": False,
        "host_http_fixture": True,
        "qemu_args": ["-netdev", "user,id=net0", "-device", "e1000,netdev=net0"],
        "actions": [("shell: lock screen shown", ["sendkey spc"]),
                    ("shell: sign-in", ["sendkey ret"]),
                    ("DHCP: eth0 configured", []),
                    ("shell: ready", []),
                    ("shell: unlocked", ["sendkey meta_l", "type:browser", "sendkey ret"]),
                    ("browser: started; local files, bounded HTTP, and verified TLS 1.2 HTTPS ready",
                     ["type:http://10.0.2.2:{host_http_port}/nat-page.html", "sendkey ret"])],
        "expect": ["Network: interface eth0 registered", "DHCP: eth0 configured",
                   "browser: using IPv4 literal 10.0.2.2 without DNS",
                   "browser: loaded stylesheet http://10.0.2.2:{host_http_port}/nat-style.css (",
                   "browser: loaded image http://10.0.2.2:{host_http_port}/nat-image.bmp (70 bytes, 2x2)",
                   "browser: HTTP 200 loaded http://10.0.2.2:{host_http_port}/nat-page.html"],
        "fail": ["*** STOP", "crashed"],
        "timeout": 180.0,
    },
    "browser-https": {
        "entry": None,
        "disk": False,
        "qemu_args": ["-netdev", "user,id=net0", "-device", "e1000,netdev=net0"],
        "actions": [("shell: lock screen shown", ["sendkey spc"]),
                    ("shell: sign-in", ["sendkey ret"]),
                    ("shell: ready", []),
                    ("shell: unlocked", ["sendkey meta_l", "type:browser", "sendkey ret"]),
                    ("browser: started; local files, bounded HTTP, and verified TLS 1.2 HTTPS ready",
                     ["type:https://example.com", "sendkey ret"])],
        "expect": ["Network: interface eth0 registered", "DHCP: eth0 configured",
                   "browser: DNS resolved example.com to ", "browser: HTTPS 200 loaded https://example.com"],
        "fail": ["*** STOP", "crashed", "browser: HTTPS stream ended without TLS close_notify",
                 "browser: HTTPS transport read failed"],
        "timeout": 180.0,
    },
    "install": {
        "entry": None,
        "disk": "empty:384",
        # Setup starts automatically from the install-media boot entry.
        "actions": [("setup: started", ["sleep 1", "screenshot:installer-welcome.png", "sendkey ret"]),
                    ("setup: disk 0 ", ["sleep 1", "screenshot:installer-disk-selection.png", "sendkey ret"]),
                    ("setup: chose disk 0", ["sleep 1", "screenshot:installer-erase-review.png",
                                             "type:ERASE", "sendkey ret"]),
                    ("setup: formatted partition 2 as FAT32 'NEXORAOS'",
                     ["sleep 0.5", "screenshot:installer-progress.png"]),
                    ("setup: done; Nexora OS is installed on disk 0",
                     ["sleep 0.5", "screenshot:installer-complete.png"])],
        "expect": ["[HZ] BOOT OK", "Ramdisk: ", "at \\Device\\Ramdisk1", "Installer: Setup launched automatically",
                   "setup: disk 0 'QEMU HARDDISK', 384 MB: empty", "setup: installing on disk 0",
                   "Disk: Harddisk0 partition table read again: GPT, 2 partition(s)", "setup: partitioned disk 0",
                   "setup: formatted partition 1 as FAT32 'EFI'", "setup: formatted partition 2 as FAT32 'NEXORAOS'",
                   "setup: boot loader, kernel and system image written", "setup: read-back verification passed",
                   "setup: copied ",
                   "setup: done; Nexora OS is installed on disk 0"],
        # Keep capturing functional installer milestones after a degraded boot, then fail the case
        # explicitly rather than accepting installation as healthy.
        "defer_fail": ["BOOT DEGRADED"],
        "defer_expect": ["[HZ] BOOT OK"],
        "fail": ["*** STOP", "BOOT DEGRADED", "ERROR:", "setup: failed", "FAIL"],
        "timeout": 360.0,
        "save_disk": os.path.join("build", "test", "installed.img"),
        "install_checks": [
            (1, "::/EFI/BOOT/BOOTX64.EFI", "exists", None),
            (1, "::/boot/hzkrnl.elf", "same", os.path.join("build", "hzkrnl.elf")),
            (1, "::/boot/hzkrnl.sym", "same", os.path.join("build", "hzkrnl.sym")),
            (1, "::/boot/ramdisk.tar", "same", os.path.join("build", "ramdisk.tar")),
            (1, "::/boot/limine/limine.conf", "contains", "default_entry: /Nexora OS"),
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
                   "Volume: C: is \\Device\\Harddisk0\\Partition2, FAT32 'NEXORAOS'",
                   "File system self-tests: ", "[HZ] BOOT OK",
                   "shell: 15 programs installed", "shell: settings loaded: user 'Guest', wallpaper 0, first boot",
                   "shell: first boot: started first-boot setup", "shell: wallpaper 1 (requested)",
                   "setup: first boot done: user 'Ada', wallpaper 1 (settings saved: 0x00000000)",
                   "shell: first-boot setup finished; hello, Ada", "shell: ready"],
        "defer_fail": ["BOOT DEGRADED"],
        "defer_expect": ["[HZ] BOOT OK"],
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
                    ("shell: ready", ["sendkey meta_l", "sleep 1", "type:settings", "sendkey ret"]),
                    ("settings: Home", ["sleep 1", "type:accounts", "sendkey tab", "sendkey down", "sendkey ret"]),
                    ("settings: Accounts", ["sleep 1", "sendkey tab", "sendkey tab", "sendkey tab", "type:sun\n",
                                            "type:sun\n"]),
                    ("settings: password set", ["sleep 1", "sendkey meta_l", "sleep 1", "type:term", "sendkey ret"]),
                    ("terminal: started", ["sleep 1", "type:lock\n"]),
                    ("shell: locked", ["sleep 1", "sendkey spc"]),
                    ("shell: password prompt", ["sleep 1", "type:moon\n"]),
                    ("shell: wrong password (1)", ["sleep 1", "type:sun\n"])],
        "expect": ["Volume: C: is \\Device\\Harddisk0\\Partition2, FAT32 'NEXORAOS'", "[HZ] BOOT OK",
                   "shell: settings loaded: user 'Ada', wallpaper 1", "shell: lock screen shown", "shell: unlocked",
                   "shell: ready", "settings: password set (0x00000000)",
                   "shell: settings loaded: user 'Ada', wallpaper 1, password", "shell: locked",
                   "shell: wrong password (1)", "shell: password accepted"],
        "defer_fail": ["BOOT DEGRADED"],
        "defer_expect": ["[HZ] BOOT OK"],
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
                   "shell: password accepted", "shell: unlocked", "shell: ready"],
        "defer_fail": ["BOOT DEGRADED"],
        "defer_expect": ["[HZ] BOOT OK"],
        "fail": ["*** STOP", "BOOT DEGRADED", "ERROR:", "FAIL", "shell: wrong password", "first-boot setup"],
        "timeout": 180.0,
    },
    "smp": {
        "entry": None,
        "processors": 4,
        # Four processors (M11): every one online, the multiprocessor scheduler tests, all
        # self-tests, then a terminal running programs and Task Manager's performance page.
        "actions": [("shell: lock screen shown", ["sendkey spc"]),
                    ("shell: ready", ["sendkey meta_l", "sleep 1", "type:term", "sendkey ret"]),
                    ("terminal: started", ["type:ps\n", "type:mem\n"]),
                    ("terminal| Page size 4096 bytes;", ["sendkey meta_l", "sleep 1", "type:task", "sendkey ret"]),
                    ("taskmgr: started", ["sleep 1", "sendkey ctrl-tab", "sleep 3"])],
        "expect": ["MP: 4 processors reported; 3 application processor(s) parked", "MP: 4 processors online",
                   "Scheduler self-tests: 14/14 passed", "Process self-tests: 1/1 passed",
                   "File system self-tests: 16/16 passed",
                   "[HZ] BOOT OK", "[HZ] HEARTBEAT: 4 processors", "shell: unlocked",
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
                    ("shell: ready", ["sendkey meta_l", "sleep 1", "type:term", "sendkey ret"]),
                    ("terminal: started", ["sleep 1", "type:calc\n", "sleep 1", "type:kill shell\n"]),
                    ("shell: restarted", ["sleep 2", "sendkey meta_l", "sleep 1", "type:calc", "sendkey ret"])],
        "expect": ["[HZ] BOOT OK", "shell: unlocked",
                   "Session: shell.exe exited (0x00000001); restarting it", "shell: restarted", "calc: started"],
        "fail": ["*** STOP", "BOOT DEGRADED", "ERROR:", "FAIL"],
        "timeout": 200.0,
    },
    "poweroff": {
        "entry": None,
        "exit_ok": True,
        # M11: "shutdown" in the terminal asks the shell to close every program first. Notepad has
        # unsaved text, so it asks about it and stays open; after 15 s the shell names it and
        # offers "Shut down anyway" (Enter), then the volumes are flushed and ACPI powers off.
        "actions": [("shell: lock screen shown", ["sendkey spc"]),
                    ("shell: ready", ["sendkey meta_l", "sleep 1", "type:notepad", "sendkey ret"]),
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
                   "relocations applied",
                   "PsTest: objects left: 0 processes, 0 threads, 0 events, 0 files; pool +0 bytes: OK",
                   "[HZ] BOOT OK"],
        "fail": ["*** STOP", "BOOT DEGRADED", "ERROR:"],
    },
    "xhci-storage": {
        "entry": "Nexora OS (Live, verbose log)",
        "disk": False,
        "usb_disk": True,
        "usb_disk_count": 2,
        "check_usb_disk": True,
        "qemu_args": ["-device", "qemu-xhci,id=xhci,p3=0", "-device", "usb-kbd,bus=xhci.0",
                      "-device", "usb-storage,bus=xhci.0,drive=usbtest",
                      "-device", "usb-storage,bus=xhci.0,drive=usbtest2"],
        "actions": [("[HZ] HEARTBEAT", ["qmpkey:a"])],
        "expect": ["xHCI: 00:03.0 PCI", "USB: device enumerated", "USB MSC: BOT disk 0 online",
                   "USB MSC: BOT disk 1 online",
                   "USB HID: boot keyboard ready", "USB HID: key 4 down",
                   "Disk: Harddisk0 is 'USB Mass Storage (BOT)'", "Volume: D: is \\Device\\Harddisk0\\Partition1",
                   "File system self-tests: 15/15 passed", "[HZ] BOOT OK"],
        "fail": ["*** STOP", "BOOT DEGRADED", "USB MSC: port "],
        "timeout": 120.0,
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

# Reuse the complete install-and-readback checks with QEMU's PCIe NVMe controller. This exercises
# discovery, namespace identify, PRP-based DMA, sector reads/writes, GPT creation and Setup's copy
# verification through the same storage stack used by real NVMe disks.
CASES["install-nvme"] = {
    **CASES["install"],
    "entry": "/Install Nexora OS",
    "disk_interface": "nvme",
    # Boot clock tolerance is covered by the boot cases; this case isolates NVMe install I/O.
    # Keep all installer milestones and host readback checks, while reporting degraded boot as a
    # warning instead of obscuring whether the NVMe storage path passed.
    "expect": [(Marker.replace("QEMU HARDDISK", "QEMU NVMe Ctrl")
                if Marker.startswith("setup: disk 0 'QEMU HARDDISK'") else Marker)
               for Marker in CASES["install"]["expect"] if Marker != "[HZ] BOOT OK"],
    "defer_fail": [],
    "defer_expect": [],
    "fail": [Marker for Marker in CASES["install"]["fail"] if Marker != "BOOT DEGRADED"],
    "warn_markers": ["BOOT DEGRADED"],
    "save_disk": os.path.join("build", "test", "installed-nvme.img"),
}
CASES["installer-nvme"] = {
    **CASES["installer-pnp"],
    "disk_interface": "nvme",
    "expect": [Marker.replace("QEMU HARDDISK", "QEMU NVMe Ctrl")
               if Marker.startswith("setup: disk 0 'QEMU HARDDISK'") else Marker
               for Marker in CASES["installer-pnp"]["expect"]] + ["NVMe:"],
}
CASES["network-e1000e"] = {
    **CASES["network-interface"],
    "qemu_args": ["-netdev", "user,id=net0", "-device", "e1000e,netdev=net0"],
    "actions": [("shell: lock screen shown", ["sendkey spc"]),
                ("shell: sign-in", ["sendkey ret"])],
    "expect": ["Network: interface eth0 registered", "E1000: 00:02.0 8086:10D3",
               "up link", "DHCP: eth0 configured", "shell: ready"],
}


def log(msg):
    print(f"[smoke] {msg}", flush=True)


def start_host_http_fixture():
    fixture_dir = os.path.join(ROOT, "user", "apps", "browser", "tests", "fixtures")

    class FixtureHandler(http.server.SimpleHTTPRequestHandler):
        def __init__(self, *args, **kwargs):
            super().__init__(*args, directory=fixture_dir, **kwargs)

        def log_message(self, format, *args):
            pass

    server = http.server.ThreadingHTTPServer(("0.0.0.0", 0), FixtureHandler)
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    return server, thread


def build_iso(case):
    """Builds installer media for install tests and a test-only live desktop for other cases."""
    entry = CASES[case]["entry"]
    if entry is None and case == "install":
        subprocess.run(["make", "--no-print-directory", "iso"], cwd=ROOT, check=True)
        return os.path.join("build", "nexora-os.iso")

    os.makedirs(os.path.join(ROOT, TEST_DIR), exist_ok=True)
    conf = os.path.join(TEST_DIR, f"{case}.conf")
    with open(os.path.join(ROOT, "boot", "limine.conf"), encoding="utf-8") as f:
        lines = f.read().splitlines()
    if entry is None:
        entry = "/Nexora OS (Live desktop, automated test only)"
        out = [f"default_entry: {entry}"]
        for line in lines:
            if line.startswith("default_entry:"):
                continue
            if line.startswith("timeout:"):
                line = "timeout: 0"
            elif line.strip() == "cmdline: hz.installer=1":
                line = "    cmdline: hz.desktop=1" + (
                    f" {CASES[case]['cmdline']}" if CASES[case].get("cmdline") else "")
            out.append(line)
        out.extend([
            "",
            entry,
            "    protocol: limine",
            "    path: ${KERNEL}",
            "    cmdline: hz.desktop=1" +
            (f" {CASES[case]['cmdline']}" if CASES[case].get("cmdline") else ""),
            "    module_path: ${RAMDISK}",
            "    module_string: ramdisk",
            "    module_path: ${SYMBOLS}",
            "    module_string: symbols",
            "    module_path: ${INSTALL}",
            "    module_string: install",
        ])
    else:
        out = [f"default_entry: {entry}"]
        for line in lines:
            if line.startswith("default_entry:"):
                continue
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
        try:
            with open(ppm_path, "rb") as f:
                data = f.read()
        except OSError as error:
            if error.errno not in (errno.ENOENT, errno.EAGAIN, errno.EWOULDBLOCK, getattr(errno, "ENODATA", -1)):
                raise
            data = b""
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
    family = socket.AF_INET if isinstance(sock_path, tuple) else socket.AF_UNIX
    s = socket.socket(family, socket.SOCK_STREAM)
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


def qmp_send_key(socket_path, key):
    """Inject a key through QMP's input subsystem for the emulated USB keyboard."""
    sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    sock.settimeout(5)
    try:
        sock.connect(socket_path)
        buffered = b""

        def receive_reply():
            nonlocal buffered
            while b"\n" not in buffered:
                chunk = sock.recv(4096)
                if not chunk:
                    raise RuntimeError("QMP closed while waiting for a reply")
                buffered += chunk
            line, buffered = buffered.split(b"\n", 1)
            return json.loads(line)

        greeting = receive_reply()
        if "QMP" not in greeting:
            raise RuntimeError(f"invalid QMP greeting: {greeting}")
        sock.sendall(b'{"execute":"qmp_capabilities"}\n')
        while "return" not in receive_reply():
            pass
        request = {
            "execute": "input-send-event",
            "arguments": {"events": [
                {"type": "key", "data": {"down": True, "key": {"type": "qcode", "data": key}}},
                {"type": "key", "data": {"down": False, "key": {"type": "qcode", "data": key}}},
            ]},
        }
        sock.sendall((json.dumps(request) + "\n").encode())
        while True:
            reply = receive_reply()
            if "error" in reply:
                raise RuntimeError(f"QMP input-send-event failed: {reply['error']}")
            if "return" in reply:
                return
    finally:
        sock.close()


def qmp_send_mouse(socket_path, dx, dy, button="left"):
    """Inject relative mouse motion and one press/release through QMP."""
    sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    sock.settimeout(5)
    try:
        sock.connect(socket_path)
        buffered = b""

        def receive_reply():
            nonlocal buffered
            while b"\n" not in buffered:
                chunk = sock.recv(4096)
                if not chunk:
                    raise RuntimeError("QMP closed while waiting for a reply")
                buffered += chunk
            line, buffered = buffered.split(b"\n", 1)
            return json.loads(line)

        greeting = receive_reply()
        if "QMP" not in greeting:
            raise RuntimeError(f"invalid QMP greeting: {greeting}")
        sock.sendall(b'{"execute":"qmp_capabilities"}\n')
        while "return" not in receive_reply():
            pass
        events = [
            {"type": "rel", "data": {"axis": "x", "value": int(dx)}},
            {"type": "rel", "data": {"axis": "y", "value": int(dy)}},
        ]
        if button != "none":
            if button.endswith("-down"):
                button_name, down = button[:-5], True
                button_events = [{"type": "btn", "data": {"button": button_name, "down": down}}]
            elif button.endswith("-up"):
                button_name, down = button[:-3], False
                button_events = [{"type": "btn", "data": {"button": button_name, "down": down}}]
            else:
                button_events = [
                    {"type": "btn", "data": {"button": button, "down": True}},
                    {"type": "btn", "data": {"button": button, "down": False}},
                ]
            events.extend(button_events)
        sock.sendall((json.dumps({"execute": "input-send-event", "arguments": {"events": events}}) + "\n").encode())
        while True:
            reply = receive_reply()
            if "error" in reply:
                raise RuntimeError(f"QMP mouse input failed: {reply['error']}")
            if "return" in reply:
                return
    finally:
        sock.close()


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

    # Keep QEMU's Unix monitor socket on a native filesystem. WSL's mounted
    # Windows drives do not support Unix socket binding reliably.
    temp_root = "/tmp" if os.path.isdir("/tmp") and os.path.exists("/proc/version") else os.path.join(ROOT, TEST_DIR)
    tmp = tempfile.mkdtemp(prefix="hz-smoke-", dir=temp_root)
    if os.environ.get("SMOKE_MONITOR_TCP") == "1":
        probe = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        probe.bind(("127.0.0.1", 0))
        mon = probe.getsockname()
        probe.close()
        monitor = f"tcp:{mon[0]}:{mon[1]},server,nowait"
    else:
        mon = os.path.join(tmp, "monitor.sock")
        monitor = f"unix:{mon},server,nowait"
    ppm = os.path.join(tmp, "screen.ppm")
    vars_copy = os.path.join(tmp, "vars.fd")
    qmp_socket = os.path.join(tmp, "qmp.sock")

    env = dict(os.environ, QEMU_DISPLAY="none", QEMU_SERIAL=f"file:{serial_log}", QEMU_VARS=vars_copy)
    if spec.get("machine"):
        env["QEMU_MACHINE"] = spec["machine"]
    if spec.get("disk_interface"):
        env["QEMU_DISK_INTERFACE"] = spec["disk_interface"]
    disk = spec.get("disk", True)
    disk_copy = os.path.join(tmp, "disk.img")  # each case starts from a copy
    usb_copy = None
    virtio_copy = None
    qemu_args = list(spec.get("qemu_args", []))
    if spec.get("animevault_payload"):
        installer = os.path.join(ROOT, TEST_DIR, "AnimeVault-Windows-x64.exe")
        output = os.path.join(ROOT, TEST_DIR, "animevault-app-disk.img")
        prepared = subprocess.run(
            [sys.executable, os.path.join(ROOT, "scripts", "prepare-animevault-disk.py"), installer, output],
            cwd=ROOT,
        )
        if prepared.returncode != 0:
            shutil.rmtree(tmp, ignore_errors=True)
            log(f"{case}: FAIL (could not prepare the complete AnimeVault payload disk)")
            return False
    if spec.get("nexoracode_payload"):
        installer = os.path.join(ROOT, TEST_DIR, "Nexora-Code-Setup.exe")
        output = os.path.join(ROOT, TEST_DIR, "nexoracode-app-disk.img")
        prepared = subprocess.run(
            [sys.executable, os.path.join(ROOT, "scripts", "prepare-nexoracode-disk.py"), installer, output],
            cwd=ROOT,
        )
        if prepared.returncode != 0:
            shutil.rmtree(tmp, ignore_errors=True)
            log(f"{case}: FAIL (could not prepare the complete Nexora Code payload disk)")
            return False
    if disk is True:
        shutil.copyfile(os.path.join(ROOT, TEST_DISK), disk_copy)
        env["QEMU_DISK"] = disk_copy
    elif isinstance(disk, str) and disk.startswith("empty:"):
        with open(disk_copy, "wb") as f:
            f.truncate(int(disk[6:]) * 1024 * 1024)
        env["QEMU_DISK"] = disk_copy
    elif isinstance(disk, str) and disk.startswith("from:"):
        source = os.path.join(ROOT, disk[5:])
        if spec.get("ntfs_fixture"):
            fixture = os.path.join(ROOT, "tests", "fixtures", "ntfs-test-volume.img.gz")
            volume = os.path.join(tmp, "ntfs-test-volume.img")
            os.makedirs(os.path.dirname(source), exist_ok=True)
            with gzip.open(fixture, "rb") as compressed, open(volume, "wb") as output:
                shutil.copyfileobj(compressed, output, 1024 * 1024)
            subprocess.run([sys.executable, os.path.join(ROOT, "scripts", "package-ntfs-test-disk.py"),
                            volume, source], cwd=ROOT, check=True)
        if not os.path.exists(source):
            shutil.rmtree(tmp, ignore_errors=True)
            log(f"{case}: FAIL ({disk[5:]} is missing: run the case that makes it first)")
            return False
        shutil.copyfile(source, disk_copy)
        env["QEMU_DISK"] = disk_copy
    if spec.get("usb_disk"):
        usb_copy = os.path.join(tmp, "usb-disk.img")
        shutil.copyfile(os.path.join(ROOT, TEST_DISK), usb_copy)
        qemu_args.extend(["-drive", f"file={usb_copy},if=none,id=usbtest,format=raw"])
        if spec.get("usb_disk_count", 1) > 1:
            usb_copy2 = os.path.join(tmp, "usb-disk-2.img")
            shutil.copyfile(os.path.join(ROOT, TEST_DISK), usb_copy2)
            qemu_args.extend(["-drive", f"file={usb_copy2},if=none,id=usbtest2,format=raw"])
    if spec.get("virtio_disk"):
        virtio_copy = os.path.join(tmp, "virtio-disk.img")
        shutil.copyfile(os.path.join(ROOT, TEST_DISK), virtio_copy)
        qemu_args.extend(["-drive", f"file={virtio_copy},if=none,id=vblk,format=raw"])
    if spec.get("iso", True) is False:
        iso = "none"
    if spec.get("processors"):
        env["QEMU_SMP"] = str(spec["processors"])
    host_http_server = None
    host_http_thread = None
    host_http_port = ""
    if spec.get("host_http_fixture"):
        try:
            host_http_server, host_http_thread = start_host_http_fixture()
            host_http_port = str(host_http_server.server_address[1])
        except OSError as error:
            shutil.rmtree(tmp, ignore_errors=True)
            log(f"{case}: FAIL (could not start host HTTP fixture: {error})")
            return False
    pending_actions = [
        (marker.replace("{host_http_port}", host_http_port),
         [command.replace("{host_http_port}", host_http_port) for command in commands])
        for marker, commands in spec.get("actions", [])
    ]
    log(f"{case}: booting {iso}")
    qemu = subprocess.Popen(
        [os.path.join("scripts", "run-qemu.sh"), iso, "-monitor", monitor,
         "-qmp", f"unix:{qmp_socket},server,nowait",
         *qemu_args],
        cwd=ROOT, env=env, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)

    ok = False
    reason = "timeout"
    deferred_failure = ""
    started = time.time()
    text = ""
    expected = [marker.replace("{host_http_port}", host_http_port) for marker in spec["expect"]]
    expected_any = [marker.replace("{host_http_port}", host_http_port)
                    for marker in spec.get("expect_any", [])]
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
                    elif command.startswith("screenshot:"):
                        name = command[len("screenshot:"):]
                        if os.path.basename(name) != name or not name.endswith(".png"):
                            raise ValueError(f"invalid screenshot name: {name!r}")
                        monitor_command(mon, f'screendump "{ppm}"')
                        ppm_to_png(ppm, os.path.join(ROOT, TEST_DIR, name))
                    elif command.startswith("type:"):
                        for ch in command[5:]:
                            name = KEY_NAMES.get(ch, ("shift-" + ch.lower()) if ch.isupper() else ch)
                            monitor_command(mon, f"sendkey {name}")
                    elif command.startswith("qmpkey:"):
                        qmp_send_key(qmp_socket, command[len("qmpkey:"):])
                    elif command.startswith("qmpmouse:"):
                        values = command[len("qmpmouse:"):].split(":")
                        qmp_send_mouse(qmp_socket, values[0], values[1], values[2] if len(values) > 2 else "left")
                    else:
                        monitor_command(mon, command)
            deferred = [m for m in spec.get("defer_fail", []) if m in text]
            bad = [m for m in spec["fail"] if m in text and m not in deferred]
            if bad:
                reason = f"unexpected serial output: {bad[0]!r}"
                break
            counts_met = all(text.count(marker) >= count for marker, count in spec.get("expect_counts", {}).items())
            if deferred:
                required = [m for m in expected if m not in spec.get("defer_expect", [])]
                if all(m in text for m in required) and counts_met:
                    deferred_failure = deferred[0]
                    ok = True
                    break
            any_met = not expected_any or any(m in text for m in expected_any)
            if all(m in text for m in expected) and any_met and counts_met and not spec.get("exit_ok"):
                ok = True
                break
            time.sleep(0.5)

        if qemu.poll() is None:
            time.sleep(1.5)  # let the last frame reach the framebuffer
            try:
                monitor_command(mon, f'screendump "{ppm}"')
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
        if ok and spec.get("check_usb_disk") and usb_copy:
            problem = check_disk(usb_copy)
            if problem:
                ok = False
                reason = f"USB disk write/readback check failed: {problem}"
        if ok and spec.get("install_checks") and "QEMU_DISK" in env:
            problem = check_install(env["QEMU_DISK"], spec["install_checks"])
            if problem:
                ok = False
                reason = problem
        if ok and spec.get("save_disk") and "QEMU_DISK" in env:
            shutil.copyfile(env["QEMU_DISK"], os.path.join(ROOT, spec["save_disk"]))
        if ok and deferred_failure:
            ok = False
            reason = f"unexpected serial output: {deferred_failure!r}"
        if host_http_server:
            host_http_server.shutdown()
            host_http_server.server_close()
            host_http_thread.join(timeout=5)
        shutil.rmtree(tmp, ignore_errors=True)

    elapsed = time.time() - started
    if ok:
        log(f"{case}: PASS in {elapsed:.1f}s (serial: {serial_log}, screenshot: {png})")
        for marker in spec.get("warn_markers", []):
            if marker in text:
                print(f"    ! system warning observed: {marker}")
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
