# Architecture overview

**Status:** initial proposal; implementation and compatibility are not yet
verified.

## Principles

1. Deliver a working, recoverable Linux desktop before optimizing its visual
   polish.
2. Keep upstream distribution packages and Nexora-owned components separable.
3. Prefer documented Linux interfaces and existing system services over
   privileged custom daemons.
4. Keep compositor responsibilities separate from shell UI responsibilities.
5. Make builds repeatable and test image artifacts in VMs before hardware.
6. Record security, licensing, and compatibility evidence for shipped
   dependencies.

## Proposed layers

| Layer | Responsibility | Initial direction |
| --- | --- | --- |
| Image and distribution | Live image, package selection, system configuration, update and installation strategy | Arch Linux packages and an owned archiso profile; image work must run on a validated Arch Linux build host |
| Linux session | Session startup, systemd user services, D-Bus integration, portals, session and power coordination | Reuse standard system services and protocols; define Nexora units only when their ownership and privilege needs are clear |
| Wayland compositor | Display/input backends, protocol implementation, focus, surfaces, window management, workspaces | Evaluate Smithay and wlroots in a focused prototype; no selection yet |
| Desktop shell | Panel, dock, launcher, notifications, settings, wallpaper, accessibility | Qt 6/QML clients and services where they fit; do not couple shell UI to compositor internals |
| Applications and integrations | Files, settings, monitoring, software management, browser and code products | Prefer existing Linux applications; integrate separate Nexora products only after verifying their actual source and distribution details |

## Compositor boundary

The compositor is a security-sensitive system component and must be implemented
and tested independently from shell surfaces. Shell interfaces should communicate
through supported Wayland protocols and documented session/system interfaces.
They must remain restartable without taking down the session where the protocol
and compositor design permit it. A minimal shell launching is not evidence of a
complete desktop.

The compositor decision is gated on a buildable prototype and documented
comparison. The evaluation criteria are protocol support needed by Qt clients
and target applications, DRM/KMS and input backends, XWayland needs, session
integration, maintainership and release activity, dependency/licensing review,
debugging and test support, and the amount of Nexora-specific code required.
See [ADR 0002](../decisions/0002-compositor-evaluation.md).

## Privilege and configuration

Desktop processes run unprivileged. Privileged operations must use an
appropriate authorization mechanism and a narrow system interface with
validated inputs and explicit error reporting. Never construct arbitrary root
shell commands from UI input. Store user settings in user-owned configuration;
define schemas and migration behavior when settings are implemented.

## Not yet decided

- Final compositor and protocol-extension set.
- Installer and installed-system update design.
- Package-signing and release-key custody.
- Supported hardware baseline and minimum resources.
- Application choices for files, software management, browser, and code.
- Contribution and copyright-ownership process.

Each decision needs a reviewed record before implementation depends on it.
