# Nexora OS project plan

This project continues the existing Nexora OS foundation toward a dependable, polished installed
desktop OS. The installation ISO remains installer-first; the installed OS boots through a separate
first-run **Set up your device** experience before reaching the desktop.

## Planning documents

- [Implementation checklist and phased roadmap](./checklist.md) — installer validation steps,
  installed-OS milestones, feature requirements, and acceptance gates. Its top section, **"Working
  ledger — written, unverified"**, is the resume point after a lost history: read it first to see
  what code exists, what is still unverified, and which item is next.
- [Installer and OS product decisions](./NEXORA-OS-INSTALLER-AND-OS-DECISIONS.md) — agreed
  experience, app catalog, desktop behavior, Control Center, privacy, and release priorities.

## Where we are (keep this current)

No build, ISO, or VM run has been performed, by the user's standing instruction, since the current
wave of changes began. Every implementation recorded in the ledger exists on disk and is
**unverified**; no checklist gate has been ticked. One build plus a QEMU/VirtualBox pass is owed
before any `written` item can be called working.

**Resume point: the top of `checklist.md` — its "Handoff prompt — paste this to start the next
session" block is self-contained and was refreshed 2026-10-25 (late); below it, the "Written and
unverified" table and "Next up" are the state.** The newest list is § "Requested next steps" items
14–19 (the 2026-10-25 late batch: driver reuse, `.exe`/Linux binaries, the missing apps, Explorer
additions, dock hover/motion, volume controls, cleanup). Item 7 is the earlier one and is largely
written: a real File Explorer (archive
extract, create, rename), a bootable/real installer, Start/app-drawer re-polish, real icons from the
internet, a working + polished Control Center, a clean Windows-like directory layout, Photo and Video
Viewers, window snapping + multitasking with motion, removal of mock/beta apps, and Wi-Fi plus
browsing. Item 7.4 (real icons) is **written**: Material Icons is vendored and `Painter::DrawIcon`
draws it, with the 47 vector paths kept as fallback. **Item 7.1 is complete in code**: an archive
engine (`hzarchive.h`/`hzarchive.cpp`, in `USER_RUNTIME_OBJS`), "Extract all..." in the Files context
menu, archive/image icons and types, and a Windows 11 Quick access sidebar (Home/Desktop/Documents
when they exist, then the volumes) with the current folder's row highlighted. **Item 7.5 (Control
Center) is written**: it has an Appearance card whose three switches really change the light/dark
scheme, the top dock and the desktop icons — each applied to the shell at once and saved to
`shell.ini` (`Shell::SaveSettings` now writes `Theme`/`TopDock`/`DesktopIcons`, which before only
`LoadSettings` read) — and the flyout slides its content into place on open (`RevealHost`,
`Motion::FlyoutRevealMs`; hzui has no opacity layer, so the motion is position, never a fade).
**Item 7.3 is written too**: `CreateStartMenu` builds one `AppSection` that owns the heading, the tile
grid and an **All apps / Pinned** switch, so Start is a browsable app drawer and not pins only; the
menu slides into place on open and `RecentRow` now fades its hover like the tiles. **Item 7.2 is
written** as well: Setup measures what it will copy (`MeasurePayload`) and refuses a disk it cannot
fill with `STATUS_DISK_FULL` and a named error **before the erase**, and the Ready page names the
sizes — "bootable" itself stays a *verification* item because the ISO is already El-Torito
UEFI-bootable by construction. **Item 7.6 is written**: the `Makefile` ramdisk recipe now creates the
whole `C:\Nexora` folder set — `System`, `Fonts`, `Apps`, `Wallpapers`, `Licenses` and the `Settings`
folder `hz::fs::SettingsPath()` writes into but the media never carried — and the orphan
`rootfs/Nexora/welcome.txt` is gone. **Item 7.7 is written in both halves**: the picture decoder —
`third_party/stb/stb_image.h` vendored with `third_party/stb/compat/{stdlib.h,string.h}` and
`third_party/stb/LICENSE.txt`, compiled in `user/lib/hzrt/hzimage.cpp` behind `user/include/hzimage.h`
(`STBI_NO_STDIO/THREAD_LOCALS/HDR/LINEAR/PSD/PIC`, the `STBI_*` hooks on the Nexora heap, signature
detection, `0xAARRGGBB` output, PSD/HDR recognised but not decoded, rotation done on a copy) — and the
viewer, `user/apps/photos/photos.cpp` + `rootfs/Nexora/Apps/photos.app` (`ImageStage`, File/View menu,
nine-button toolbar, folder stepping, honest per-format failure text). **Video remains unwritten and
unclaimed**: no demuxer, no codec, no audio device. The rest of item 7 — 7.8–7.13 — is not written;
the Wi-Fi and remote-browsing work has **started** (see the paragraph below) but is far from done, and
neither toggle may ever be faked. **Next: item 7.8 (window snapping + multitasking), or the next
browsing step (`net/dns.c` onto `net/udp.c`) — see the Wi-Fi/browsing paragraph below.**

