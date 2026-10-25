# Nexora OS implementation checklist and roadmap

## Working ledger — written, unverified (resume here if history is lost)

**Standing rule (user, 2026-10-25):** do not run builds, `make iso`, or VM tests — they consume too
many tokens. Because of this rule, **no checklist gate anywhere in this file is marked passed**.
Every item recorded below as `written` means the code exists on disk but has **never been compiled
or run**. Do not read a `written` entry as "works".

### Resume in five lines

1. Read this ledger, then `plan.md`, then the phased checklist and OS-0…OS-7 sections below.
2. The one outstanding action that releases everything: **one build + QEMU/VirtualBox pass** (§
   "Verification still owed"). Until that runs, nothing may be ticked.
3. New code goes in the files named in the table below; match the surrounding style (kernel C17
   `-Werror`, no FP/SIMD in kernel, freestanding C++20 user mode, `hzui` toolkit).
4. Keep the ledger table and "Next up" current after every edit — that is the point of this section.
5. Never claim a capability works because code exists; only a passing test does that.

### Handoff prompt — paste this to start the next session

If the chat history is lost, paste the block below as the first message. It is self-contained.

```text
You are continuing work on "Nexora OS Prototype" in e:\Nexora OS Prototype\Nexora OS Prototype
(an original x86-64 UEFI desktop OS: C17 kernel, freestanding C++20 user mode, hzui toolkit).

FIRST, before anything else: read checklist.md top to bottom — its "Working ledger — written,
unverified", "Next up", and "Requested next steps" §7 are the authoritative state — then plan.md's
"Where we are". Do not re-derive the plan; it is already written down.

HARD CONSTRAINTS (from the user, still in force):
1. NO builds, NO `make iso`, NO ISO generation, NO VM runs. The user is credit-sensitive. Every
   item is therefore "written, unverified" and NO checklist box may be ticked. Verify by reading
   code, never by compiling.
2. After EVERY file edit, immediately update checklist.md (the ledger table + a change-log line +
   "Next up") and plan.md's "Where we are". This is what protects progress from history resets.
3. Never fake a capability. In particular Wi-Fi/Bluetooth toggles and remote browsing must not be
   made to look like they work; they are blocked on a named/emulated driver, and on the kernel's
   TCP + TLS work (OS-5). Say so honestly.
4. Work in the existing style and files. Do not restructure the roadmap.

WHERE WE LEFT OFF (2026-10-25, late):
Read the "Written and unverified" table and "Next up" below — those are the state, not this summary.
The bullet list that follows is an earlier snapshot, kept only for the file and function names in it,
so treat the two later paragraphs after it as the current position.
- Item 7.4 (real icons from the internet) — WRITTEN. Material Icons Regular (Apache-2.0) is
  vendored in third_party/icons/material/ (TTF + .codepoints + LICENSE.txt). hzuser gained
  HzIconFont(size) + HzDrawGlyphCentered (user/lib/hzuser/font.cpp, user/include/hzuser.h); hzui
  gained IconCodePoint() + Painter::DrawIconGlyph() (user/lib/hzui/painter.cpp, user/include/hzui.h)
  called at the top of Painter::DrawIcon, with the 47 built-in vector paths kept as the fallback
  when the font file is missing. Makefile: UI_FONTS + LICENSE_FILES + a ramdisk copy to
  Nexora/Licenses/MaterialIcons-Apache-2.0.txt. THIRD_PARTY_NOTICES.md updated.
- Item 7.1 (real File Explorer) — COMPLETE IN CODE, unverified. (a) user/include/hzarchive.h +
  user/lib/hzrt/hzarchive.cpp: a self-contained DEFLATE (RFC 1951) + CRC-32 + ZIP (stored/deflated)
  + tar/ustar (GNU long names) + gzip engine with Detect/FormatName/IsArchive/StripSuffix/List/
  Extract; SafeRelative refuses entry names that would escape the destination; every stream is
  CRC-checked; Zip64/encrypted/bzip2 return STATUS_NOT_SUPPORTED. It is listed in
  USER_RUNTIME_OBJS in the Makefile. (b) user/apps/files/files.cpp: includes hzarchive.h, has
  ExtractSelected() and an "Extract all..." item in ShowItemContextMenu (archives on writable
  volumes only) unpacking to "<name>\" via hz::archive::Extract, logging counts and showing a
  STATUS error otherwise; IconFor/IconColorFor/TypeName know archives and images. (c) hzui gained
  three font-only icons: Icon::Archive (0xEB2C folder_zip), Icon::Image (0xE3F4), Icon::Play
  (0xE038). (d) Layout: the sidebar is now a Windows 11 Quick access list (struct Place,
  PlaceItems_, FilesApp::RebuildPlaces) — Home, Desktop and Documents each only when the folder
  really exists, then one row per volume — and Navigate() highlights the row for the folder on
  screen (exact match, else the volume); the unused Volumes_ member was removed.
- Earlier, also written and unverified: Wave 1 Explorer + Trash; Wave 2 desktop icons/theme/
  Settings; the OS-1 first-boot wizard in user/apps/setup/setup.cpp (9 pages, per-page resume, writes
  Apps/AppsConfigured/Shortcut* keys — the shell does NOT read those keys yet); the Windows 11 boot
  screen (init/inbv.c, drivers/bootvid/bootvid.c, init/init.c); the installer's named stages / verify
  stage / completion page; terminal Linux+Windows command parity (user/apps/terminal/); the user
  profile picture (wallpaper.cpp + Settings > Accounts); and six polished apps (calc, calendar,
  clock, stopwatch, sysinfo, taskmgr) using hzui::Transition + Element::Animate().

WRITTEN SINCE THIS BLOCK (still unverified): 7.5 Control Center — it works and moves now (an
Appearance card whose three switches really change the light/dark scheme, the top dock and the
desktop icons, each persisted to shell.ini; the flyout slides its content into place on open). 7.3
Start / app drawer — one AppSection owns the heading, the tile grid and an All apps/Pinned switch, so
Start is browsable; the menu slides in on open. 7.2 Installer — Setup now measures what it will copy
and refuses a disk it cannot fill with STATUS_DISK_FULL *before* the erase, and the Ready page names
the sizes. 7.6 Layout — the ramdisk recipe now creates the full C:\Nexora set
(System/Fonts/Apps/Wallpapers/Licenses/Settings) and the orphan rootfs/Nexora/welcome.txt is gone.
7.7 Photo Viewer — **both halves written** (item 7.7): the vendored stb_image decoder behind
user/include/hzimage.h + user/lib/hzrt/hzimage.cpp (third_party/stb/compat/ supplies the stdlib.h and
string.h the freestanding build lacks), and the viewer user/apps/photos/photos.cpp +
rootfs/Nexora/Apps/photos.app. **Video is deliberately absent and must stay unclaimed** — no demuxer,
no codec, no audio device. 7.11 Wi-Fi + browsing — **started, far from done**: net/netcore.c (the
NetTransmitFrame that net/arp.c already called and nothing defined — the kernel net layer could not
have linked without it), net/ipv4.c (send/routing/receive + a real ICMP echo reply), net/udp.c
(bounded bindings/queues, checksum-verified NetUdpDeliver, now reached from NetIpReceive), and
include/net80211.h (the cfg80211-shaped contract only). Still missing: TCP, sockets + user API, HTTP,
TLS, a renderer, net/net80211.c, its supplicant, and every card driver.
See the ledger rows above.

2026-10-25 (late): the user then asked for a further batch, recorded as §7 items 14–19 plus a
consolidated "what a proper OS still needs" list — see §7 for the full text and the honesty gates.
In short: reuse other projects' drivers (porting yes, loading no; GPL-2.0 gate; the real blocker is
the missing Delivery-stage 2 PCI/DMA/MSI framework), run Windows .exe and Linux ELF binaries (three
separate projects — no single step delivers it), the missing apps (App Store, Media Player, Disk
Manager…) where an App Store needs a package + trust story and a Media Player needs an audio driver
and codec, more File Explorer features, dock hover/motion on both docks, **volume controls (blocked
on an audio driver — do not fake it)**, and removing blockyard plus the beta apps. Suggested order
is at the top of "Next up".

NOT WRITTEN YET — this is the work queue, in this order:
- 7.7 Photo Viewer — **do not redo it; both halves are written** (see above). The only thing owed is
  the honest video-player answer: no demuxer, no codec, no audio device, so do not claim "all
  formats" and do not draw a player that plays nothing.
- 7.8 Window snapping + multitasking like Windows with motion (snap layouts, drag-to-edge,
  halves/quarters, shortcuts, snap-assist overlay, Alt+Tab / task view).
- 7.9 Re-state "how the OS behaves, including installations".
- 7.10 Do all the skipped things: Music Player, Video Player, Screenshot Tool, Email, Contacts,
  Archive Manager, App Store, PDF Viewer (Photos is now written), plus OS-2…OS-7 and Delivery
  stages 2–9.
- 7.11 Wi-Fi + browsing — **started, far from done**: net/netcore.c, net/ipv4.c, net/udp.c and
  include/net80211.h are written. Next browsing step is moving net/dns.c onto net/udp.c (it still
  hand-rolls ARP/IPv4/UDP frames and polls the ring), then net/tcp.c → net/socket.c + a bounded user
  API → HTTP → TLS → renderer. On the wireless side net/net80211.c, its WPA2/WPA3 supplicant and
  every card driver are not written, and the PCI ID to pin is still undecided.
- 7.12 Re-polish every component (including the shell's manifest-icon table, which still maps the
  icon name "Image" to Icon::Palette and has no Archive/Play entries, so photos.app draws the wrong
  tile in Start and on the desktop).
- 7.13 Remove the beta apps: USER_PROGRAMS := hello usertest crash fstest wintest uitest apptest
  (Makefile line ~140), the welcome app/manifest, and the placeholder brochure page in
  user/apps/browser/browser.cpp. Also make the shell read Apps/AppsConfigured/Shortcut* (OS-1).
- 7.14–7.19 The 2026-10-25 (late) batch — read §7 items 14–19 in full before starting any of it.
  Item 19 (remove blockyard + the beta apps) is the smallest and safest; then item 17 (Explorer),
  item 18 (dock hover/motion; volume stays honestly unavailable), item 16's apps that need no new
  subsystem, then the driver framework, then the bigger projects (audio, App Store, compatibility
  layer). Everything there is unstarted.

USEFUL FACTS SO YOU DO NOT HAVE TO RE-DISCOVER THEM:
- Build: `make iso` is the default goal; user programs link hzrt (crt/heap/hzcore/hzfile/hzapps/
  hzcrypto/hzarchive) + hzsys + hzuser + hzui. USER_RUNTIME_OBJS is the list to extend for a new
  library .cpp. -Werror -Wall -Wextra; kernel forbids FP/SIMD; user mode is freestanding
  (-fno-exceptions -fno-rtti -fno-threadsafe-statics), PE32+, clang + lld-link.
- There is no libm: hzmath.h has HzFabs/Floor/Ceil/Sqrt/Exp/Log/Pow/Sin/Cos/Atan/Acos and HZ_PI.
- hz::fs (hzfile.h): Resolve/Exists/List/Join/Parent/FileName/Extension/EqualNames/ReadAll/
  WriteAll/Copy/CopyTree/MoveTree/Delete/DeleteTree/CreateDirectory/Rename/Stat/QueryVolume/
  Volumes/FormatSize/FormatTime/DocumentsFolder/Settings(Load/Save/Get/GetInt/Set/SetInt)/
  SettingsPath/IsRoot/IsLiveSystem/StatusText/Trash* and TrashFolder().
- hz::archive (hzarchive.h): Detect, FormatName, IsArchive, StripSuffix, List, Extract(path, dest,
  Result*{Files,Folders,Bytes}).
- hzui: Transition {To(target, ms), Jump, Value, Target, Running} + Motion:: constants + virtual
  bool Element::Animate() at ~15 ms; Painter has Clear/FillRect/DrawBitmap/FillRoundRect/
  StrokeRoundRect/FillVerticalGradient/FillCircle/StrokeCircle/Line/Polyline/Text/TextLine/DrawIcon/
  PushClip/PopClip/FocusRing. Icon is enum class Icon : uint8_t (now 50 values).
- Fonts: hzuser/font.cpp rasterizes TTF at runtime with stb_truetype, loaded through
  hz::fs::SharedFile, so every font the OS reads must be reachable at C:\Nexora\Fonts\ (Makefile
  UI_FONTS copies them; the icon font is MaterialIcons-Regular.ttf).
- Status codes: HZ_SUCCESS(s) ((STATUS)(s) >= 0); STATUS_SUCCESS, STATUS_NOT_SUPPORTED
  (0xC00000BB), STATUS_FILE_TOO_LARGE, STATUS_DISK_FULL, STATUS_OBJECT_NAME_COLLISION, etc. in
  include/hzbase.h, reachable through hzsys.h -> uapi/hzapi.h.
- App manifests: rootfs/Nexora/Apps/*.app, "Key=Value" lines; required keys name and path, where
  path must match ^C:\Nexora\System\([A-Za-z0-9_-]+)\.exe$ (validate-app-catalog.py enforces it).
- Real state of the OS today: the kernel net layer has working-by-construction packet parsing, ARP
  and DHCP, and now IPv4 send/receive (with a real ICMP echo reply) and UDP. include/net.h also
  *declares* TCP and sockets, but declarations are not code — net/tcp.c and net/socket.c do not
  exist, so nothing above UDP works. There is NO user-mode socket API, NO HTTP client, NO TLS, NO
  renderer, and NO 802.11/Bluetooth/audio driver. The browser is local-only and says so, and the
  Control Center keeps Network/Bluetooth/Volume/Brightness labelled unavailable. Do not pretend
  otherwise anywhere.
- Auto-discovery (so you rarely edit the Makefile for new files): kernel sources come from
  KERNEL_SRC_DIRS (which includes net, drivers/net, fs/…, ps, mm, …) via a wildcard, so any new
  net/*.c is picked up with no Makefile change; user apps come from USER_APP_DIRS
  (user/shell plus a wildcard over user/apps/*/*.cpp), so a new user/apps/<name>/<name>.cpp is
  discovered automatically — but it MUST get a matching rootfs/Nexora/Apps/<name>.app whose
  path is C:\Nexora\System\<name>.exe, or scripts/validate-app-catalog.py fails the build.
  Extending a *library* .cpp (hzrt/hzui/…) does require adding it to USER_RUNTIME_OBJS.
- Freestanding user mode uses -nostdinc, so only clang's resource headers exist: there is no
  <stdlib.h>, <string.h>, <math.h> or <stdio.h>. third_party/stb/compat/ supplies the first two for
  stb_image; use hzmath.h (HzFabs/Floor/Ceil/Sqrt/Exp/Log/Pow/Sin/Cos/Atan/Acos, HZ_PI) instead of
  math.h. A new third-party single-header that includes those headers needs its own compat shim.
- Cheap pre-verification without a build: the editor's language server can report diagnostics per
  file (a GetDiagnostics call on a file path). It catches syntax and type errors and costs nothing —
  use it after writing, but it is NOT a build and never justifies ticking a box.
- Kernel C facts worth not re-deriving: -Werror -Wall -Wextra, no FP/SIMD in the kernel; helpers are
  in include/ke.h (KeInitializeSpinLock, KeAcquireSpinLock(&Lock,&OldIrql), KeReleaseSpinLock,
  KeDelayExecutionThread, KeQueryUptimeMicroseconds, KeInitializeEvent/KeResetEvent/KeSetEvent/
  KeWaitForSingleObject), include/ps.h (PsCreateSystemThread(Routine, Context, 1, "Name", &Thread)),
  include/rtl.h (memset/memcpy/DbgPrint/_stricmp), and include/hzbase.h for STATUS_* codes
  (STATUS_SUCCESS, STATUS_UNSUCCESSFUL, STATUS_NOT_SUPPORTED, STATUS_BUFFER_TOO_SMALL,
  STATUS_INVALID_PARAMETER, STATUS_OBJECT_NAME_COLLISION, STATUS_INSUFFICIENT_RESOURCES,
  STATUS_DEVICE_NOT_READY, STATUS_DEVICE_BUSY, STATUS_DEVICE_DATA_ERROR, STATUS_IO_TIMEOUT,
  STATUS_NO_MORE_ENTRIES, STATUS_DISK_FULL …). HZ_SUCCESS(s) is ((STATUS)(s) >= 0).
- net/ layout as of now: netif.c (interface registry), packet.c/h (header parsing + checksums),
  arp.c, dhcp.c (the only ring poller at boot — NetpWaitDhcpReply reads ReceiveFrame directly),
  dns.c (still hand-rolls its own ARP/IPv4/UDP frames and polls the ring), netcore.c
  (NetTransmitFrame/NetProcessFrame/NetReceiveWorker/NetInitializeStack/NetStackTick), ipv4.c, udp.c.
  Because DHCP and DNS poll the receive ring themselves, NetReceiveWorker is deliberately NOT started
  anywhere; whoever starts it must first move DHCP/DNS off the ring, or two readers will steal each
  other's frames (this is written into net/netcore.c and net/udp.c).

NEXT STEP: item 19 (remove blockyard + the beta apps) unless the user names a different item — it is
the smallest verifiable unit of the newest batch. Then follow the order at the top of "Next up".
For every edit: change the code, then update the checklist.md ledger row + change log + "Next up",
and plan.md's "Where we are". Report what you did, note that it is uncompiled and unverified, and
keep going through the queue. Never tick a box: only the owed build + VM pass can do that.
```


