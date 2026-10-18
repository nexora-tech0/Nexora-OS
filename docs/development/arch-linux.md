# Arch Linux development environment

**Status:** requirements identified; not yet provisioned or tested.

## Build host

Use an Arch Linux machine or VM for the initial Archiso workflow. Upstream
Archiso documents Arch Linux as its supported image-building environment. Keep
build and output directories separate from system paths and source files.

The first setup should install only tools required by an implemented milestone:

- `archiso` for building a project-owned image profile.
- `git` for source control.
- `qemu-full` (or an equivalently validated QEMU package) for disposable VM
  boot tests.
- A shell linter and format/check tools once scripts are added.
- Rust/Cargo or C/C++/Meson/Ninja toolchains only after the compositor decision
  establishes the needed stack.

Exact packages, versions, and installation commands must be recorded and
verified when the image profile and compositor prototype are created. No
dependency installation has been performed from this Windows host.

## Safety gates

1. Confirm the VM has sufficient memory, disk, and virtualization support.
2. Generate images only in a disposable build environment.
3. Write image files only to a project-owned output directory.
4. Calculate and verify a checksum before booting an image.
5. Boot with an isolated QEMU virtual disk and network configuration.
6. Never attach a host physical disk or run installer tests on the host.

## Initial image acceptance

The first image milestone is complete only when an independently reproducible
build produces an artifact, its checksum is recorded, and that artifact boots
in a VM to a usable Wayland session with keyboard and pointer interaction. A
successful `mkarchiso` exit alone is insufficient.