**Most recent request (2026-10-25, evening): add Wi-Fi and internet browsing.** The user asked how to
add both and chose to **build both** — a real 802.11 driver *and* the browsing stack — so the plan is
written into `checklist.md` §7 item 11 (with item 2 pointing at it), and **both tracks have now been
started (written, unverified)**. Two parts, deliberately separated:
- **Browsing** (delivers "internet browsing"; runs over the existing wired e1000, so no hardware
  dependency) in the roadmap's Delivery-stage 3 order: `net/ipv4.c` → `net/udp.c` (and move DNS off
  its hand-rolled frames) → `net/tcp.c` → `net/socket.c` + a bounded user API → a bounded HTTP client
  → TLS (a maintained port, BearSSL or mbedTLS, with a trust store and hostname validation) → the
  HTML/CSS renderer. Local-file browsing is preserved throughout and plaintext HTTP stays refused.
  **Written so far: `net/netcore.c`** — the `NetTransmitFrame` that `net/arp.c` already called and
  nothing defined (so the kernel net layer could not have linked without it), plus `NetProcessFrame`,
  `NetReceiveWorker` (not started yet: DHCP polls the same ring) and `NetInitializeStack` — **
  `net/ipv4.c`** (subnet/routing helpers, `NetIpSend` with DF and no fragmentation,
  `NetIpResolveAndSend` over ARP, and a real ICMP echo reply so the machine is pingable) — **and
  `net/udp.c`** (bounded port bindings each with a small receive queue, ephemeral ports, a real
  checksum on send, and checksum-verified `NetUdpDeliver` as the enqueue path, now reached from
  `NetIpReceive`'s UDP case; its receive polls the ring while the worker stays unstarted, and says
  so). **Next browsing step: move `net/dns.c` onto `net/udp.c` (it still hand-rolls ARP/IPv4/UDP
  frames and polls the ring), then write `net/tcp.c`.**
- **A real 802.11 driver**, honestly scoped: there is no emulated 802.11 radio in QEMU/VirtualBox, the
  Delivery-stage 2 PCI/DMA driver framework is not built yet, a WPA2/WPA3 supplicant is its own work,
  and no-firmware ath9k-class cards are preferred to avoid a firmware blob and its licence.
  **Written so far: `include/net80211.h`** — the shared-layer contract only (driver ops + an embedded
  `NET_INTERFACE`, BSS/security/state types, shared entry points), so supporting many cards later means
  adding a driver rather than rewriting the wireless logic. `net/net80211.c`, its supplicant and every
  card driver under `net/80211/` are **not written**.
**Blocking decision: the exact PCI ID of the 802.11 card to pin.**

**Newest request (2026-10-25, evening) — a large batch, recorded as `checklist.md` §7 items 14–19
plus a consolidated "what a proper OS still needs" list. No code written for it yet.** It asks for
driver reuse from other projects (answered: **porting** is possible, **loading** is not — Linux
drivers sit on Linux-internal APIs, `ath9k`/`iwlwifi`/`mt76` are GPL-2.0, and the actual blocker is
the missing Delivery-stage 2 PCI/DMA/MSI framework); running Windows `.exe` and Linux ELF binaries
(recorded as **three separate projects** with an incremental order — no single step delivers it);
the missing apps (App Store, Media Player, Disk Manager, Device Manager, Screenshot Tool, Archive
Manager, PDF Viewer, Email, Contacts, Software Update, Backup & Restore) where an App Store needs a
package + trust story first and a Media Player needs an audio driver and a codec; more File Explorer
features; dock hover/motion on **both** docks; **volume controls — recorded as blocked on an audio
driver and explicitly not to be faked**; and removing `blockyard` plus the beta apps. Suggested
order: cleanup (item 19) → Explorer (17) → dock hover/motion (18) → the apps with no new subsystem
(16) → driver framework → essential drivers → audio → then volume and Media Player → App Store once
packages and the network exist → compatibility layer one stage at a time.

**To resume after a lost history:** paste the "Handoff prompt" block from `checklist.md` (top of the
§ "Working ledger" section) as the first message of the new session. It carries the constraints, the
exact state, the remaining queue and the facts that would otherwise have to be re-discovered.

## Guiding principles

- Build on the existing kernel, shell, native apps, and installer rather than starting over.
- Keep installation safe and verify that the installed disk boots independently of the ISO.
- Make only implemented, tested capabilities appear functional; report unsupported hardware and
  services honestly.
- Prioritize responsiveness, accessibility, real functionality, persistent user data, and
  purposeful motion over decorative effects.
- Keep build artifacts, toolchains, VM disks, and test images on E: or F:.

Detailed implementation steps and phase completion gates are maintained in [checklist.md](./checklist.md).