### Written and unverified

| Area | Files | State |
|---|---|---|
| Wave 1 — File Explorer + Trash | `user/apps/files/files.cpp`, `user/include/hzfile.h`, `user/lib/hzrt/hzfile.cpp` | written, unverified |
| Wave 2 — desktop icons, icons, theme, Settings | `user/shell/desktop.cpp`, `user/shell/desktop.h`, `user/apps/settings/settings.cpp` | written, unverified |
| Wave 2 — first-boot wizard (base 5 pages) | `user/apps/setup/setup.cpp` (`FirstBootWizard`) | written, unverified |
| OS-1 — first-boot flow: "Set up your device" landing + About + accessibility text, region page, app-selection page (locked shell/Settings; 14 preselected, 6 optional), Connect page with Skip, review page, per-page resume via `SetupPage`, shortcut + app keys written on finish | `user/apps/setup/setup.cpp` (`FirstBootWizard`, `Wizard::StartPage_`) | written, unverified; **the shell does not read the new `Apps`, `AppsConfigured`, `ShortcutFiles`, `ShortcutTerminal`, `ShortcutBrowser`, `ShortcutTrash` keys yet**, so choosing apps does not change Start or the desktop yet |
| Wave 3 — Windows 11 boot screen, 60 fps spinner | `init/inbv.c`, `drivers/bootvid/bootvid.c`, `include/bootvid.h`, `init/init.c` | written, unverified |
| Installer — named stages, verification stage, completion page, locale row, write lock | `user/apps/setup/setup.cpp` (`Install`, `VerifyInstall`) | written, unverified; the verification stage's progress band was fixed to stay monotonic (`kStageBand[5]`, not `[4]`) |
| Terminal — Linux + Windows command parity | `user/apps/terminal/parse.h`, `parse.cpp`, `terminal.cpp` | written, unverified; see "Requested next steps" item 1 |
| User profile picture | `user/lib/hzui/wallpaper.cpp`, `user/include/hzwallpaper.h`, `user/apps/settings/settings.cpp`, `user/shell/shell.h`, `user/shell/shell.cpp`, `user/shell/flyouts.cpp` | written, unverified; `shell.ini` key `ProfilePicture`; 24-bit `.bmp` only |
| App polish pass (motion + UI) | `user/apps/calc/calc.cpp`, `calendar/calendar.cpp`, `clock/clock.cpp`, `stopwatch/stopwatch.cpp`, `sysinfo/sysinfo.cpp`, `taskmgr/taskmgr.cpp` | written, unverified; all six use `hzui::Transition` + `Element::Animate()` (taskmgr re-confirmed present at `taskmgr.cpp:82,119,145,202,221,344,370,416`) |
| Real icons — Material Icons font (item 7.4) | `third_party/icons/material/` (TTF 356,840 B + `.codepoints` + Apache-2.0 `LICENSE.txt`), `user/include/hzuser.h`, `user/lib/hzuser/font.cpp` (`HzIconFont`, `HzDrawGlyphCentered`), `user/include/hzui.h`, `user/lib/hzui/painter.cpp` (`IconCodePoint`, `Painter::DrawIconGlyph`), `Makefile` (`UI_FONTS`, `LICENSE_FILES`, ramdisk license), `THIRD_PARTY_NOTICES.md` | written, unverified; `DrawIcon` now tries the font first and falls back to the 47 vector paths when the file is missing, so nothing can break if the font is absent |
| Archives — engine (item 7.1, part 1) | `user/include/hzarchive.h`, `user/lib/hzrt/hzarchive.cpp`, `Makefile` (`USER_RUNTIME_OBJS`) | written, unverified; self-contained DEFLATE (RFC 1951) + CRC-32 + ZIP/tar(ustar, GNU long names)/gzip; `SafeRelative` refuses entries that would escape the destination, ZIP entries are CRC-checked; Zip64/encrypted/bzip2 are reported as unsupported. Now in `USER_RUNTIME_OBJS`, so it links into every program |
| Explorer — archive extract + archive/image icons, types (item 7.1, part 2) | `user/apps/files/files.cpp` (`ExtractSelected`, `ShowItemContextMenu`, `IconFor`, `IconColorFor`, `TypeName`), `user/include/hzui.h` (`Icon::Archive/Image/Play`), `user/lib/hzui/painter.cpp` (`IconCodePoint` for the three new icons: folder_zip `0xEB2C`, image `0xE3F4`, play_circle_filled `0xE038`) | written, unverified; an archive's context menu gains "Extract all..." (writable volumes only) which unpacks to `<name>\`, reports the count/size in the log and shows a `STATUS` error otherwise. `.zip` shows as "Compressed folder", `.tar/.gz/.tgz` as "Archive", images as "Image" |
| Explorer — Quick access sidebar layout (item 7.1, part 3) | `user/apps/files/files.cpp` (`struct Place`, `PlaceItems_`, `RebuildPlaces`, `Build`'s sidebar, `Navigate`'s row highlight; the old `Volumes_` member is gone) | written, unverified; the sidebar is now Quick access (Home, Desktop, Documents - each only when the folder really exists) above the volumes, and the row matching the folder being shown is highlighted (exact match, else the volume). Files no longer lists volumes with no side information |
| Control Center — real switches + reveal motion (item 7.5) | `user/shell/flyouts.cpp` (`RevealHost`, `kRevealSlide`, `ControlToggleRow`, `CreateQuickSettings`), `user/shell/shell.h`, `user/shell/shell.cpp` (`ReloadChrome`, `SetLightMode`, `SetTopDock`, `SetDesktopIcons`, `SaveSettings`), `user/include/hzui.h` (`Motion::FlyoutRevealMs`, `Icon::Bluetooth`), `user/lib/hzui/painter.cpp` (`IconCodePoint`, bluetooth `0xE1A7`) | written, unverified; the Appearance card holds three switches that really work — light/dark, the top dock, the desktop icons — each applied to the shell at once and saved to `shell.ini` (`Theme`/`TopDock`/`DesktopIcons`, now *written* by `SaveSettings`, which before only `LoadSettings` read). The flyout opens by sliding its content up 16 px inside the window's clip (`RevealHost`, `Motion::FlyoutRevealMs` = 220 ms); hzui has no opacity layer, so position is the only motion available. Bluetooth joins the muted rows, and Volume/Brightness/Focus stay labelled unavailable — no 802.11/Bluetooth driver and no audio mixer exist. **No box ticked; nothing compiled.** |
| Start / app drawer re-polish (item 7.3) | `user/shell/flyouts.cpp` (`AppSection`, `CreateStartMenu`, `RecentRow`) | written, unverified; one `AppSection` (a `Grid` subclass) now owns the heading, the **All apps / Pinned** switch and the tile grid, so Start is a browsable app drawer and not pins only. The mode is element state, so it is freed with the window and both the search box and the button can reach it. Searching filters to matches and the heading reads "Results" (unchanged behaviour); Return opens the best match. The menu slides in on open (`RevealHost`) and its size is clamped to the work area. `RecentRow` now fades its hover like `AppTile` did (it painted instantly before). **No box ticked; nothing compiled.** |
| Installer — pre-flight space check (item 7.2) | `user/apps/setup/setup.cpp` (`MeasurePayload`, `Install`'s guard, `InstallWizard::PageShown(2)`, `UpdateButtons`, `SpaceFits_`) | written, unverified; `MeasurePayload` sizes what Setup writes where (the install image plus the live boot image for the EFI system partition, `C:\Nexora` for the system partition). `Install` now measures **before the first sector is written** and returns `STATUS_DISK_FULL` with a named `Job.Error` if either partition cannot hold it, instead of erasing a disk it cannot fill; the earlier post-format measurement was replaced by that one, so `Job.TotalBytes` (and the progress percentages) are unchanged. The **Ready to install** page shows the copied sizes next to the partition sizes in its layout paragraph (no new element: the page already fills the 620 px window) and turns accent-coloured, with Setup refusing to continue, on a disk that is too small. **Bootability itself is unchanged and remains a verification item** — the ISO is El-Torito UEFI-bootable by construction (`Makefile` `--efi-boot boot/limine/limine-uefi-cd.bin`, `boot/limine.conf`'s `/Install Nexora OS` with `hz.installer=1`) and `install.tar` carries `EFI/BOOT/BOOTX64.EFI`, so the installed ESP gets a boot loader. **No box ticked; nothing compiled.** |
| Clean system layout — the six `C:\Nexora` folders (item 7.6) | `Makefile` (the `$(RAMDISK)` recipe + its new comment), `rootfs/Nexora/welcome.txt` (deleted) | written, unverified; the ramdisk recipe now creates `Nexora/{System,Fonts,Apps,Wallpapers,Licenses,Settings}` — `Settings` was the real gap, because `hz::fs::SettingsPath("shell")` is `C:\Nexora\Settings\shell.ini` and `Settings::Save` does not create its parent, so the media carried a settings folder the OS had to write into but did not ship. `Apps` is now created explicitly too (it used to depend on `rootfs` happening to contain it). A comment states the layout once and why `C:\Users` belongs to an installation and not to the media. Nothing in the installer changes: `Install` already tolerates `Settings` arriving with the copied tree (its `CreateDirectory` collision branch). The stray `rootfs/Nexora/welcome.txt` — an orphan note no code reads — is gone. **No box ticked; nothing compiled.** |

