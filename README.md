# Nexora OS

Nexora OS is a planned x86_64 Linux distribution with a custom Wayland desktop
environment. The project is at the **foundation** stage; it does not yet produce
a bootable image or a usable desktop.

## Project status

- **Host inspected:** Windows 11 Pro, x64.
- **Repository:** connected to the public GitHub repository
  [`nexora-tech0/Nexora-OS`](https://github.com/nexora-tech0/Nexora-OS); remote
  repository was empty when checked, and these local changes have not been
  pushed.
- **Image workflow direction:** Arch Linux with archiso, to be exercised on an
  Arch Linux build host.
- **Compositor:** undecided. A Smithay-versus-wlroots prototype and evaluation
  are required before committing.
- **License:** MIT for Nexora-owned work; third-party dependencies retain their
  own licenses. See [LICENSE](LICENSE).
- **Build, boot, and hardware support:** not verified.

Do not install or flash this project onto a host machine. The first image work
must use a disposable virtual machine and a verified image artifact.

## Architecture at a glance

Keep the distribution (base packages, image profile, system configuration and
installer) separate from Nexora desktop components. The desktop is planned as
Wayland clients and services, with Qt 6/QML for shell interfaces where suitable.
The compositor owns display, input, focus and window-management responsibilities;
the shell must not impersonate or bypass it. Use system interfaces and
authorization mechanisms for privileged operations rather than invoking
arbitrary root commands from the UI.

See [the architecture overview](docs/architecture/overview.md), [architecture
decisions](docs/decisions/), and [development setup](docs/development/).

## Development status and next milestone

**Milestone 1 — project foundation** is in progress. The immediate next
implementation milestone is a reproducible Archiso development image that boots
in a VM into a minimal Wayland session. Before that work starts, provision an
Arch Linux build environment, install the documented tools there, and complete
the compositor evaluation gate. No GUI or image implementation is claimed yet.

## Development platform

This checkout was inspected on Windows 11. Git, GitHub CLI, and Python are
available; WSL, QEMU, Rust/Cargo, CMake, Ninja, C/C++ compilers, Docker, and
Podman were not detected. Archiso's documented image-building environment is
Arch Linux, so use a dedicated Arch Linux machine or VM rather than treating
this Windows checkout as an image builder. See
[Windows host notes](docs/development/windows-host.md) for the verified
inventory and setup path.

## Reproducible setup (once an Arch Linux build host is available)

1. Clone this repository on the Arch Linux build host.
2. Install `archiso`, `git`, `qemu-full`, and the build/lint tools listed in
   [the development setup](docs/development/arch-linux.md).
3. Build only from a project-owned Archiso profile and write outputs to
   ignored build directories.
4. Verify the image checksum and boot it in QEMU before testing on any physical
   hardware.

The detailed image and VM commands will be committed with the first profile;
they are intentionally not represented as working commands before that profile
and host have been tested.

## Project policies

- [Contributing](CONTRIBUTING.md)
- [Security reporting](SECURITY.md)
- [Testing strategy](docs/testing/strategy.md)
- [MIT License](LICENSE)

This is an early engineering project, not a production-ready distribution. No
security certification, release integrity, hardware compatibility, or working
desktop is claimed.
