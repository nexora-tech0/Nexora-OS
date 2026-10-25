# Nexora OS: Installer and OS Decisions

This file records the answers provided in the completed installer and operating-system
questionnaire. The original blank [questionnaire](./NEXORA-OS-INSTALLER-AND-OS-QUESTIONNAIRE.md)
is kept for future revisions.

## Product direction

- Build a modern, complete desktop OS with polished graphics, smooth motion, and reliable
  functionality.
- Keep Nexora's original brand. Use Windows 11 as inspiration for usability, organization,
  windowing, context menus, and motion without copying its logos or branding.
- Offer dark and light styling during first-boot setup, with silver and restrained, customizable
  Nexora accents.
- Prioritize visual consistency and everyday usability, real core features and persistence, and a
  real disk installer.
- Keep the implementation lightweight; responsiveness and correct behavior take precedence over
  decorative effects.

## Installer scope and presentation

- Deliver a bootable ISO that can also be written to USB.
- User clarification: boot media must automatically open Setup and expose only installation and
  About information to normal users; do not offer the app desktop from the install ISO. Install
  the full OS and applications to disk. Bundle all installation files on the ISO for offline use.
  About is available from a button on Setup's welcome page.
- Target UEFI and test VirtualBox first, with QEMU coverage; expand physical hardware support after
  core installation works.
- Use a compact, centered, responsive, single-page-at-a-time wizard matching the installed Nexora
  visual style: premium charcoal/graphite, original Nexora symbol, modern sans-serif, restrained
  transparency, and configurable accent color.
- Provide Back/Next, a visible step indicator, keyboard navigation, and a review before disk writes.
  Use subtle, fast animations and no sound by default.
- Preferred flow: welcome; language/region; keyboard; optional network; disk selection;
  installation options; final review; system copy; bootloader configuration; verification; restart
  instructions.
- Installation must work offline. Network setup is optional and can support future services.
- Show disk identity, actual capacity, relevant device information, unsupported reasons, and a
  refresh action. Require explicit target selection; never guess.
- Initially support the currently implemented UEFI + GPT + FAT32 layout and automatic full-disk
  installation. Defer dual boot and manual partitioning until safe and tested.
- Before destructive writes, show the selected disk identity, size, and existing partitions;
  explain data loss; require typing `ERASE`; and keep a final Cancel option. Never silently erase
  another OS.
- Report useful failures, preserve diagnostics where possible, allow safe retries, and state when
  an interrupted install may have left a partial disk. Never claim success unless verification
  succeeds.
- Default progress should show the current stage/operation and overall progress. Show estimated
  time only when reliable; offer details/logs as optional.
- Cancellation after writes start is allowed only at safe checkpoints with a warning.
- On success, explain ISO/USB removal and offer Restart and Shut Down.
- Keep sensible defaults, initially label the system volume `NEXORA`, and determine the minimum
  disk requirement from the actual system payload and required storage.
- Basic diagnostic logs and repair functions may ship only where safely supported. A full recovery
  environment can come later.

## First boot, accounts, and data

- Create one local user on the first boot after installation. Multiple users, Guest, and online
  accounts are later work; do not suggest an online account without a real service.
- Ask for display name, username, password, and password confirmation. Store passwords securely
  and enforce genuine authentication; never store plaintext or bypass verification.
- First boot is a separate **Set up your device** flow after the installed-system booting screen.
  It must not enter the desktop or lock screen before setup completion. Offer display name,
  username, optional password with confirmation, appearance/wallpaper, time/region, app selection,
  Wi-Fi and Bluetooth pages, and a final review. Resume safely after interruption.
- Let users choose every optional app during setup; required system apps remain preselected and
  cannot be deselected. Install choices from the offline ISO payload and do not expose unselected
  apps as installed.
- App chooser catalog requested: Files, Browser, Terminal, Notepad, Calculator, Calendar, Clock,
  Stopwatch, Paint, System Information, Task Manager, Photo viewer, Music player, Video player,
  Email, Contacts, Archive manager, App Store, Screenshot tool and PDF viewer. Default preselection:
  Files, Browser, Terminal, Notepad, Calculator, Calendar, Clock, Stopwatch, System Information,
  Task Manager, Photo viewer, Music player, Video player and Screenshot tool. Paint, Email,
  Contacts, Archive manager, App Store and PDF viewer start unselected. Include an app only when
  its core function is implemented; Blockyard is not in the selected catalog currently.
- Always show Wi-Fi/Bluetooth setup with a Skip action. If a feature is unsupported, clearly say
  so; never simulate discovery, pairing or connectivity.
- No telemetry. Network use is allowed after setup; troubleshooting logs stay local, and any future
  upload must be optional and clearly explained.
- Preserve a persistent user home and files across restarts. Provide Documents, Downloads,
  Desktop, Pictures, Music, and Videos as supported, with sensible app-specific data locations.

## Installed system

- Start at a clean desktop; welcome screen/tour is optional, never forced.
- Keep the top dock optional and minimal, independently showable/hidden in Settings: compact
  rounded presentation, subtle transparency, pinned shortcuts, responsive layout, and no
  unnecessary duplication of the bottom taskbar.
- Keep the bottom taskbar always shown. Center Start and pinned apps; group running windows by app
  with clear active indicators. Clicking a group shows a small window preview/list; choosing an
  item activates it, and middle-click closes that window. Start opens search first, followed by
  pinned apps and recent files/apps. Search installed apps, settings and file/folder names locally;
  never search file contents or send search queries online. Provide an app drawer, app search and
  pinning.