| Photo Viewer — picture decoder (item 7.7, part 1) | `third_party/stb/stb_image.h` (283,010 B), `third_party/stb/compat/stdlib.h`, `third_party/stb/compat/string.h`, `third_party/stb/LICENSE.txt`, `user/include/hzimage.h`, `user/lib/hzrt/hzimage.cpp`, `Makefile` (`-Ithird_party/stb/compat`, `USER_RUNTIME_OBJS`, `LICENSE_FILES`, ramdisk license) | written, unverified; the vendored stb_image is compiled in `hzimage.cpp` (not a library) with `STBI_NO_STDIO/THREAD_LOCALS/HDR/LINEAR/PSD/PIC` and the `STBI_MALLOC/REALLOC/FREE/ASSERT` hooks pointed at the Nexora heap and the process globals, so no libm and no TLS directory are needed. Freestanding user mode has no `<stdlib.h>`/`<string.h>`, so `third_party/stb/compat/` supplies the declarations the header includes. `hz::image` reads 32 bytes to recognise the format by signature (extension only as a fallback), decodes through `stbi_load_from_memory(…, 4)` to **`0xAARRGGBB`** pixels (stb hands back RGBA quads, so each quad is repacked), and reports PSD/HDR as *recognised but not decoded* (`IsSupported` false) rather than half-decoding them. `RotateQuarter` turns a copy pixel by pixel because `Painter::DrawBitmap` has no rotation. `LastError` carries stb's own reason. **Video is deliberately absent** — a container demuxer plus an open codec, and for sound an audio device, none of which exist. **No box ticked; nothing compiled.** |
| Photo Viewer — the viewer app (item 7.7, part 2) | `user/apps/photos/photos.cpp`, `rootfs/Nexora/Apps/photos.app` | written, unverified; auto-discovered by `USER_APP_DIRS` (`wildcard user/apps/*/*.cpp`), and the manifest's `Path=C:\Nexora\System\photos.exe` satisfies `scripts/validate-app-catalog.py` in both directions (the binary name is in the built list *and* matches the source directory). `ImageStage` (a custom `Element`) draws the picture at the fit or chosen zoom, drag-pans with the left button, zooms on the wheel, centres with a pill zoom badge, and reveals through a `Transition` 0.94→1.0 over 180 ms (`hzui` has no opacity layer, so the motion is scale, never a fade). A 3-row `Grid` holds a File/View `MenuBar` + nine-button toolbar, the stage, and a status bar reading "name | WxH | FORMAT [| i of n]". Left/Right (or Space/PageDown) step through the folder's pictures, Ctrl+O/W/R/0/1 and Ctrl+Shift+R are wired, and a file that will not decode **names its format and the reason in the stage** instead of drawing blank. **No box ticked; nothing compiled.** |
| Browsing — net core + IPv4 (item 7.11, part 1) | `net/netcore.c`, `net/ipv4.c` | written, unverified; `net/netcore.c` is the glue `include/net.h` always declared and nothing defined — **`net/arp.c` already called `NetTransmitFrame`, so without it the kernel net layer could not link**. It defines `NetTransmitFrame` (retries up to 20 times over `STATUS_DEVICE_BUSY` with a 50 µs backoff), `NetProcessFrame` (Ethernet → ARP/raw-IPv4 dispatch), `NetReceiveWorker` (drains every interface into a `NETIF_MAX_FRAME_SIZE` buffer — **deliberately not started yet**), `NetInitializeStack` (resets ARP, and — since part 1b — UDP too, because the TCP/socket initialisers do not exist and calling them would not link) and `NetStackTick` (`NetArpTick`). `net/ipv4.c` is the first real layer of the browsing plan: `NetAddressInSubnet`, `NetResolveNextHop` (broadcast → `0xFF…`, in-subnet → destination, else the gateway), `NetFindConfiguredInterface`, `NetIpSend` (TTL 64, version/IHL `0x45`, incrementing identification via `__atomic_add_fetch`, IPv4 checksum through `NetIpv4Checksum`, **DF set and no fragmentation**, so an over-MTU datagram returns `STATUS_BUFFER_TOO_SMALL`), `NetIpResolveAndSend` (`NetResolveNextHop` → `NetArpResolve` → send) and `NetIpReceive` with a real ICMP-echo reply (`NetpReplyEcho`, checksum recomputed) so the machine is pingable; **UDP now reaches `NetUdpDeliver` (part 1b)** and TCP is still dropped with `STATUS_NOT_SUPPORTED` **on purpose** (calling the unwritten `NetTcpDeliver` would not link). The receive worker stays unstarted because `net/dhcp.c`'s `NetpWaitDhcpReply` polls the same `ReceiveFrame` ring — two readers of one ring would steal each other's frames. **No box ticked; nothing compiled.** |
| Wi-Fi — shared 802.11 layer contract (item 7.11, part 2) | `include/net80211.h` | written, unverified; **contract only** — `net/net80211.c` (the shared layer and its WPA2-PSK/WPA3 supplicant) and any card driver under `net/80211/` are **not written**, so no wireless driver exists. The header is deliberately shaped like Linux's `cfg80211`: `NET80211_DEVICE` carries the driver ops (`Start`, `Stop`, `SetChannel`, `Scan`, `Join`, `Leave`, `SendEapol`, `SendData`, `QueryLink`) plus an embedded **`NET_INTERFACE`**, so everything above (ARP/IPv4/DHCP) is unchanged whether the link is wired or wireless; `NET80211_BSS` / `NET80211_SECURITY` / `NET80211_STATE` and the shared entry points (`Net80211RegisterDevice`, `Scan`, `Connect`, `Disconnect`, `ReceiveEapol`, `Tick`) are declared. The file writes the ownership split down: the driver owns firmware/DMA/channel/raw frame TX-RX, the shared layer owns scanning, BSS bookkeeping, association and the supplicant (4-way handshake, PBKDF2, CCMP/AES) — "so the hard security code exists once". Supporting many cards then means adding a driver, not rewriting the wireless logic. **The pinned PCI ID is still unresolved**, so the first driver cannot be chosen. **No box ticked; nothing compiled.** |

