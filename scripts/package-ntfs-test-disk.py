#!/usr/bin/env python3
"""Wrap a formatted NTFS image in a minimal MBR disk for disposable QEMU tests."""

import argparse
import os
import struct

SECTOR_SIZE = 512
PARTITION_START = 2048
TRAILING_SECTORS = 2048


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source", help="formatted NTFS volume image")
    parser.add_argument("destination", help="raw MBR disk image to create")
    args = parser.parse_args()

    volume_bytes = os.path.getsize(args.source)
    if volume_bytes < 16 * 1024 * 1024 or volume_bytes % SECTOR_SIZE:
        parser.error("source must be a sector-aligned NTFS volume of at least 16 MiB")
    with open(args.source, "rb") as volume:
        boot = volume.read(SECTOR_SIZE)
    if boot[3:11] != b"NTFS    " or boot[510:512] != b"\x55\xAA":
        parser.error("source does not contain an NTFS volume boot record")
    bps = struct.unpack_from("<H", boot, 11)[0]
    total_sectors = struct.unpack_from("<Q", boot, 40)[0]
    if bps != SECTOR_SIZE or not total_sectors or total_sectors > volume_bytes // SECTOR_SIZE:
        parser.error("source NTFS geometry does not match its image size")
    if total_sectors > 0xFFFFFFFF:
        parser.error("MBR test fixture cannot represent this partition size")

    os.makedirs(os.path.dirname(os.path.abspath(args.destination)), exist_ok=True)
    disk_sectors = PARTITION_START + total_sectors + TRAILING_SECTORS
    mbr = bytearray(SECTOR_SIZE)
    entry = 446
    mbr[entry + 4] = 0x07  # Microsoft basic data / NTFS partition type.
    struct.pack_into("<II", mbr, entry + 8, PARTITION_START, total_sectors)
    mbr[510:512] = b"\x55\xAA"
    with open(args.destination, "wb") as disk, open(args.source, "rb") as volume:
        disk.truncate(disk_sectors * SECTOR_SIZE)
        disk.write(mbr)
        disk.seek(PARTITION_START * SECTOR_SIZE)
        while block := volume.read(1024 * 1024):
            disk.write(block)
    print(f"ntfs-test-disk: {args.destination} ({disk_sectors * SECTOR_SIZE} bytes)")


if __name__ == "__main__":
    main()
