#!/usr/bin/env bash
# Boot a Nexora OS ISO in QEMU with OVMF (UEFI firmware).
#
#   scripts/run-qemu.sh [ISO|none] [extra qemu args...]
#
# "none" boots without a CD (from QEMU_DISK, for example an installed disk image).
#
# Environment:
#   QEMU_DISPLAY   gtk | sdl | cocoa | none | ...   (default: gtk/cocoa if a GUI is available,
#                  otherwise "none" with a VNC server on 127.0.0.1:5900)
#   QEMU_SERIAL    QEMU -serial backend (default: stdio)
#   QEMU_VARS      OVMF variable store to use (default: build/ovmf-vars.fd, created on demand)
#   QEMU_MEM       RAM in MiB (default: 2048)
#   QEMU_SMP       processors (default: 1; the smoke tests also run cases with 4)
#   QEMU_ACCEL     kvm | hvf | tcg (default: best available)
#   QEMU_DISK      raw disk image attached as SATA port 0 of the q35 AHCI controller (optional;
#                  `make run` passes build/test/disk.img). The ISO is on port 2.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
ISO="${1:-$ROOT/build/nexora-os.iso}"
[ $# -gt 0 ] && shift
if [ "$ISO" != "none" ]; then
    [ -f "$ISO" ] || { echo "run-qemu: $ISO not found (run 'make iso' first)" >&2; exit 1; }
fi

QEMU="${QEMU:-qemu-system-x86_64}"
command -v "$QEMU" >/dev/null 2>&1 || { echo "run-qemu: $QEMU not found" >&2; exit 1; }

# ---- Locate OVMF (code + vars template) across distributions and Homebrew ------------------
ovmf_code=""
ovmf_vars_template=""
brew_share=""
if command -v brew >/dev/null 2>&1; then brew_share="$(brew --prefix)/share/qemu"; fi
for pair in \
    "/usr/share/OVMF/OVMF_CODE_4M.fd:/usr/share/OVMF/OVMF_VARS_4M.fd" \
    "/usr/share/OVMF/OVMF_CODE.fd:/usr/share/OVMF/OVMF_VARS.fd" \
    "/usr/share/edk2/x64/OVMF_CODE.4m.fd:/usr/share/edk2/x64/OVMF_VARS.4m.fd" \
    "/usr/share/edk2/ovmf/OVMF_CODE.fd:/usr/share/edk2/ovmf/OVMF_VARS.fd" \
    "/usr/share/qemu/edk2-x86_64-code.fd:/usr/share/qemu/edk2-i386-vars.fd" \
    "$brew_share/edk2-x86_64-code.fd:$brew_share/edk2-i386-vars.fd"; do
    code="${pair%%:*}"
    vars="${pair##*:}"
    if [ -f "$code" ] && [ -f "$vars" ]; then
        ovmf_code="$code"
        ovmf_vars_template="$vars"
        break
    fi
done
[ -n "$ovmf_code" ] || { echo "run-qemu: OVMF not found (install the 'ovmf' package)" >&2; exit 1; }

VARS="${QEMU_VARS:-$ROOT/build/ovmf-vars.fd}"
if [ ! -f "$VARS" ]; then
    mkdir -p "$(dirname "$VARS")"
    cp "$ovmf_vars_template" "$VARS"
fi

# ---- Acceleration ----------------------------------------------------------------------------
accel_args=()
case "${QEMU_ACCEL:-auto}" in
    kvm) accel_args=(-accel kvm -cpu host) ;;
    hvf) accel_args=(-accel hvf -cpu host) ;;
    tcg) accel_args=(-accel tcg -cpu max) ;;
    auto)
        if [ -r /dev/kvm ] && [ -w /dev/kvm ]; then
            accel_args=(-accel kvm -cpu host)
        elif [ "$(uname -s)" = "Darwin" ] && [ "$(uname -m)" = "x86_64" ]; then
            accel_args=(-accel hvf -cpu host)
        else
            accel_args=(-accel tcg -cpu max)
        fi
        ;;
esac

# ---- Display ---------------------------------------------------------------------------------
display_args=()
display="${QEMU_DISPLAY:-}"
if [ -z "$display" ]; then
    if [ "$(uname -s)" = "Darwin" ]; then
        display="cocoa"
    elif [ -n "${DISPLAY:-}${WAYLAND_DISPLAY:-}" ]; then
        display="gtk"
    else
        display="none"
        display_args=(-vnc 127.0.0.1:0)
        echo "run-qemu: no GUI detected; connect a VNC viewer to 127.0.0.1:5900" >&2
    fi
fi
display_args=(-display "$display" "${display_args[@]}")

disk_args=()
if [ -n "${QEMU_DISK:-}" ]; then
    disk_args=(-drive "file=$QEMU_DISK,if=none,id=hd0,format=raw" -device ide-hd,drive=hd0,bus=ide.0)
fi
media_args=(-cdrom "$ISO" -boot d)
if [ "$ISO" = "none" ]; then
    [ -n "${QEMU_DISK:-}" ] || { echo "run-qemu: 'none' needs QEMU_DISK" >&2; exit 1; }
    media_args=(-boot c)
fi

exec "$QEMU" \
    -machine q35 \
    -m "${QEMU_MEM:-2048}" \
    -smp "${QEMU_SMP:-1}" \
    "${accel_args[@]}" \
    -drive "if=pflash,format=raw,unit=0,file=$ovmf_code,readonly=on" \
    -drive "if=pflash,format=raw,unit=1,file=$VARS" \
    "${media_args[@]}" \
    "${disk_args[@]}" \
    -vga std \
    -serial "${QEMU_SERIAL:-stdio}" \
    -no-reboot \
    "${display_args[@]}" \
    "$@"