| Browsing — UDP (item 7.11, part 1b) | `net/udp.c` (new), `net/ipv4.c` (UDP dispatch), `net/netcore.c` (`NetInitializeStack` now resets UDP too) | written, unverified; implements the UDP contract `include/net.h` declares but nothing defined. A bounded table of `NET_UDP_MAX_BINDINGS` (8) port bindings, each with a `NET_UDP_QUEUE_DEPTH` (3) queue of `NET_UDP_MAX_DATAGRAM` (1472) byte datagrams behind one spin lock; port 0 picks an ephemeral port in 49152..65535 and a taken port returns `STATUS_OBJECT_NAME_COLLISION`. `NetUdpSend` builds the segment, computes the checksum through `NetUdpChecksum` (a computed zero is transmitted as 0xFFFF per RFC 768), substitutes the interface address when the source is `0.0.0.0` (DHCP's case) and sends via `NetIpResolveAndSend`. `NetUdpDeliver` is the enqueue path — **checksum verified first, so a corrupt datagram is not a datagram** — and drops (with a log line) on an unbound port or a full queue rather than overwriting a queued one; `net/ipv4.c`'s UDP case now parses and calls it. **The receive path is honest about its shape:** nothing else reads the ring for UDP because `NetReceiveWorker` is still not started (DHCP polls the same ring), so `NetUdpReceive` polls the ring itself while its queue is empty and the file states that this polling must be replaced by a pure event wait when the worker starts. A datagram larger than the caller's buffer returns `STATUS_BUFFER_TOO_SMALL` and is dropped, never truncated. **No box ticked; nothing compiled.** |

### Verification still owed

A single build and test pass settles all of the above at once:

- `make iso` with the pinned toolchain (E:/F:), then `make test` on one CPU and `QEMU_SMP=4`.
- Focused QEMU cases in order: `install`, then `diskboot`, `diskboot2`, `diskboot3`.
- One disposable-VDI VirtualBox install/boot.

### Next up (checklist order)

- **Newest user request (2026-10-25, evening) — a large batch, recorded as §7 items 14–19 plus a
  consolidated "what a proper OS still needs" list.** Nothing in it is written yet. It asks for:
  third-party driver reuse (answered: porting yes, loading no, framework is the blocker), running
  `.exe` and Linux ELF binaries (recorded as three separate projects — no single step delivers it),
  the missing apps (App Store, Media Player, Disk Manager, Device Manager, Screenshot Tool, Archive
  Manager, PDF Viewer, Email, Contacts, Software Update, Backup…), more File Explorer features, dock
  hover/motion on both docks, **volume controls (blocked on an audio driver — must not be faked)**,
  and removing `blockyard` plus the beta apps. **Suggested order (smallest verifiable units first):**
  (a) item 19 cleanup — pure deletion, low risk; (b) item 17 Explorer batch; (c) item 18 dock hover +
  motion (volume stays honestly unavailable); (d) the app set in item 16 that needs no new subsystem
  (Archive Manager, Disk Manager, Screenshot Tool, Device Manager-style viewers); (e) Driver
  framework → essential drivers → audio → then volume and Media Player; (f) App Store once packages
  and the network stack exist; (g) the compatibility layer, one delivery stage at a time.
- **Previous user request (2026-10-25, evening) — Wi-Fi + internet browsing.** The user asked how
  to add both and chose to **build both**: a real 802.11 driver and the browsing stack. The plan is
  written into "Requested next steps" **item 11** (browsing in the Delivery-stage 3 layer order —
  `net/ipv4.c` → `net/udp.c` → `net/tcp.c` → `net/socket.c` + user API → HTTP → TLS → renderer — and
  the 802.11 project scoped honestly). **Both tracks have started and are written, unverified:**
  `net/netcore.c` + `net/ipv4.c` (the glue that `net/arp.c` already needed — `NetTransmitFrame`,
  `NetProcessFrame`, `NetReceiveWorker` — plus IPv4 send/routing/receive and a working ICMP echo
  reply), **`net/udp.c`** (bounded port bindings, per-port queues, checksum-verified delivery, and
  `net/ipv4.c`'s UDP case now reaching `NetUdpDeliver`), and `include/net80211.h` (the shared 802.11
  contract each future card driver plugs into).
  **Still blocking: the exact PCI ID of the 802.11 card to pin** (a no-firmware ath9k-class card is
  preferred), because `net/net80211.c` and any driver under `net/80211/` are not written. The
  browsing stack has no hardware dependency and runs over the existing e1000 — **next: move
  `net/dns.c` onto `net/udp.c` (it still hand-rolls ARP/IPv4/UDP frames and polls the ring), then
  `net/tcp.c`.**
- **Previous user request (2026-10-25, evening)** — see "Requested next steps" item 7 below: real icons
  (7.4 **written**), real File Explorer (**7.1 complete in code**: archive engine + "Extract all...",
  archive/image icons and types, Quick access sidebar), **7.5 Control Center written** (three real
  switches — light mode, top dock, desktop icons — plus the slide-in reveal and the `SaveSettings`
  gap closed) and **7.3 Start / app drawer written** (the new `AppSection` owns the heading, the tile
  grid and an "All apps" / "Pinned" switch, so the drawer is browsable). **7.2 is written too** — the
  installer measures what it will copy and refuses a disk it cannot fill *before* erasing it, and the
  Ready page names the sizes; "bootable" itself stays a verification item (the ISO is El-Torito
  UEFI-bootable by construction). The **video** player, window snapping + multitasking and removing
  the mock/beta apps are **not written**; Wi-Fi and remote browsing are **started but far from done**
  (the net core + IPv4 and the 802.11 contract are written — see the "Most recent user request" bullet
  above — but UDP/TCP/sockets/TLS, `net/net80211.c` and every card driver are not, so neither toggle
  may be made to look like it works). **7.6 is written as well** — the ramdisk recipe creates the full
  `C:\Nexora` folder set (`System`, `Fonts`, `Apps`, `Wallpapers`, `Licenses`, and the `Settings`
  folder `SettingsPath()` needs) and the orphan `rootfs/Nexora/welcome.txt` is gone. **7.7 is written
  in both halves** — the `stb_image` decoder (`hz::image`, `third_party/stb/`, the `compat` shims) and
  the viewer app (`user/apps/photos/`, `rootfs/Nexora/Apps/photos.app`), with video honestly absent
  (no demuxer, no codec, no audio device).
  **Next: item 7.8 (window snapping + multitasking), then 7.9 (re-state how the OS behaves).**
- **User-requested (added 2026-10-25)** — see "Requested next steps" below: (1) terminal Linux +
  Windows command parity — **written**; (5) polish every built-in app with motion — **six apps
  written**; (6) user profile picture — **written**; (2) Wi-Fi + Bluetooth + secure browsing,
  (3) browser rebuild, (4) Linux/Windows app support — blocked on work that does not exist.
- OS-1 — remaining: make the shell read `Apps` / `AppsConfigured` / `Shortcut*` so the app drawer,
  Start, desktop shortcuts and app launch obey the first-boot selection.
- Installer Phases 2–3 — remaining wording/lock review (mostly done).
- OS-2 — coherent motion system + reduced-motion; context menus.
- OS-3 — wire Settings/Control Center to real APIs; mark unsupported states.
- OS-4 — App Store + Video Player; catalog truthfulness.
- OS-5 — ARP/IPv4/UDP/DNS/TCP/sockets + TLS + browser navigation.
- OS-6 — diagnostics, backup/rollback, repair, factory reset.
- OS-7 — release integration + docs.
- Delivery stages 2–9 — driver framework, networking+TLS, USB xHCI+HID, Bluetooth, NVMe, ext2,
  HDA audio, VirtIO-GPU.

### Change log of this ledger

- 2026-10-25 — created the ledger; fixed the installer verification progress band; added the OS-1
  first-boot pages (region, apps, connect, review), resume, and the app/shortcut settings keys.
- 2026-10-25 — recorded the new user request below ("Requested next steps"); none of it is written
  yet, and items 2–4 are blocked on work that does not exist.
- 2026-10-25 — terminal Linux + Windows command parity written (`Environment`, `ExpandVariables`,
  `IsSwitch`, aliases, `tree`/`find`/`sort`/`set`/`export`/`unset`/`env`/`printenv`/`where`); user
  profile picture written (`MakeRoundPicture` + Settings > Accounts + Start account row); recorded
  the browser-feature, app-polish and Bluetooth parts of the request. Still no build.
- 2026-10-25 (evening) — recorded the **latest request as item 7** (real File Explorer incl. archive
  extract, bootable/real installer, Start/app-drawer re-polish, real icons, Control Center, clean
  folders, photo/video viewers, window snapping + multitasking, remove mock/beta apps, Wi-Fi and
  browsing). Added the app-polish row to the table; **re-confirmed the taskmgr polish is on disk**
  (its earlier subagent result had been missing). Facts checked, no code written: no archive support
  anywhere; ISO already UEFI-bootable by construction; Material Icons is the fetchable icon set;
  `Pillow` absent so the icon pipeline stays font-based; `stb_image.h` fetched for Photo Viewer.
  **Still no build, no ISO, no VM run.**
- 2026-10-25 (evening) — item 7.4 **written, unverified**: vendored Material Icons into
  `third_party/icons/material/`, added `HzIconFont` + `HzDrawGlyphCentered` (hzuser) and
  `IconCodePoint` + `Painter::DrawIconGlyph` (hzui) with the vector paths kept as fallback, wired the
  `Makefile` and `THIRD_PARTY_NOTICES.md`. **Still no build, no ISO, no VM run.** Next: item 7.1, the
  File Explorer (archive extract, create file, better layout).
- 2026-10-25 (evening) — item 7.1 **archive engine written, unverified** (`user/include/hzarchive.h`,
  `user/lib/hzrt/hzarchive.cpp`, ~600 lines: DEFLATE/CRC-32/ZIP/tar/gzip with traversal rejection)
  and added to `USER_RUNTIME_OBJS` in the `Makefile` so it links. The File Explorer does not call it
  yet. **Still no build, no ISO, no VM run.** Next: wire "Extract all…" into `files.cpp`.
- 2026-10-25 (evening) — item 7.1 **extraction wired, unverified**: `files.cpp` gains
  `ExtractSelected()` + an "Extract all..." item in the file context menu (archives only, writable
  volumes), and `IconFor`/`IconColorFor`/`TypeName` now know archives and images. Three font-only
  icons added (`Icon::Archive`, `Icon::Image`, `Icon::Play`). **Still no build, no ISO, no VM run.**
  Next: item 7.1's second half (Explorer layout), then item 7.5 (Control Center) or 7.3 (Start).
- 2026-10-25 (evening) — item 7.1 **finished, unverified**: the Explorer sidebar is a Windows 11
  Quick access list (`struct Place`, `PlaceItems_`, `RebuildPlaces`) - Home, Desktop and Documents
  when those folders exist, then the volumes - and the row for the folder on screen is highlighted.
  The unused `Volumes_` member was removed. **Item 7.1 is now complete in code; still no build, no
  ISO, no VM run.** Next: item 7.5 (Control Center) or item 7.3 (Start when the window button is
  pressed).
- 2026-10-25 (evening) — added the **"Handoff prompt"** section above, a self-contained first
  message for the next session (constraints, exact state, the queue, and the facts that would
  otherwise have to be re-discovered). No code changed.
- 2026-10-25 (evening) — item 7.5 **Control Center written, unverified**: it now works and moves.
  New in `flyouts.cpp`: `RevealHost` (+ `kRevealSlide` 16 px) slides the content up into place inside
  the window's clip on open (`Motion::FlyoutRevealMs`), and `ControlToggleRow` puts a real
  `ToggleSwitch` on a row; `CreateQuickSettings` gained an **Appearance** card with three switches —
  **Light mode**, **Top dock**, **Desktop icons** — each wired to new `Shell::SetLightMode` /
  `SetTopDock` / `SetDesktopIcons`, which apply immediately, refresh the surfaces that were painted
  in the old state through `Shell::ReloadChrome`, and save. `Shell::SaveSettings` now persists
  `Theme`/`TopDock`/`DesktopIcons` (a real gap: only `LoadSettings` read them, so neither Settings nor
  the Control Center could ever have kept a change). `Icon::Bluetooth` (+ code point `0xE1A7` in
  `painter.cpp`) labels the Bluetooth row. Network/Bluetooth/Volume/Brightness/Focus remain honestly
  labelled unavailable. **Still no build, no ISO, no VM run.** Next: item 7.3 (Start / app drawer).
- 2026-10-25 (evening) — item 7.3 **Start / app drawer written, unverified**: `AppSection` (a `Grid`
  subclass in `flyouts.cpp`) now owns Start's heading, its tile grid **and a new "All apps" / "Pinned"
  switch**, so the drawer can finally be browsed instead of only showing pins; the mode is element
  state (freed with the window, reachable from both the button and the search box). Searching filters
  and reads "Results" as before. Start slides in on open through `RevealHost` and is clamped to the
  work area; `RecentRow` gained the same hover `Transition` as `AppTile`. **Still no build, no ISO, no
  VM run.** Next: item 7.2 (installer), then 7.6 (folders) — or 7.13 (remove the beta apps).
- 2026-10-25 (evening) — item 7.2 **pre-flight space check written, unverified**. `setup.cpp` gains
  `MeasurePayload` (the install image + live boot image for the ESP, `C:\Nexora` for the system
  partition); `Install` measures and refuses with `STATUS_DISK_FULL` and a named `Job.Error` **before
  it erases anything** — the old measurement, which ran after the format, is now that same one, so
  `Job.TotalBytes` and the progress bands are unchanged. The Ready page's layout paragraph now names
  the copied sizes and turns accent-coloured when the disk is too small, and `UpdateButtons` will not
  enable **Install** in that case. Bootability is untouched and stays a *verification* item: the ISO
  is already El-Torito UEFI-bootable (`Makefile` `--efi-boot …`, `limine.conf` `/Install Nexora OS`,
  `install.tar` → `EFI/BOOT/BOOTX64.EFI`). **Still no build, no ISO, no VM run.**
- 2026-10-25 (evening) — item 7.6 **clean system layout written, unverified**. The `Makefile`
  ramdisk recipe now creates the whole `C:\Nexora` set in one `mkdir` —
  `System`, `Fonts`, `Apps`, `Wallpapers`, `Licenses` **and `Settings`** — with a comment stating the
  layout and why `C:\Users` is not part of it. `Settings` was a real gap: `SettingsPath()` resolves
  to `C:\Nexora\Settings\*.ini` and `Settings::Save`/`WriteAll` never create the parent folder, so the
  media shipped a system folder the OS had to write into but did not carry (an installed disk already
  had it, because `Install` creates it). The orphan `rootfs/Nexora/welcome.txt` was deleted.
  **Still no build, no ISO, no VM run.** Next: item 7.7 (Photo Viewer + an honest Video Player
  answer), then 7.8 (window snapping).
- 2026-10-25 (evening) — item 7.7 **Photo Viewer written, unverified, in both halves**. Decoder:
  `third_party/stb/stb_image.h` vendored with `third_party/stb/compat/{stdlib.h,string.h}` (user mode
  is `-nostdinc`, so the header's `<stdlib.h>`/`<string.h>` need shims) and `third_party/stb/LICENSE.txt`;
  `user/include/hzimage.h` + `user/lib/hzrt/hzimage.cpp` compile it (not a library) with
  `STBI_NO_STDIO/THREAD_LOCALS/HDR/LINEAR/PSD/PIC` and the `STBI_*` hooks on the Nexora heap/globals,
  recognise a file by its signature, decode to **`0xAARRGGBB`** (stb returns RGBA quads), keep PSD/HDR
  as *recognised but not decoded*, and turn a copy for rotation (no rotation in `Painter::DrawBitmap`);
  four `Makefile` edits (compat include, `USER_RUNTIME_OBJS`, `LICENSE_FILES`, ramdisk license).
  Viewer: `user/apps/photos/photos.cpp` (auto-discovered) with an `ImageStage` custom `Element`
  (fit/zoom, drag-pan, wheel zoom, pill zoom badge, a 0.94→1.0 reveal `Transition`) over a
  File/View `MenuBar`, a nine-button toolbar, folder stepping on Left/Right, and honest per-format
  failure text; `rootfs/Nexora/Apps/photos.app` (`Path=C:\Nexora\System\photos.exe`) keeps
  `validate-app-catalog.py` happy. **Video is not written and not claimed** — no demuxer, no codec,
  no audio device. Added the `stb_image` row to `THIRD_PARTY_NOTICES.md`. **Still no build, no ISO,
  no VM run.** Next: item 7.8 (window snapping + multitasking).
- 2026-10-25 (evening) — **decision recorded for §7 items 2 and 11 (Wi-Fi + internet browsing).** The
  user asked how to add both and chose to **build both** — a real 802.11 driver *and* the browsing
  stack — with the Wi-Fi path as a **real 802.11 driver pinned to one card**, not an emulated radio.
  Wrote the two-part plan into item 11: the browsing stack in the roadmap's Delivery-stage 3 order
  (`net/ipv4.c` → `net/udp.c` → `net/tcp.c` → `net/socket.c` + user API → HTTP → TLS → renderer, with
  local-file browsing preserved and plaintext HTTP refused), and the 802.11 project honestly scoped
  (no emulated radio in QEMU/VirtualBox, the PCI/DMA driver framework is unbuilt so drivers are still
  registered at build time, a WPA2/WPA3 supplicant is its own work, and no-firmware ath9k-class cards
  are preferred to avoid a firmware blob and its licence). **No code written; the exact PCI ID is
  still to be pinned.** Key ground facts confirmed by reading: `drivers/` has only a **wired** e1000
  NIC (no 802.11 of any kind); `net/` implements ARP, DHCP, DNS and packet parsing/checksums only —
  `NetIpv4Send`/`NetUdpSend`/`NetTcpConnect`/`NetSocketCreate` are prototypes in `include/net.h` with
  no definition; `init.c` calls `NetStart()` (DHCP) at boot; and `user/apps/browser/browser.cpp` is a
  real UI that refuses remote pages and says so. **Still no build, no ISO, no VM run.**
- 2026-10-25 (evening) — **Wi-Fi + browsing, both tracks started, written, unverified.** Browsing
  layer 1: `net/netcore.c` (`NetTransmitFrame` with `STATUS_DEVICE_BUSY` retries, `NetProcessFrame`,
  `NetReceiveWorker`, `NetInitializeStack` resetting ARP only, `NetStackTick`) — **this is the glue
  `net/arp.c` already called and nothing defined, so the kernel net layer could not have linked
  without it** — and `net/ipv4.c` (subnet/routing helpers, `NetIpSend` with DF and no fragmentation,
  `NetIpResolveAndSend` over ARP, and `NetIpReceive` answering ICMP echo so the machine can be
  pinged; UDP/TCP dropped with `STATUS_NOT_SUPPORTED` rather than calling unwritten deliver
  functions). The 802.11 side: `include/net80211.h`, the shared-layer contract modelled on
  `cfg80211` (driver ops + an embedded `NET_INTERFACE`, BSS/security/state types, and the shared
  entry points) with the ownership split written into the file — **contract only**: `net/net80211.c`,
  the supplicant and every card driver are not written, and the PCI ID to pin is still unresolved.
  `NetReceiveWorker` is deliberately not started yet (DHCP polls the same ring). **Still no build,
  no ISO, no VM run.** Next: `net/udp.c` (and moving `net/dns.c` onto it), then `net/tcp.c`.
- 2026-10-25 (evening) — **browsing layer 2 written, unverified: `net/udp.c`.** Bounded bindings
  (8 ports, each a 3-deep queue of 1472-byte datagrams under one spin lock), ephemeral ports in
  49152..65535, `NetUdpSend` with a real checksum (`NetUdpChecksum`, zero transmitted as 0xFFFF)
  over `NetIpResolveAndSend`, and `NetUdpDeliver` as the enqueue path — checksum verified first, and
  an unbound port or a full queue drops the datagram instead of overwriting a queued one. Wiring:
  `NetIpReceive`'s UDP case now parses and calls `NetUdpDeliver`, and `NetInitializeStack` resets the
  UDP table as well as ARP. `NetUdpReceive` polls the interface ring while its queue is empty because
  `NetReceiveWorker` is still not started (DHCP owns the ring at boot); the file states that this
  polling must become a pure event wait once the worker starts. **Still no build, no ISO, no VM
  run.** Next: move `net/dns.c` onto UDP (it still hand-rolls ARP/IPv4/UDP frames and polls the
  ring), then `net/tcp.c`.
- 2026-10-25 (evening) — **new user request batch recorded as §7 items 14–19 plus a consolidated
  "what a proper OS still needs" list. No code written for it.** The batch: (14) *can we reuse other
  projects' drivers?* — answered honestly (porting yes, loading no, because Linux drivers sit on
  Linux-internal APIs; `ath9k`/`iwlwifi`/`mt76` are GPL-2.0 so linking them would make the combined
  work GPL; and the actual blocker is that the Delivery-stage 2 PCI/DMA/MSI framework does not
  exist); (15) *run `.exe` and Linux apps* — recorded as three separate projects with a proposed
  incremental order, and stated plainly that no single step delivers it; (16) the missing apps /
  "many system apps", with the two honesty gates (an App Store needs a package + trust story first;
  a Media Player needs an audio driver and a codec, neither of which exists); (17) everything else
  that can be added to the File Explorer; (18) dock hover/motion polish on **both** docks and
  **volume controls — recorded as blocked, not faked**, because there is no audio device or mixer,
  with the recommendation to build the audio driver as its own step rather than ship a slider that
  controls nothing; (19) remove `blockyard` and the beta apps, noting `Makefile:143`
  `USER_PROGRAMS` drives both the build and `validate-app-catalog.py`. **Still no build, no ISO, no
  VM run.**
- 2026-10-25 (late) — **handoff prompt refreshed so the next session can start cold.** The
  self-contained block at the top of this file was stale: it still told the next session to "start
  item 7.7" and listed 7.7 as unwritten. It now records 7.7 as written in both halves (with video
  explicitly unclaimed), the real state of 7.11 (`net/netcore.c`, `net/ipv4.c`, `net/udp.c`,
  `include/net80211.h` written; TCP/sockets/HTTP/TLS/renderer and every 802.11 driver not), the
  7.14–7.19 batch with its honesty gates, and the suggested smallest-first order. Added facts the
  next session would otherwise re-discover: the Makefile auto-discovery rules
  (`KERNEL_SRC_DIRS`/`USER_APP_DIRS` wildcards vs. `USER_RUNTIME_OBJS`), the `-nostdinc` consequence
  for freestanding user mode (`third_party/stb/compat/`, `hzmath.h`), the fact that a language-server
  diagnostics call is a free syntax check but is **not** a build, the kernel `STATUS_*`/`Ke*`/`Ps*`/
  `rtl` helper lists, and why `NetReceiveWorker` must stay unstarted while DHCP and DNS read the ring.
  **Still no build, no ISO, no VM run, no box ticked.**

