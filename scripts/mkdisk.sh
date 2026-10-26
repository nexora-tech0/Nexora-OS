#!/usr/bin/env bash
# Builds the Nexora OS test disk image used by `make test` and the VirtualBox test disk:
#
#   64 MiB raw image, GPT:
#     partition 1: 48 MiB basic data, FAT32 labeled HZTEST (512-byte clusters, so files and
#                  directories span many clusters), with known files the kernel self-test checks
#     partition 2: 4 MiB "Nexora OS test scratch" (GPT type 7A1F5C3E-2B4D-4E8A-9F60-485A54455354),
#                  the only place the raw disk self-test writes to
#
# Usage: scripts/mkdisk.sh OUTPUT.img     (needs sgdisk, mtools and python3)
set -euo pipefail

out="$1"
mkdir -p "$(dirname "$out")"
# Work in a native Linux directory (faster than WSL drvfs for the many small mtools writes).
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT
tmp="$work/disk.img"
truncate -s 64M "$tmp"

sgdisk -o \
    -n 1:2048:+48M -t 1:EBD0A0A2-B9E5-4433-87C0-68B6B72699C7 -c 1:"Nexora OS test data" \
    -n 2:0:+4M -t 2:7A1F5C3E-2B4D-4E8A-9F60-485A54455354 -c 2:"Nexora OS test scratch" \
    "$tmp" >/dev/null

part="$tmp@@$((2048 * 512))"
export MTOOLS_SKIP_CHECK=1
mformat -i "$part" -F -c 1 -v HZTEST -T $((48 * 1024 * 2)) -h 64 -s 32 ::

printf 'Nexora OS FAT32 test volume\n' > "$work/README.TXT"
printf 'long names work\n' > "$work/long.txt"
python3 -c 'import sys; sys.stdout.buffer.write(bytes(((i * 31) ^ (i >> 7)) & 0xFF for i in range(300000)))' \
    > "$work/pattern.bin"

mcopy -i "$part" "$work/README.TXT" ::/README.TXT
mmd -i "$part" ::/Docs ::/Data
mcopy -i "$part" "$work/long.txt" "::/Docs/A long file name for testing.txt"
mcopy -i "$part" "$work/pattern.bin" ::/Data/pattern.bin

cp "$tmp" "$out"
echo "testdisk: $out"
