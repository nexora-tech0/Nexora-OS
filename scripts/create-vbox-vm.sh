#!/usr/bin/env bash
# Create (or refresh) the VirtualBox VM "Nexora OS" and attach the live ISO.
#
#   scripts/create-vbox-vm.sh [ISO] [--start] [--recreate] [--name NAME]
#
#   (no flags)   create the VM if missing; otherwise swap in the freshly built ISO
#   --start      start the VM afterwards (GUI)
#   --headless   start the VM afterwards without a window (serial log only)
#   --recreate   DELETE the existing VM, including its virtual disk, and create it again
#   --test-disk IMG  (re)attach a fresh copy of the raw test disk IMG (scripts/mkdisk.sh,
#                `make testdisk`) as SATA port 2; without the flag an attached test disk is kept
#   --wipe-disk  replace the 16 GB system disk (port 0) with a new, empty one (to install again)
#   --boot-disk  leave the DVD drive empty, so the VM starts from its disk (after Setup); without
#                it the freshly built ISO is attached
#   --cpus N     give the VM N processors (1 to 8; M11); without the flag the count is kept
#
# Installing (M10): start with the ISO, run "Install Nexora OS" from Start, power off, then
# `create-vbox-vm.sh --boot-disk --start` boots the installed system.
#
# VM: Other/Unknown (64-bit), EFI, 2048 MB RAM, 1 CPU (see --cpus), VMSVGA with 64 MB VRAM,
# PS/2 keyboard and mouse, an E1000 NAT network adapter, SATA AHCI with a 16 GB VDI (port 0),
# a DVD drive with the ISO (port 1) and optionally the FAT32 test disk (port 2), COM1 logged
# to <VM folder>/serial-com1.log, EFI GOP resolution
# 1920x1080, paravirt provider "minimal", TSC mode RealTSCOffset (M11).
#
# Works from Linux/macOS (VBoxManage) and from WSL or Git Bash on Windows (VBoxManage.exe).
# The ISO is copied into the VM folder so VirtualBox never locks build/nexora-os.iso.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
ISO="$ROOT/build/nexora-os.iso"
VM="Nexora OS"
START=0
RECREATE=0
TEST_DISK=""
WIPE_DISK=0
BOOT_DISK=0
CPUS=""

while [ $# -gt 0 ]; do
    case "$1" in
        --start) START=1 ;;
        --headless) START=2 ;;
        --recreate) RECREATE=1 ;;
        --name) VM="$2"; shift ;;
        --test-disk) TEST_DISK="$2"; shift ;;
        --wipe-disk) WIPE_DISK=1 ;;
        --boot-disk) BOOT_DISK=1 ;;
        --cpus) CPUS="$2"; shift ;;
        -h|--help) sed -n '2,29p' "$0"; exit 0 ;;
        *) ISO="$1" ;;
    esac
    shift
done

die() { echo "create-vbox-vm: $*" >&2; exit 1; }
log() { echo "[vbox] $*"; }

case "$CPUS" in ""|[1-8]) ;; *) die "--cpus takes 1 to 8" ;; esac
[ "$BOOT_DISK" = 1 ] || [ -f "$ISO" ] || die "$ISO not found (run 'make iso' first)"

# ---- Find VBoxManage and a host-path converter ---------------------------------------------
VBM=""
to_host() { printf '%s' "$1"; }
from_host() { printf '%s' "$1"; }
if command -v VBoxManage >/dev/null 2>&1; then
    VBM="VBoxManage"
fi
if [ -z "$VBM" ]; then
    for c in "/mnt/c/Program Files/Oracle/VirtualBox/VBoxManage.exe" \
             "/mnt/e/VIRTUALBOX/VBoxManage.exe" \
             "/c/Program Files/Oracle/VirtualBox/VBoxManage.exe" \
             "/cygdrive/c/Program Files/Oracle/VirtualBox/VBoxManage.exe"; do
        if [ -x "$c" ]; then VBM="$c"; break; fi
    done
fi
[ -n "$VBM" ] || die "VBoxManage not found; install VirtualBox 7.x"

case "$VBM" in
    *.exe)
        if command -v wslpath >/dev/null 2>&1; then
            to_host() { wslpath -w "$1"; }
            from_host() { wslpath -u "$1"; }
        elif command -v cygpath >/dev/null 2>&1; then
            to_host() { cygpath -w "$1"; }
            from_host() { cygpath -u "$1"; }
        fi
        ;;
esac

vbm() { "$VBM" "$@"; }
vm_exists() { vbm showvminfo "$VM" --machinereadable >/dev/null 2>&1; }
vm_state() { vbm showvminfo "$VM" --machinereadable | tr -d '\r' | sed -n 's/^VMState="\(.*\)"$/\1/p'; }
vm_dir() {
    local cfg
    cfg="$(vbm showvminfo "$VM" --machinereadable | tr -d '\r' | sed -n 's/^CfgFile="\(.*\)"$/\1/p')"
    cfg="${cfg//\\\\/\\}"
    dirname "$(from_host "$cfg")"
}

log "using $VBM ($(vbm --version | tr -d '\r'))"

if vm_exists; then
    state="$(vm_state)"
    case "$state" in
        poweroff|aborted|saved) ;;
        *) die "VM '$VM' is $state; power it off first" ;;
    esac
    if [ "$RECREATE" = 1 ]; then
        log "deleting VM '$VM' and its disks (--recreate)"
        vbm unregistervm "$VM" --delete
    fi
fi