## Requested next steps — added 2026-10-25 (user)

Recorded so the request survives a lost history. Nothing here is claimed working; each item says
honestly whether the build can actually deliver it.

### 1. Terminal: Linux and Windows commands
Goal: the terminal understands both Unix-style and Windows-style commands.
- Verdict: **doable**; this is shell/terminal work (`user/apps/terminal/parse.cpp`,
  `console.cpp`, `terminal.cpp`).
- Needed: one command registry with aliases and both option styles — `ls`/`dir`, `cat`/`type`,
  `cp`/`copy`, `mv`/`move`, `rm`/`del`, `mkdir`/`md`, `rmdir`/`rd`, `pwd`, `cd`/`chdir`, `cls`/
  `clear`, `echo`, `tree`, `find`, `grep`, `where`/`which`, `set`/`export`, `%VAR%` and `$VAR`
  expansion, and `/switch` alongside `-option`.
- Status: **written, unverified** (2026-10-25). Done in `user/apps/terminal/parse.h`,
  `parse.cpp`, `terminal.cpp`: an `Environment` with `$NAME`/`${NAME}`/`%NAME%` expansion and `\$`/
  `\%` escapes; `IsSwitch` accepting both `-x` and `/x`; aliases `ls`/`dir`, `cat`/`type`, `cp`/
  `copy`, `mv`/`move`, `rm`/`del`, `mkdir`/`md`, `rmdir`/`rd`, `cls`/`clear`, `where`/`which`/
  `whereis`; and new commands `tree`, `find`, `sort`, `set`/`export`, `unset`, `env`/`printenv`.
  `USERNAME`, `USER`, `HOME`, `USERPROFILE`, `OS` and `PATH` are set at start-up; `Help` and
  `LinuxHelp` list the new names. **Not compiled and not run** — the standing no-build rule is in
  force.

### 2. Wi-Fi, Bluetooth and secure browsing
Goal: real Wi-Fi and Bluetooth, and secure web browsing.
- Verdict: **both radios are blocked on hardware and a driver, not on UI.** Nexora has no wireless
  driver of any kind (Wi-Fi or Bluetooth); the kernel must enumerate one named, tested adapter (its
  PCI/USB ID pinned) and its firmware path before any scan/association/pairing UI may exist. A
  settings toggle alone must never claim Wi-Fi or Bluetooth works.
- Browsing: **blocked on the network stack above DHCP/DNS** (ARP/IPv4/UDP/TCP) plus TLS. The
  roadmap forbids browsing over plaintext HTTP. Local-file browsing already exists.
- Status: **not written yet.** Decision taken 2026-10-25: build a **real 802.11 driver** (pinned to
  one tested PCI ID, no-firmware ath9k-class card preferred) **and** the browsing stack — see item 11
  below for the layered plan and the honest blockers (no emulated 802.11 in QEMU/VirtualBox, the
  Delivery-stage 2 PCI/DMA driver framework not built yet, WPA2/WPA3 supplicant as its own project,
  TLS as a maintained port). Requires the OS-5 stack and, for Bluetooth, USB/HID first.

### 3. Browser rebuild (Chrome-like feature set, not too advanced)
Goal: rebuild the browser with the features Chrome has — tabs, an address bar with search, back/
forward/reload/stop, bookmarks, history, downloads, find-in-page, zoom, a private window, and a
settings page — without claiming remote HTTPS in advance.
- Verdict: **partly doable now** — every feature above except remote navigation is local UI and
  state work, and can be built and used against local files. **Remote navigation stays unavailable**
  until HTTP and TLS exist; HTTPS needs a maintained TLS implementation with certificate-chain,
  hostname, expiry and trust-store checks and negative tests before any remote page is shown.
- Status: **not written yet** (the current browser is the M-milestone local one).

### 4. Run Linux and Windows applications (App Store full of them)
Goal: run Linux and Windows binaries and offer them in the App Store.
- Verdict: **possible only as a large, separate compatibility project** — not as an App Store
  feature. A foreign binary needs, at minimum:
  - **Linux:** an ELF64 loader, a Linux syscall ABI layer, a dynamic linker and a glibc-compatible
    libc, plus the kernel facilities those syscalls assume (futex, epoll, `/proc`, `/sys`, unix
    sockets, `mmap` semantics).
  - **Windows:** a PE32+ loader (the PE loader partly exists) plus a Win32/NT API layer, a registry,
    a DLL loader, and a real drawing/input path for anything graphical.
  - **Both:** file and process isolation, which the current single-profile, no-ACL kernel does not
    provide, and a legality/licensing review before redistributing any runtime.
- Honest scope: this is months of dedicated work per platform and **cannot** be represented as
  complete by listing third-party programs in a catalog.
- Decision needed from the user: **which ABI first** (Linux ELF or Windows PE), and **console-only
  programs first** (a far smaller surface) before any GUI support. The user's answer to "do both"
  is recorded: build **both** layers, Linux ELF first, console programs first.
- Status: **blocked / not started**; waiting on the ordering decision above.

### 5. Polish every built-in app (motion and better UI)
Goal: every built-in program gets consistent motion and a refined UI.
- Verdict: **doable now** for pure UI work (transitions, hover/press states, empty states, spacing,
  icons, keyboard focus rings) in `user/apps/*` on the `hzui` toolkit.
- Scope: Files, Notepad, Calculator, Calendar, Clock, Stopwatch, System Information, Task Manager,
  Photo Viewer, Music Player, Video Player, Screenshot Tool, Paint, Email, Contacts, Archive
  Manager, App Store, PDF Viewer, Terminal, Settings.
- Status: **not started** (terminal UI is the first target after its command parity above).

### 6. User profile picture
Goal: the user can set their own picture for the OS account.
- Verdict: **doable now** for a 24-bit `.bmp` (the only decoder that exists — `hz::wallpaper::
  LoadBmp` in `user/lib/hzui/wallpaper.cpp`). PNG and JPEG would each need a decoder (PNG needs an
  inflate implementation) and are **not** supported yet; the Settings note says so.
- Status: **written, unverified** (2026-10-25). `MakeRoundPicture` renders the file as an
  antialiased circular picture; Settings > Accounts has a preview and Choose/Remove buttons writing
  `shell.ini` key `ProfilePicture`; the shell's Start account row (`Avatar` in
  `user/shell/flyouts.cpp`) draws it. **Not compiled and not run.** The lock-screen account chip
  still shows the person icon.

### 7. Latest request — added 2026-10-25 (evening)

Verbatim intent, split into items so none is lost. **Status of every item below: not written yet**
unless stated. The standing no-build rule is in force, so nothing may be ticked even when written.

1. **File Explorer must behave like a real one** — extract archives, create files (not just folders),
   rename, "and much more". Real OS-like. Also: **a better File Explorer layout.**
   - Today: `user/apps/files/files.cpp` has NewFolder, Rename, Delete, Cut/Copy/Paste and unique-name
     `"X - Copy"` logic; `hz::fs` has Resolve/List/Read/Write/Copy/CopyTree/MoveTree/Delete/
     DeleteTree/CreateDirectory/Rename/Stat. **There is no archive support at all** (no ZIP, tar, gz,
     inflate or CRC-32 anywhere in the tree).
   - Verdict: create-file, rename, and layout are **doable now**. Archive **extract** needs a new
     module (`hzarchive`): ZIP central-directory reader + inflate + CRC-32 (tar/ustar and gzip are
     simpler and can follow). Decide ZIP-first.
   - Status: **written, unverified (2026-10-25 evening) — item 7.1 complete in code.** `user/include/hzarchive.h` +
     `user/lib/hzrt/hzarchive.cpp` add self-contained DEFLATE (RFC 1951) + CRC-32 + ZIP (stored and
     deflated) + tar (ustar, GNU long names) + gzip, with `Detect`/`List`/`Extract`/`IsArchive`/
     `StripSuffix`; entry names that would escape the destination are refused and every stream is
     CRC-checked. Added to `USER_RUNTIME_OBJS` in the `Makefile`. `files.cpp` gained
     `ExtractSelected()` and an "Extract all..." item in the file context menu (archives only,
     writable volumes); `IconFor`/`IconColorFor`/`TypeName` know archives and images, and three
     font-only icons were added (`Icon::Archive`, `Icon::Image`, `Icon::Play`). The sidebar became a
     Windows 11 Quick access list (Home/Desktop/Documents when they exist, then the volumes) with the
     current folder's row highlighted. **Not compiled and not run.**
2. **Installer must be bootable / behave like a real OS installer.**
   - The ISO target is **already El-Torito UEFI-bootable by construction** (`Makefile` uses
     `xorriso … --efi-boot …`) and `boot/limine.conf` carries `/Install Nexora OS` with
     `hz.installer=1`. So "bootable" is a **verification** item, not new code — it cannot be proven
     without the one build+VM pass already owed.
   - "Real installer" polish is **written, unverified** in `user/apps/setup/setup.cpp`: `MeasurePayload`
     sizes what Setup writes where, `Install` refuses a disk it cannot fill with `STATUS_DISK_FULL`
     and a named `Job.Error` **before the erase** (the measurement that used to run after the format is
     now the same one, so the progress bands are unchanged), and the **Ready to install** page names
     the sizes and blocks **Install** when they do not fit. The five pages, six named stages, verify
     stage, ERASE authorization, write lock and Restart/Shut down completion were already in place.
