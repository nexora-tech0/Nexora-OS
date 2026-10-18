# Windows host notes

- **Inventory date:** 2026-10-18
- **Host:** Windows 11 Pro, x64 (`10.0.26200`)
- **Workspace:** initially empty; Git repository initialized after inspection.

## Verified tools

| Tool | Result |
| --- | --- |
| Git | Available (`2.55.0.windows.3`) |
| GitHub CLI | Available (`2.97.0`) |
| Python | Available (`3.14.8`) |
| WSL | Not installed |
| QEMU (`qemu-system-x86_64`) | Not found |
| Rust/Cargo | Not found |
| CMake/Ninja/C/C++ compiler/Make | Not found |
| Docker/Podman | Not found |
| Arch `pacman` | Not found |

This inventory only reflects commands discoverable on the inspected host; it
does not test Linux builds or VM acceleration.

## Supported next step

Use a dedicated Arch Linux machine or VM as the image build host. Install
`archiso` and the VM/runtime tools there, then validate image generation and
boot using the Archiso documentation. QEMU/KVM acceleration, nested
virtualization, available disk space, and RAM must be checked on the selected
host before committing to that setup.

The current Windows host cannot build Archiso images or run the planned QEMU
tests with the detected tools. WSL is absent, and installing WSL, QEMU, or Linux
development packages has deliberately not been attempted. Do not install WSL or
modify Windows optional features without the user's authorization. Do not use
this machine as a target for disk or bootloader experiments.

## Reproducibility

Record the exact Arch Linux version, package versions, image profile revision,
build command, and artifact checksum for every tested image. Pinning and update
policy will be specified with the first image profile rather than guessed in
this host note.