if ! vm_exists; then
    log "creating VM '$VM'"
    vbm createvm --name "$VM" --ostype Other_64 --register >/dev/null
    vbm modifyvm "$VM" \
        --firmware efi64 \
        --memory 2048 \
        --cpus 1 \
        --ioapic on \
        --rtc-use-utc on \
        --graphicscontroller vmsvga \
        --vram 64 \
        --mouse ps2 \
        --keyboard ps2 \
        --audio-enabled off \
        --usb-ohci off --usb-ehci off --usb-xhci off \
        --nic1 nat --nictype1 82540EM --cableconnected1 on \
        --boot1 dvd --boot2 disk --boot3 none --boot4 none
    vbm setextradata "$VM" VBoxInternal2/EfiGraphicsResolution 1920x1080

    dir="$(vm_dir)"
    vbm storagectl "$VM" --name SATA --add sata --controller IntelAhci --portcount 2 --bootable on
    vbm createmedium disk --filename "$(to_host "$dir/$VM.vdi")" --size 16384 --format VDI >/dev/null
    vbm storageattach "$VM" --storagectl SATA --port 0 --device 0 --type hdd \
        --medium "$(to_host "$dir/$VM.vdi")"
    log "created 16 GB disk $dir/$VM.vdi"
fi

dir="$(vm_dir)"

# COM1 -> file (re-applied every run so the path follows the VM folder). The "minimal" paravirt
# provider publishes exact TSC/APIC frequencies (CPUID 0x40000010) so the kernel need not
# calibrate against VirtualBox's catch-up virtual clock. Override with HZ_VBOX_PARAVIRT=none|kvm|....
vbm modifyvm "$VM" --uart1 0x3F8 4 --uart-type1 16550A --uart-mode1 file "$(to_host "$dir/serial-com1.log")" \
    --paravirt-provider "${HZ_VBOX_PARAVIRT:-minimal}"
# The guest TSC is the host's (plus an offset). With VirtualBox's default TSC mode on the Hyper-V
# (NEM) backend and more than one processor, the guest TSC only advanced while guest code ran, so
# the processors' clocks drifted apart and a processor spinning in user mode stalled the others'
# timers (M11). Override with HZ_VBOX_TSCMODE=VirtTSCEmulated|RealTSCOffset|Dynamic, or "" to unset.
vbm setextradata "$VM" VBoxInternal/TM/TSCMode ${HZ_VBOX_TSCMODE-RealTSCOffset}
if [ -n "$CPUS" ]; then
    vbm modifyvm "$VM" --cpus "$CPUS"
    log "$CPUS processor(s)"
fi

# A new, empty system disk on port 0.
if [ "$WIPE_DISK" = 1 ]; then
    vbm storageattach "$VM" --storagectl SATA --port 0 --device 0 --medium none >/dev/null 2>&1 || true
    vbm closemedium disk "$(to_host "$dir/$VM.vdi")" --delete >/dev/null 2>&1 || true
    rm -f "$dir/$VM.vdi"
    vbm createmedium disk --filename "$(to_host "$dir/$VM.vdi")" --size 16384 --format VDI >/dev/null
    vbm storageattach "$VM" --storagectl SATA --port 0 --device 0 --type hdd --medium "$(to_host "$dir/$VM.vdi")"
    log "replaced the system disk with an empty 16 GB $dir/$VM.vdi"
fi

# Swap in a private copy of the ISO (or leave the drive empty to start from the disk).
vbm storageattach "$VM" --storagectl SATA --port 1 --device 0 --type dvddrive --medium emptydrive \
    >/dev/null 2>&1 || true
vbm closemedium dvd "$(to_host "$dir/nexora-os-live.iso")" >/dev/null 2>&1 || true
if [ "$BOOT_DISK" = 1 ]; then
    log "DVD drive left empty: the VM starts from its disk"
else
    cp "$ISO" "$dir/nexora-os-live.iso"
    vbm storageattach "$VM" --storagectl SATA --port 1 --device 0 --type dvddrive \
        --medium "$(to_host "$dir/nexora-os-live.iso")"
    log "attached $(to_host "$dir/nexora-os-live.iso")"
fi

# Optional FAT32 test disk on port 2, converted from the raw image (a fresh copy every time the
# flag is given; the kernel's file system tests clean up after themselves between boots).
if [ -n "$TEST_DISK" ]; then
    [ -f "$TEST_DISK" ] || die "$TEST_DISK not found (run 'make testdisk')"
    vbm storagectl "$VM" --name SATA --portcount 3 >/dev/null
    vbm storageattach "$VM" --storagectl SATA --port 2 --device 0 --medium none >/dev/null 2>&1 || true
    vbm closemedium disk "$(to_host "$dir/$VM-test.vdi")" --delete >/dev/null 2>&1 || true
    rm -f "$dir/$VM-test.vdi"
    cp "$TEST_DISK" "$dir/$VM-test.img"
    vbm convertfromraw "$(to_host "$dir/$VM-test.img")" "$(to_host "$dir/$VM-test.vdi")" --format VDI >/dev/null
    rm -f "$dir/$VM-test.img"
    vbm storageattach "$VM" --storagectl SATA --port 2 --device 0 --type hdd \
        --medium "$(to_host "$dir/$VM-test.vdi")"
    log "attached test disk $(to_host "$dir/$VM-test.vdi")"
fi
log "serial log: $(to_host "$dir/serial-com1.log")"

if [ "$START" != 0 ]; then
    : > "$dir/serial-com1.log"
    log "starting '$VM'"
    vbm startvm "$VM" --type "$([ "$START" = 2 ] && echo headless || echo gui)" >/dev/null
fi
log "done"