3. **Re-polish the app drawer when the window button is pressed.** The start button is
   `ToggleStart`/`TaskbarClick` in `user/shell/`; the Start/app drawer lives in
   `user/shell/flyouts.cpp`. **Written, unverified** (2026-10-25): `CreateStartMenu` now builds one
   `AppSection` (a `Grid` subclass) that owns the heading, the **All apps / Pinned** switch and the
   tile grid, so Start finally has a browsable app drawer instead of pin-only tiles (the mode lives
   in the tree and is freed with the window; the search box and the button both reach it through the
   section pointer). Searching narrows to matches and renames the section "Results", as before; Return
   opens the best match. The whole menu slides into place on open through `RevealHost`, `RecentRow`
   gained the same hover `Transition` as `AppTile` (it used to paint instantly), and the flyout's
   size is clamped to the work area like the Control Center's.
4. **Remove all mock stuff; use real icons from the internet (download or make).**
   - Decided: **download an open-source set**. An **icon font** is the right pipeline because
     `user/lib/hzuser/font.cpp` already rasterizes TTF at runtime via `stb_truetype` (and decodes
     UTF-8, so Private-Use-Area codepoints can be drawn by glyph). **Material Icons Regular
     (Apache-2.0)** is fetchable and was confirmed available; Tabler webfont and Material Symbols
     variable-font URLs were 404.
   - Status: **written, unverified (2026-10-25 evening).** Downloaded `MaterialIcons-Regular.ttf`
     (356,840 B), `MaterialIcons-Regular.codepoints` (39,522 B) and the Apache-2.0 `LICENSE.txt`
     into `third_party/icons/material/`. Added `HzIconFont(size)` (per-size cache, 13 steps) and
     `HzDrawGlyphCentered` to `user/lib/hzuser/font.cpp` + `user/include/hzuser.h`; added
     `IconCodePoint` (all 46 mapped `hzui::Icon` values → Material code points; `Icon::Nexora` has no
     equivalent and keeps its vector mark) and `Painter::DrawIconGlyph` to
     `user/lib/hzui/painter.cpp` + `user/include/hzui.h`, hooked at the top of `DrawIcon` so the 47
     vector paths remain the fallback when the font file is missing. Wired the TTF into the
     `Makefile` (`UI_FONTS`, `LICENSE_FILES`, ramdisk copy to `Nexora/Licenses/MaterialIcons-Apache-2.0.txt`)
     and added the row to `THIRD_PARTY_NOTICES.md`. **Not compiled and not run** — the icons are
     therefore unverified; if a call site passes a non-square or tiny box the glyph is centred by
     ink, which is the intended behaviour but has not been seen on screen.
5. **Control Center must work, and be polished with motion/animation.** Control Center code lives in
   `user/shell/` (Quick Settings). **Written, unverified** (2026-10-25): `CreateQuickSettings` gained
   an **Appearance** card whose three switches really work — light/dark (`Theme`), the top dock
   (`TopDock`) and the desktop icons (`DesktopIcons`) — each applied at once through
   `Shell::SetLightMode` / `SetTopDock` / `SetDesktopIcons` + `Shell::ReloadChrome` and saved by
   `SaveSettings` (which now writes those three keys; before only `LoadSettings` read them, so no
   change could ever have persisted). The flyout slides its content up into place on open
   (`RevealHost`, `Motion::FlyoutRevealMs`; hzui has no opacity layer, so the motion is position).
   Bluetooth/Volume/Brightness/Focus stay labelled unavailable and are not faked.
6. **Clean the OS directories like Windows does.** `rootfs/` + ramdisk layout
   (`Nexora/System`, `Nexora/Fonts`, `Nexora/Apps`, `Nexora/Wallpapers`, `Nexora/Licenses`).
   **Written, unverified** (2026-10-25): the `Makefile` ramdisk recipe now creates all six system
   folders — the five above plus `Nexora/Settings`, the folder `SettingsPath()` actually reads and
   writes (the installed tree already had it; the media did not) — and states the layout once in a
   comment, including why `C:\Users` is not in the image. `rootfs/` stays a verbatim mirror of the
   system volume: its `Nexora/Apps/*.app` manifests and nothing else. The stray
   `rootfs/Nexora/welcome.txt` (an orphan note; no code reads it) was removed.
7. **Video Player supports all famous formats; Photo Viewer supports popular ones.**
   - Decided: **real decoders**. `third_party/stb/stb_image.h` (283,010 bytes) was fetched and can be
     vendored (ASCII-only; do not also paste `stb_image_resize2`). Photo Viewer = PNG/JPEG/BMP/TGA/
     GIF through `stb_image` + a new `user/include/hzimage.h`.
   - Video is much larger: H.264/H.265 need a real codec and audio needs an output device (the kernel
     has **no audio driver**). Verdict: a **container/demux + open-codec** player is a large separate
     project; must not be claimed as "all formats" before a decoder and audio path exist.
   - Status: **Photo Viewer written, unverified (2026-10-25 evening); video is still not written.**
     `third_party/stb/stb_image.h` (283,010 B) is vendored with `third_party/stb/compat/{stdlib.h,string.h}`
     (user mode compiles `-nostdinc`) and `third_party/stb/LICENSE.txt`; `user/include/hzimage.h` and
     `user/lib/hzrt/hzimage.cpp` compile it in place with the `STBI_NO_*` switches and the `STBI_*`
     hooks on the Nexora heap, so no libm and no TLS directory are needed. `hz::image` recognises a
     file by signature (JPEG/PNG/BMP/GIF/TGA/PNM), decodes it to `0xAARRGGBB`, keeps PSD/HDR as
     *recognised but not decoded*, and turns a copy for rotation. The viewer is
     `user/apps/photos/photos.cpp` (+ `rootfs/Nexora/Apps/photos.app`), a real app with a File/View
     menu, a nine-button toolbar, folder stepping and honest per-format failure text. **Video is a
     large separate project** (a container demuxer plus an open codec, and for sound an audio device
     the kernel does not have): nothing was written for it, and it must not be claimed as "all
     formats". **Not compiled and not run.**
   - Follow-up worth doing in 7.12: `user/shell/shell.cpp`'s manifest-icon table still maps the icon
     name `Image` to `Icon::Palette` (and has no `Archive`/`Play` entry), so `photos.app`'s
     `Icon=Image` draws the palette tile in Start and on the desktop instead of the new font-only
     `Icon::Image` (0xE3F4).
8. **Window snapping + multitasking like Windows, with motion/animation.** Snap layouts, drag-to-
   edge, halves/quarters, keyboard shortcuts, a snap-assist overlay, and Alt+Tab/task-view.
   **Not written**; the shell currently has no snapping.
9. **Explain how the OS behaves, including installations.** Already delivered once; re-state on
   request from the ledger + `docs/`.
10. **Do all the skipped things we did not do.** = items 7–9 here plus the older item 5 app list
    (Photo Viewer — **now written**, item 7.7; Music Player, Video Player, Screenshot Tool, Paint,
    Email, Contacts, Archive Manager, App Store, PDF Viewer) and the OS-2…OS-7 / Delivery-stage items
    in the phased checklist.
11. **Wi-Fi works, browsing works.** **Decision taken 2026-10-25: build both — a real 802.11 driver
    *and* the browsing stack.** Faking the toggle remains forbidden. The two are separated because
    they have different shapes and different blockers:
    - **Browsing (delivers "internet browsing", works over the existing wired e1000) — the
      prerequisite.** Nothing above DHCP/DNS is implemented: `NetIpv4Send`, `NetUdpSend`,
      `NetTcpConnect` and `NetSocketCreate` are prototypes in `include/net.h` with no definition.
      **Written, unverified (2026-10-25 evening): `net/netcore.c`** (the missing `NetTransmitFrame`
      that `net/arp.c` already called, plus `NetProcessFrame`, `NetReceiveWorker` — not started yet —
      and `NetInitializeStack`) **and `net/ipv4.c`** (subnet/routing, `NetIpSend` with DF and no
      fragmentation, `NetIpResolveAndSend`, and a real ICMP echo reply; TCP dropped with
      `STATUS_NOT_SUPPORTED` until net/tcp.c exists), **and `net/udp.c`** — the bounded binding/queue
      layer with checksum-verified `NetUdpDeliver`, now reached from `NetIpReceive`'s UDP case. The
      roadmap's Delivery-stage 3 already names the order and the files, so work proceeds in layers:
      `net/ipv4.c` (send/route/receive) → `net/udp.c` (+ move DNS off its hand-rolled frames) →
      `net/tcp.c` (connect state machine, retransmit, timeouts — the constants are already in
      `include/net.h`) → `net/socket.c` + a bounded **user API** (new services through
      `ex/svctable.c`, `user/lib/hzsys/hzsys.asm`, `include/uapi`) → a bounded **HTTP** client →
      **TLS** → the HTML/CSS **renderer**. TLS must be a *maintained implementation* (BearSSL is the
      small fit; mbedTLS the larger), with entropy (RDRAND), a trust store, certificate-chain and
      hostname validation, and negative tests; plaintext HTTP navigation stays rejected. Each layer
      is gated by protocol tests; the final gate is DNS + verified HTTPS through QEMU/VirtualBox NAT.
      **Local-file browsing stays available throughout.**
    - **Wi-Fi (real 802.11) — a separate, larger driver project, pinned to one tested card.**
      There is **no emulated 802.11 radio** in QEMU or VirtualBox, so this cannot be demonstrated in
      the VM and can only be verified on the pinned physical card. It needs: (i) PCI + BAR + DMA +
      MSI/interrupt infrastructure (the roadmap's Delivery-stage 2 "driver framework", not built yet
      — today drivers are registered at build time); (ii) the 802.11 MAC (scan, associate) and a
      **WPA2/WPA3 supplicant** (4-way handshake, PBKDF2, CCMP/AES) as its own piece of work;
      (iii) a control API so user mode can scan/connect; (iv) an adapter that presents itself as a
      `NET_INTERFACE` so the IP stack above is unchanged. **No-firmware cards are strongly preferred**
      — an ath9k-class PCIe card (AR9280/AR9285/AR9287) needs no firmware blob, so no additional
      licence or blob distribution. Cards like the Intel AX200 need an `iwlwifi`-style firmware blob
      with its own licence, which would also have to go into `THIRD_PARTY_NOTICES.md`.
    - **Hardware target: still to be pinned.** "A real 802.11 driver" requires one exact PCI ID and
      its firmware path named before the PCI/DMA layer is written. Awaiting that choice.
      **Written, unverified (2026-10-25 evening): `include/net80211.h`** — the shared-layer *contract*
      only (driver ops + an embedded `NET_INTERFACE`, BSS/security/state types, shared entry points),
      so that supporting many cards later means adding a driver rather than rewriting the wireless
      logic. `net/net80211.c` (the shared layer and its WPA2-PSK/WPA3 supplicant) and every card
      driver under `net/80211/` are **not written**.
12. **Re-polish every component and everything in the OS.**
13. **Remove all useless beta apps.** Candidates: `USER_PROGRAMS := hello usertest crash fstest
    wintest uitest apptest` (line 140 of the `Makefile`), the one-off `welcome` app/manifest, and the
    placeholder brochure page in `user/apps/browser/browser.cpp`. **Not written.**
14. **"Can we reuse other projects' drivers instead of writing our own?"** — asked 2026-10-25.
    Answer, in three parts, because "use someone else's driver" is true only with qualifications:
    - **Porting is possible; loading is not.** Linux drivers (`ath9k`, `iwlwifi`, `mt76`, `e1000e`…)
      are C written against the Linux kernel's *internal* APIs — `struct net_device`, `sk_buff`,
      DMA/PCI helpers, workqueues, `cfg80211` — and expect a module loader and symbol tables this
      kernel does not have. Nothing can be dropped in as a binary. Reuse in practice means
      **porting** a driver's register-level logic into this kernel's driver model, or
      **re-implementing** it from the datasheet.
    - **Licence is a hard gate.** `ath9k`/`iwlwifi`/`mt76` are **GPL-2.0**; linking GPL driver code
      into a kernel makes the combined work GPL. If Nexora is not going to be GPL, those cannot be
      copied in — a permissively licensed driver (BSD/MIT) or original code is required. Whichever
      is chosen must be recorded in `THIRD_PARTY_NOTICES.md`.
    - **The real blocker is the framework, not the driver.** Delivery stage 2 (PCI + BAR + DMA + MSI
      interrupts, a driver registry, a device manager) does not exist; drivers are registered at
      build time. No ported driver has anywhere to plug in until it does.
    - **Therefore:** build the framework first, then implement the *essential* drivers natively
      (e1000 exists; HDA audio, NVMe, USB xHCI/HID do not), and port third-party code only where a
      licence-compatible source and a real card justify it.