- Default desktop shortcuts are Files, Terminal, Browser and Trash. Hide a shortcut if its target
  app is not installed. Deleting moves items to recoverable Trash; restore/permanent-delete are
  available there, and emptying Trash requires confirmation. Provide target-appropriate right-click
  menus throughout desktop and Files; Create folder and Create file must work on the desktop and
  writable Files backgrounds. Create file accepts a user-entered filename with any extension and
  creates an empty file, with path validation and overwrite protection. Support keyboard
  context-menu invocation.
- Support resizable windows, title-bar double-click to maximize, and left/right snapping through
  keyboard shortcuts.
- Control Center is a compact floating panel with quick toggles and expandable network/device
  details. Requested controls/status: Wi-Fi/network, Bluetooth/devices, Airplane mode,
  brightness/display, volume/audio, Night Light, Focus/Do Not Disturb, power saver, notifications,
  clock/date/calendar, accessibility shortcuts and Settings. Only report real state and behavior;
  unsupported hardware/services must be explicitly identified. Notifications are useful, quiet by
  default and controllable in Settings.
- Prioritize reliable move, resize, minimize, maximize, close, responsive input, keyboard shortcuts
  and the specified snapping behavior. Workspaces can come later.
- Password-protected accounts require sign-in after every boot and manual lock; an optional
  inactivity lock timeout is configurable and off by default. Password-free accounts start
  directly at the desktop.
- The lock/sign-in screen shows large time/date, user identity, selected wallpaper with a readable
  overlay, network status, battery/power status when available, accessibility shortcuts, and
  Restart/Shut down. Add Sleep only after it is implemented and tested. Authentication must use the
  real account system.
- Prioritize bundled App Store/package management and a Video Player after current core apps. The
  App Store installs/removes optional apps from the verified offline bundle, never required OS
  components; basic package management remains in Settings even if the standalone App Store is not
  selected. Online app catalogs require a trusted source, package integrity/signatures, approval
  and safe rollback. The Video Player targets MOV, MKV and MP4 video plus MP3 audio; claim each
  format only after codec and malformed-file tests. Additional formats are undecided. Only enable
  remote Browser browsing with verified HTTPS/TLS, hostname and certificate validation; until then
  preserve local-file browsing and state that secure web browsing is unavailable. Hide or clearly
  label unfinished features; do not use fake controls.
- Settings should keep the requested categories: System, Personalization, Network & Internet,
  Devices, Apps, Accounts, Privacy & Security, Accessibility, Date & Language, Updates & Recovery,
  and About Nexora OS. Follow Windows-like organization while retaining Nexora identity; controls
  must map to implemented OS functionality.
- Hide irrelevant unfinished features or explain their unavailability. Do not expose nonfunctional
  controls as real.
- Updates require a real updater and verified recovery/rollback. Notify the user and request
  approval before downloading or installing; do not install or reboot automatically.
- Provide Restart, Shut Down, and diagnostic-log export. Include diagnostics, verified rollback,
  repair, and factory reset in the first release only after each has passed its data-preservation,
  confirmation, interruption, and recovery safety tests. Factory reset must clearly disclose data
  loss and provide a tested user-data export/preservation path.
- Show version/build, CPU/memory, storage/free space, display, network status, licenses, and useful
  diagnostics.

## Accessibility, localization, and support

- Prioritize larger text where supported, high contrast, reduced motion, keyboard navigation, clear
  focus indicators, and pointer settings where supported. Screen reader support is a future
  priority.
- Begin with English and US English keyboard input; allow configurable time/date/time-zone settings
  and add languages later.
- Provide local help/troubleshooting, clear installer errors, accessible local diagnostics, and a
  Nexora website/repository link.

## Release priorities and acceptance

**Must-have:** safe target-disk selection; successful installation and independent boot from the
installed disk; persistent storage and accounts; functional desktop/window management; Settings and
personalization; responsive smooth motion; intended Browser/core-app feature set with limitations
documented.

**Must-have:** first-boot onboarding remains separate from the desktop until complete; users choose
optional apps; unsupported connectivity is explained honestly; dark/light theme choice; desktop and
Files context menus; truthful Control Center behavior; reviewable motion and accessibility settings.

**Must-have:** diagnostics, verified rollback, repair and factory reset are first-release scope,
with each capability enabled only after its data-safety gate passes.

**Should-have:** installer visual polish.

**Later:** dual boot, advanced partitioning, multiple users/Guest, broad hardware compatibility,
and unapproved/automatic updates.

Installation is successful only when:

1. The ISO boots and installs to an explicitly selected virtual or physical disk.
2. The machine restarts and boots from that disk without relying on the ISO.
3. The user can authenticate, use the desktop, save files/settings, restart, and verify persistence.

Acceptance environments: VirtualBox and QEMU with disposable blank disks; physical hardware only
after virtual testing; target 1024x768 and 1920x1080 where possible.

Never erase the wrong disk, claim success after a failed install, lose user data during an ordinary
restart, bypass password verification, present fake functionality as complete, or sacrifice
responsiveness for decoration. The installer must install the actual OS payload, configure
supported partitions and bootloader, verify its work, and produce an OS that runs independently of
the install media.
