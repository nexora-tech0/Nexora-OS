# Nexora OS: Installer and OS Decisions

This file records the answers provided in the completed installer and operating-system
questionnaire. The original blank [questionnaire](./NEXORA-OS-INSTALLER-AND-OS-QUESTIONNAIRE.md)
is kept for future revisions.

## Product direction

- Build a modern, complete desktop OS with polished graphics, smooth motion, and reliable
  functionality.
- Keep Nexora's original brand. Use Windows 11 as inspiration for usability, organization,
  windowing, context menus, and motion without copying its logos or branding.
- Default to charcoal/graphite dark styling with silver and restrained, customizable accents;
  offer light mode and a setup-time choice.
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
- First boot should offer account setup, desktop and lock-screen wallpaper, theme, time zone,
  keyboard, accessibility, and optional network configuration, with sensible defaults and a skip
  path.
- No telemetry by default. Keep troubleshooting logs local; any future upload must be optional and
  clearly explained.
- Preserve a persistent user home and files across restarts. Provide Documents, Downloads,
  Desktop, Pictures, Music, and Videos as supported, with sensible app-specific data locations.

## Installed system

- Start at a clean desktop; welcome screen/tour is optional, never forced.
- Keep the top dock optional and minimal: compact rounded presentation, subtle transparency,
  pinned shortcuts, responsive layout, and no unnecessary duplication of the bottom taskbar.
- Keep the bottom taskbar as the primary application/system-control area. Provide configurable
  desktop icons, a Windows-inspired but Nexora-branded Start/launcher, app search and pinning,
  running-app indicators, and a system tray with real controls and notifications where supported.
- Prioritize reliable move, resize, minimize, maximize, close, responsive input, and keyboard
  shortcuts. Add snapping where supported; workspaces can come later.
- The lock/sign-in screen should show clock/date, user identity, selected wallpaper, power choices,
  and network/accessibility status only when supported. Authentication must use the real account
  system.
- Ship Nexora Browser, Files or the available file-management interface, Settings, Terminal,
  system utilities, and other existing apps that actually work. Hide or clearly label unfinished
  features; do not use fake controls.
- Settings should keep the requested categories: System, Personalization, Network & Internet,
  Devices, Apps, Accounts, Privacy & Security, Accessibility, Date & Language, Updates & Recovery,
  and About Nexora OS. Follow Windows-like organization while retaining Nexora identity; controls
  must map to implemented OS functionality.
- Hide irrelevant unfinished features or explain their unavailability. Do not expose nonfunctional
  controls as real.
- Start with manual/offline updates. Add update checks only with a real updater, require approval,
  and defer automatic updates until recovery and verification are reliable.
- Provide Restart, Shut Down, diagnostic-log export, and reset of supported preferences. Defer
  boot repair/factory reset until safe.
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

**Should-have:** installer visual polish, recovery/diagnostics, repair only where safe.

**Later:** dual boot, advanced partitioning, multiple users/Guest, automatic updates, full recovery,
and broader hardware compatibility.

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