15. **Run Windows `.exe` and Linux ELF applications.** The roadmap's compatibility-layer goal, and
    the largest single item in this file. It is **three separate projects**, not one switch:
    - *What exists:* only the shell of it. User programs are already PE32+ and built by this
      toolchain, and the kernel loads PE images for user mode. That is a loader's beginning, not an
      ABI layer.
    - *What is missing (Windows side):* import-table and relocation resolution against foreign
      DLLs, a Win32 API surface (`kernel32`, `user32`, `gdi32`, `ntdll`), a registry, thread/TLS/SEH
      semantics, and a `win32k` path for GUI binaries.
    - *What is missing (Linux side):* an ELF64 loader (static first, then dynamic linking) and a
      Linux syscall table with a `libc` shim.
    - *Honest statement:* this can be built incrementally and each API tested against real binaries,
      but **no single step delivers "runs .exe files"**, and saying otherwise would be a fake.
      Proposed order: (i) PE import/relocation loader + a minimal `kernel32`
      (`GetStdHandle`/`WriteFile`/`ExitProcess`) → (ii) `user32` + `win32k` windowing for GUI `.exe`
      → (iii) ELF64 static loader + syscall table → (iv) shared C-runtime shims. Each gated by real
      binaries, each recorded as its own delivery stage.
16. **The missing apps, and "many system apps".** Present in `user/apps`: `browser`, `calc`,
    `calendar`, `clock`, `files`, `notepad`, `paint`, `photos`, `settings`, `setup`, `stopwatch`,
    `sysinfo`, `taskmgr`, `terminal`, plus `blockyard`. **Missing, requested:** App Store, Media
    Player (Music + Video), Disk Manager, Device Manager, Screenshot Tool, Archive Manager (the
    engine exists as `hz::archive` but has no app UI), PDF Viewer, Email, Contacts, Software Update,
    Backup & Restore, Font Viewer, Services/Startup, Network & Wi-Fi panel, Sound panel, Display
    panel, Print (no spooler exists). Two honesty gates:
    - **An App Store needs a package story first** — a package format, repository metadata,
      signature/trust verification, and the working network stack from item 11. Without those it is
      a shell that can only list what is already installed.
    - **Media Player has a hard dependency.** Photos is written (item 7.7). Video and music cannot
      play without a codec and an audio device; no audio driver exists (Delivery stage 8). The
      player must say that rather than draw a transport bar that plays nothing.
17. **File Explorer — everything else that can be added.** Candidates, all real and all testable:
    column/details view with sortable headers; breadcrumb address bar in place of the path box;
    multi-select with Ctrl/Shift and a rubber-band; cut/copy/paste with `Ctrl+X/C/V` (moves through
    `hz::fs::MoveTree`, which exists); undo for rename/move; drag-and-drop into folders and onto the
    desktop; a real Recycle Bin view (the `Trash*` API exists) with restore and empty; zip *create*
    (the engine reads archives only); per-folder view settings persisted like Windows; a properties
    sheet with computed folder sizes; search with a scope and a match-count; "Open with" from the
    manifest catalog; and a details pane showing the selected item's metadata.
18. **Dock polish (both docks), hover, motion, volume controls.** Finish Windows-11-style hover
    (scale + light + tooltip on the top dock's task buttons and the bottom dock's tiles), running
    indicators, window previews on hover, a smooth magnify/intensify curve on the bottom dock, and
    the same `Transition`-based motion everywhere else (nothing in `hzui` has an opacity layer, so
    motion must stay positional/scale — the existing style). **Volume controls are blocked, not
    faked:** there is no audio device, no audio driver and no mixer, so a volume slider can only be
    a slider that changes a number nothing consumes. Either (a) leave it labelled unavailable, or
    (b) build Delivery stage 8's HDA audio driver plus a mixer first and then wire a real slider.
    **Recommend (b) as its own step, not a slider now.**
19. **Remove `blockyard` and the other useless apps.** `user/apps/blockyard/` (4 files:
    `blockyard.cpp`, `render.cpp/h`, `world.cpp/h`) plus its manifest `rootfs/Nexora/Apps/blockyard.app`,
    and the beta programs in item 13 (`hello`, `usertest`, `crash`, `fstest`, `wintest`, `uitest`,
    `welcome`, `apptest` under `user/tests/`, the `welcome.app` manifest, and the placeholder page in
    `user/apps/browser/browser.cpp`). Note for whoever does it: `Makefile:143` `USER_PROGRAMS` drives
    both the build and `scripts/validate-app-catalog.py`'s `--binary` list, so all three must change
    together. **Not written.**

### Consolidated: what a proper OS still needs (from the 2026-10-25 batch)

Recorded so the list survives a lost history. Nothing here is claimed working. Grouped by the
smallest thing that can be finished and verified on its own:

- **Compatibility layer** (item 15) — PE loader + Win32 API surface; ELF64 loader + Linux syscalls.
  Largest item; several delivery stages.
- **Driver framework** (Delivery stage 2) — PCI + BAR + DMA + MSI, driver registry, device manager.
  Everything below depends on it.
- **Essential drivers** — NVMe, USB xHCI + HID (keyboard/mouse on modern machines), HDA audio
  (+ mixer), VirtIO-GPU, plus one pinned 802.11 card for item 11.
- **Network** (item 11) — UDP done; then DNS on UDP, TCP, sockets + user API, HTTP, TLS, renderer.
- **Installer / delivery** — Phase 2–3 wording and lock review, real bootability verification,
  recovery/rollback, repair, factory reset (OS-6).
- **Apps** (item 16) — App Store (needs packages), Media Player (needs audio), Disk Manager, Device
  Manager, Screenshot Tool, Archive Manager, PDF Viewer, Email, Contacts, Software Update, Backup,
  Font Viewer, Services/Startup.
- **Shell polish** (item 18) — window snapping + multitasking (7.8), dock hover/motion, the motion
  system + reduced-motion (OS-2), context menus.
- **Explorer** (item 17) — the list above.
- **Truthfulness debt** — Control Center's Network/Bluetooth/Volume/Brightness/Focus rows and
  Settings' unsupported pages must keep saying they are unavailable until the driver behind each one
  exists (OS-3). This is not optional polish; it is the project's standing rule.
- **Cleanup** (item 19) — remove `blockyard` and the beta/test programs.

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
3. **Complete networking and secure browser access.** Build on `NET_INTERFACE`, E1000, DHCP, and packet
   parsers. Deliver in testable layers: ARP and IPv4 send/receive/routing; UDP checksums and DNS;
   TCP connection state, retransmission, and timeouts; a bounded socket/user API; then a bounded
   HTTPS with a maintained TLS implementation, trusted certificate chain and hostname
   verification wired into browser navigation. Do not browse pages over plaintext HTTP. Add a
   second practical NIC target after the framework (VirtIO-net is suitable for VM testing;
   physical NIC choice must be pinned to tested PCI IDs). Candidate files: `net/arp.c`,
   `net/ipv4.c`, `net/udp.c`, `net/dns.c`, `net/tcp.c`, `net/socket.c`,
   `drivers/net/virtio_net.c`, and `include/uapi` API definitions.
   Gate each layer with protocol tests; final gate is DNS plus verified HTTPS reachability through
   QEMU NAT and VirtualBox NAT with bounded response handling.
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
and verified installed-disk boot; defer dual boot and manual partitioning. Recovery is in the
first-release scope, gated by data-safety tests. Updates require verified recovery and user approval
before download or installation.

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

## Proposed Nexora OS installed-system roadmap

### Purpose and scope

This is the proposed next product roadmap after installer-first boot and installation. It builds on
the existing M12 Nexora OS rather than starting a second OS or replacing its kernel and shell. The
target is a polished, dependable, locally usable desktop OS installed from the Nexora ISO, tested
first in QEMU and VirtualBox. Use the completed
[product decisions](./NEXORA-OS-INSTALLER-AND-OS-DECISIONS.md) as requirements.

The work is delivered in small, bootable milestones. Each milestone updates the installed payload,
builds the ISO on E: or F:, installs onto a fresh disposable disk image, boots that disk without the
ISO, captures a screenshot, and runs its focused regression tests before starting the next
milestone. Do not represent a capability as supported until its end-to-end test passes.

### Existing foundation and known gaps

Already present: UEFI/Limine boot, a hybrid x86-64 kernel, processes and threads, virtual memory,
FAT32 storage, the framebuffer window manager and compositor, a user-mode UI toolkit, a Nexora
shell with Start/taskbar/Control Center, settings persistence on installed volumes, first-boot
setup, password hashing and checking, and a catalog of native apps. The installer and
installed-disk boot paths have focused QEMU smoke cases.

This is a feature-completion and reliability project, not a promise of Windows/Linux app
compatibility or universal PC hardware support. Known limits to plan around include one local
profile, no kernel-enforced file/process access control, visual-only brightness/audio controls,
PS/2-only input, software-only graphics, and a browser that handles local content and DNS but does
not fetch remote pages. Audit these against the actual code and tests at the first milestone;
the list is a starting point, not an assumption that every item remains unchanged.

### Delivery milestones

#### OS-0 — Lock scope and establish the installed baseline

- Inventory each accepted product requirement against current implementation, code location,
  tests, and actual status: working, partial, unavailable, or deferred.
- Rebuild the ISO with the pinned toolchain located on E: or F:. Record the build identity and
  preserve existing user changes.
- From a fresh disposable image, run `install`, `diskboot`, `diskboot2`, and `diskboot3` in order;
  verify the test cases and current screenshots reflect installer-first boot rather than live
  desktop boot.
- Inspect the first-boot UI, normal sign-in/lock flow, taskbar/Start, Control Center, Settings,
  file persistence and shutdown/restart. Record issues without changing unrelated subsystems.
- Capture baseline screenshots for installer completion, first boot, unlocked desktop, Start,
  Control Center, Settings, and a core app at 1024x768 where supported.

**Gate:** a reproducible baseline, a requirement-to-evidence gap list, and no unexplained
regressions in the existing install and installed-boot cases.

#### OS-1 — First-boot welcome and guided setup

The post-install first boot is a dedicated setup experience, not the desktop or lock screen. The
installer remains responsible for partitioning, copying files, verification and restart. After
restart, the installed OS shows its booting screen and then opens the separate **Set up your device**
welcome page. The shell and lock screen must not become reachable until the user completes the
required setup steps.

Proposed flow:

1. **Booting screen:** show Nexora branding and a restrained loading/progress treatment while the
   installed system starts. Transition directly into first-boot setup when the durable setup state
   says onboarding is incomplete.
2. **Set up your device:** explain that this is Nexora's initial setup and offer Continue, About
   Nexora OS, and supported accessibility options. About is readable without starting the desktop.
3. **Local account:** collect display name and username; offer a password or a password-free local
   account, with confirmation whenever a password is entered. Explain the password-free trade-off.
   Do not authenticate into or launch the desktop from this page.
4. **Appearance and region:** let the user choose dark or light appearance, wallpaper, and
   supported time/region settings; provide sensible defaults.
5. **Choose apps:** show every optional installable app with a clear description. Required system
   components (shell, Settings and boot/installer essentials) are preselected and locked.
   Preselect Files, Browser, Terminal, Notepad, Calculator, Calendar, Clock, Stopwatch, System
   Information, Task Manager, Photo viewer, Music player, Video player and Screenshot tool. Offer
   Paint, Email, Contacts, Archive manager, App Store and PDF viewer as optional, initially
   unselected apps. Let users choose or deselect every optional app. Keep app payloads
   offline-capable from the ISO; do not expose unselected apps as installed/launchable; and report
   storage needs and install failures. Blockyard is not in the selected app catalog unless the user
   later asks to include it.
6. **Connect:** always show a Wi-Fi/Bluetooth page with a Skip button. Show only real device,
   scan, connection, and pairing state; if support is absent, explain that and let the user skip.
   Current baseline has E1000 Ethernet with DHCP/DNS but no Wi-Fi or Bluetooth stack; those need
   separately tested drivers and connection APIs.
7. **Review and finish:** summarize account, appearance, region, app and connectivity choices;
   allow Back to edit; save and verify persistent setup state; then show a clear completion
   transition into the desktop for the first session. On later boots, password-protected accounts
   require sign-in; password-free accounts go directly to the desktop.

Implementation requirements:

- Persist each page's accepted state on the installed system and resume safely after a power loss
  or setup-process failure. Do not expose the desktop/lock screen via Back, close, crash recovery,
  or an incomplete onboarding marker.
- Make account creation and app selection idempotent so returning to a page or resuming setup
  cannot duplicate accounts, folders, or package files.
- Keep the bottom taskbar always present after onboarding. The top dock is independently
  show/hide configurable in Settings. Start with Desktop shortcuts for Files, Terminal, Browser and
  Trash; keep their visibility configurable, and hide a shortcut if its target app is not installed.
- Provide Windows-like, target-sensitive right-click context menus throughout the desktop and
  Files, with actions appropriate to the selected item/background. Include Create folder and Create
  file on the desktop background and in empty Files locations. Provide a keyboard context-menu key
  equivalent, and ensure each menu action performs a real, testable operation.
- Audit existing password hashing and verification before reuse; do not store plaintext or imply
  kernel-enforced account isolation while processes can read the account file or raw disk.
- Keep About available from onboarding. Network and Bluetooth always get a setup page and Skip
  action; clearly distinguish unsupported hardware, unavailable drivers, scan/pair failures, and
  a user choosing to skip. Do not claim or simulate connectivity.
- Preserve user files and setup choices across shutdown and later boots; surface and log persistent
  write errors instead of marking setup complete.
- Capture screenshots at the booting screen, Set up your device/About, account, app selection,
  connectivity/skip, review and completion pages.

**Gate:** a fresh install boots to the booting screen then **Set up your device**, never to the
desktop or lock screen before completion; account, theme, region and app choices are saved to disk;
the connectivity page behaves honestly on supported and unsupported hardware; interruption resumes
onboarding; successful completion reaches the desktop; and a later reboot follows the configured
account sign-in path. QEMU tests cover supported virtual devices, with VirtualBox and physical
hardware results reported separately.

#### OS-2 — Desktop and shell reliability

- Refine the existing Nexora desktop, lock/sign-in screen, taskbar, Start/search, top dock,
  Control Center and app switching against the accepted visual direction: user-selected dark or
  light appearance, restrained accent, rounded surfaces, subtle transparency
  and motion, and no decorative noise.
- Design and implement a coherent motion system for the installed experience rather than adding
  unrelated effects screen by screen. Candidate moments include the boot-to-welcome transition,
  onboarding page changes, desktop reveal, window open/close/minimize/restore, Start and Control
  Center surfaces, taskbar feedback, and notifications when real notifications exist. Use short,
  eased transitions, clear hierarchy and consistent timing; motion should communicate state and
  keep the interface feeling responsive, not delay interaction.
- Profile the software compositor and UI thread before adding expensive blur, scaling, or
  full-screen effects. Prefer bounded transforms, opacity, and small-area redraws that work at the
  existing framebuffer resolutions. Provide an immediate/low-motion mode and respect a reduced-
  motion choice throughout boot, onboarding, shell, and apps; do not make critical controls depend
  on animation completing.
- Prioritize correct input, keyboard navigation and visible focus, move/resize/minimize/maximize/
  close, taskbar indicators, flyout dismissal, window ordering, responsive layout and predictable
  shell recovery. Maximize on title-bar double-click; support left/right window snapping through
  keyboard shortcuts. Keep the bottom taskbar always shown; let users turn the top dock on or off
  in Settings.
- Use the recommended starting layout: centered Start and pinned apps; group running windows by
  app with clear active indicators. Clicking a group shows a small preview/list; selecting an item
  activates that window, while middle-click closes that window. Start opens search first, followed
  by pinned apps and recent files/apps. Search installed apps, settings, and file/folder names
  locally; never search file contents or send searches online. Users can pin/unpin apps and launch
  installed apps from the app drawer.
- Start with Files, Terminal, Browser and Trash desktop shortcuts; let users configure shortcut
  visibility and do not show a shortcut to an app that is not installed.
- Implement consistent, target-sensitive right-click menus on the desktop background and items,
  Files background and file/folder items, taskbar pins/running apps, and app-drawer entries. Other
  controls get a context menu where it has a useful, real action. Support Create folder and Create
  file on the desktop and in Files locations where writing is permitted. Create file accepts a
  user-entered filename with any extension and creates an empty file; validate names and paths,
  prevent accidental overwrite, and open it only in an app with a real association. Include open,
  rename, copy, cut, paste, delete, pin/unpin and properties only when each operation is actually
  supported. Make Trash a real recoverable deletion workflow: delete moves items to Trash, users
  can restore or permanently delete individual items, and emptying Trash requires confirmation.
  Support keyboard context-menu invocation and test that menus dismiss and execute correctly.
- Keep motion short and purposeful; respect reduced-motion behavior if available. Do not sacrifice
  responsiveness or make a control appear functional when it is not.
- Fix reproducible shell/window-manager defects at their source and extend existing
  `wintest`, `uitest`, `apptest` or smoke cases rather than adding duplicate test mechanisms.

**Gate:** automated window/input/shell regressions pass; desktop interaction is manually checked
with keyboard and mouse; animation timing and interaction responsiveness are measured on the
software renderer; reduced-motion mode is verified; milestone screenshots are reviewed at
supported resolutions. Capture short recordings or frame sequences for transitions that a static
screenshot cannot demonstrate.

#### OS-3 — Settings, personalization and accessibility

- Audit each Settings category against actual OS APIs. Wire settings that have real behavior;
  label or hide unsupported controls rather than presenting visual-only toggles as operational.
- Implement and persist the agreed usable subset: theme/accent, wallpaper, account, time zone,
  display/system information, top-dock visibility, desktop shortcut visibility, installed-app
  selection, optional inactivity lock timeout and supported input/accessibility choices. Ensure
  shell and apps observe changes consistently. Keep the bottom taskbar visible.
- For password-protected accounts, require the password after every boot and manual lock; offer an
  optional configurable inactivity timeout. Default the timeout off unless the user enables it.
  Password-free accounts continue directly to the desktop after boot.
- Show large time/date, user name, selected wallpaper with a readable overlay, network state,
  battery/power status when available, accessibility shortcuts, and Restart/Shut down controls on
  the lock screen. Sleep appears only after implemented and tested.
- Build the compact floating Control Center with quick toggles and expandable network/device
  details. Include Wi-Fi/network status, Bluetooth/devices, Airplane mode, brightness/display,
  volume/audio, Night Light, Focus/Do Not Disturb, power saver, notifications, clock/date/calendar,
  accessibility shortcuts and Settings access. Each control must use a real OS API; show
  unsupported states honestly until the required hardware and services exist.
- Keep notifications quiet by default, useful and actionable, with per-app/system controls in
  Settings. Do not generate placeholder notifications.
- Improve keyboard-only operation, focus visibility, readable text, high-contrast/reduced-motion
  options where supported, and clear unavailable-state messaging.
- Verify settings survive restarts and invalid or failed saves remain visible to the user.

**Gate:** tests prove each exposed preference changes observable behavior, persists across reboot,
and fails visibly if it cannot be saved. Unsupported categories are explicitly marked unavailable.

#### OS-4 — Core native apps and everyday file workflows

- Prioritize the existing Files, Notepad, Terminal, Settings, System Information, Task Manager,
  Calculator, Clock and Paint apps; verify actual functionality before visual expansion.
- Deliver the requested optional catalog: Files, Browser, Terminal, Notepad, Calculator, Calendar,
  Clock, Stopwatch, Paint, System Information, Task Manager, Photo viewer, Music player, Video
  player, Email, Contacts, Archive manager, App Store, Screenshot tool and PDF viewer. Only list
  apps when their core task is implemented and tested; stage apps requiring missing network, media,
  document or filesystem capabilities rather than presenting placeholders.
- Prioritize App Store/package management and the Video Player after current core apps. The
  App Store installs/removes optional packages from the verified catalog bundled on the ISO/system;
  required OS components cannot be removed. Keep basic optional-app management available from
  Settings > Apps even when the standalone App Store is not selected during onboarding. An online
  Nexora catalog is later work requiring a trusted source, package integrity/signatures, user
  approval and safe rollback; do not imply third-party or online app installation in the initial
  release.
- The Video Player's requested format targets include MOV, MKV and MP4 video and MP3 audio.
  Select maintained decoders/codecs lawfully, impose resource limits, and test actual files and
  malformed inputs per format before listing it as supported; additional formats remain
  explicitly unconfirmed until tested.
- Keep required OS components preselected and locked during first-boot selection. Preselect Files,
  Browser, Terminal, Notepad, Calculator, Calendar, Clock, Stopwatch, System Information, Task
  Manager, Photo viewer, Music player, Video player and Screenshot tool; offer Paint, Email,
  Contacts, Archive manager, App Store and PDF viewer as optional, initially unselected apps.
- Respect the saved app selection in the installed catalog, app drawer, Start/search, desktop
  shortcuts and launch paths. Do not show a dead shortcut when an app is deselected.
- Exercise create/open/edit/save/copy/move/delete flows in the persistent user folders, including
  context-menu Create folder/Create file (arbitrary user-entered extension, initially empty) on
  desktop and in Files, overwrite prevention, delete/restore/permanent-delete and confirmed Trash
  emptying, cancellation, invalid paths, low-space/write failures and restart persistence.
- Improve usability and consistency through the shared hzui toolkit and app manifests. Keep the
  catalog and Start/taskbar entries truthful; do not add shell apps or claims for features that
  are not implemented.
- Keep app crashes isolated from the session, surface actionable errors, and avoid losing unsaved
  work on close/shutdown where the application already supports prompts.

**Gate:** app-level tests and an installed-system workflow demonstrate the core tasks against the
real persistent FAT32 volume, then confirm results after reboot.

#### OS-5 — Nexora Browser and honest network capability

- Extend the current E1000/DHCP/DNS foundation in layers: packet/routing tests, UDP and socket
  interfaces, bounded TCP connections and timeouts, then bounded HTTP retrieval and browser
  navigation.
- Bound memory, response sizes, redirects, timeouts and parser work. Handle malformed or
  interrupted responses without hanging the desktop or kernel.
- HTTPS is required for web browsing. Do not send web page requests over plaintext HTTP. Only
  expose remote browsing after a maintained TLS implementation, certificate-chain and hostname
  validation, trust-store handling, and negative tests are in place. Until then, clearly state that
  secure web browsing is unavailable; preserve local-file browsing.
- Keep local-file browsing available and preserve address/history/bookmark behavior that already
  works.

**Gate:** deterministic protocol tests plus QEMU and VirtualBox NAT tests verify successful
navigation, DNS/network failure handling, timeout bounds and malformed-response handling. HTTPS
must pass certificate-chain, hostname, expiry, trust-store and invalid-certificate tests before
release; plaintext HTTP navigation must be rejected.

#### OS-6 — Reliability, recovery boundaries and hardware truth

- Run focused and full self-tests on one and multiple QEMU CPUs; diagnose health failures instead
  of weakening checks solely to make a green result.
- Verify normal shutdown/restart flushes supported storage and leaves the filesystem mountable.
- Include diagnostics, rollback, repair and factory-reset capabilities in the first OS release,
  delivered in safety order: diagnostics/log export first; verified backup and rollback next;
  repair tools only with specific read/write scopes and failure recovery; and factory reset only
  after explicit destructive confirmation and a tested way to preserve or export user data.
  Never run automatic repair that risks user files without consent. Test interrupted operations and
  rollback failure on disposable images.
- Updates may be offered only after recovery/rollback is verified. Notify the user and ask approval
  before downloading or installing; do not install or reboot automatically.
- Keep the current UEFI framebuffer, PS/2, AHCI and FAT32 paths as the tested baseline. Treat USB,
  NVMe, audio, Bluetooth and accelerated graphics as separately scoped driver projects with
  explicit tested devices and fallback behavior, not prerequisites for claiming the current
  desktop works.
- Run VirtualBox acceptance on a new disposable VDI. Report QEMU, VirtualBox and physical-machine
  evidence separately; never infer physical support from a VM pass.

**Gate:** all supported boot, filesystem, process, UI, install and persistence tests pass; failure
and recovery behavior is understood; rollback preserves user data; repair/reset have verified
confirmation and failure behavior; advertised hardware support matches tested devices.

#### OS-7 — Release integration and user-facing documentation

- Build a release ISO containing exactly the verified installed-system payload and boot
  configuration. Keep installation-media boot installer-only and installed-media boot defaulted
  to Nexora OS.
- Re-run the clean sequence: build, fresh blank-disk install, host-side partition/payload
  verification, boot with ISO detached, first-boot setup, account use, file/settings persistence,
  and subsequent reboot.
- Run the full QEMU suite on one and multiple CPUs and the agreed VirtualBox install/boot check.
  Capture final milestone screenshots for installer completion, booting screen, onboarding,
  desktop, Start, Control Center, Settings, Files and Browser.
- Update README and progress documentation with exact supported behaviors, tested environments,
  limitations, build/ISO location and safe installation steps. Store build images, VMs,
  toolchains and test disks on E: or F: as requested.

**Release gate:** every must-have requirement is tied to a passing test or an explicitly reported
blocker; installed boot works without the ISO; account and user data survive reboot; screenshots
are reviewed; and no unsupported feature is presented as complete.

### Working rules and deferred scope

- Complete milestones sequentially and keep the ISO bootable at each step. Do not start a new
  subsystem while an earlier acceptance gate is failing.
- For each implementation milestone: inspect its existing code and tests, make a bounded change,
  build with the pinned toolchain, run focused tests, then relevant install/diskboot tests and
  capture screenshots. Run the full suite at integration gates.
- Preserve the installer and current OS behavior unless a requirement explicitly changes it.
  Keep user data and old VMs untouched; use new disposable images for destructive tests.
- Defer dual boot, manual partitioning, multiple users, unapproved/automatic updates, general
  Windows/Linux compatibility, broad physical-device support, and any hardware feature without a
  testable driver and fallback. Recovery/rollback/repair/reset are release goals, but each remains
  blocked until its own data-safety gate passes.
- If this roadmap conflicts with a product decision, stop at that decision point for user review
  rather than silently changing the expected behavior.
