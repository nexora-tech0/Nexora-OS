#!/usr/bin/env python3
"""Unpack the Nexora Code Windows NSIS payload onto a QEMU FAT32 test disk.

The setup program is never executed. Requires 7z and mtools.
"""
from __future__ import annotations

import argparse
import os
import shutil
import struct
import subprocess
import tempfile
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
DISK_BYTES = 1024 * 1024 * 1024
PARTITION_START = 2048
SECTOR_BYTES = 512
PARTITION_OFFSET = PARTITION_START * SECTOR_BYTES


def run(arguments: list[str], *, env: dict[str, str] | None = None) -> None:
    subprocess.run(arguments, check=True, env=env)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("installer", type=Path, help="Nexora Code Windows NSIS installer")
    parser.add_argument("output", type=Path, help="destination raw disk image")
    parser.add_argument("--rebuild", action="store_true", help="re-extract even when the disk is current")
    args = parser.parse_args()

    installer = args.installer.resolve()
    output = args.output.resolve()
    if not installer.is_file():
        parser.error(f"installer not found: {installer}")
    output.parent.mkdir(parents=True, exist_ok=True)
    output.parent.resolve().relative_to(ROOT)
    if (not args.rebuild and output.is_file() and output.stat().st_size == DISK_BYTES and
            output.stat().st_mtime >= max(installer.stat().st_mtime, Path(__file__).stat().st_mtime)):
        print(f"nexoracode-disk: reusing {output} ({DISK_BYTES} bytes)")
        return 0

    with tempfile.TemporaryDirectory(prefix="nexora-code-") as temporary:
        work = Path(temporary)
        wrapper = work / "wrapper"
        payload = work / "payload"
        wrapper.mkdir()
        payload.mkdir()
        run(["7z", "x", str(installer), "$PLUGINSDIR/app-64.7z", f"-o{wrapper}", "-y"])
        nested_archive = wrapper / "$PLUGINSDIR" / "app-64.7z"
        if not nested_archive.is_file():
            raise RuntimeError("the NSIS installer did not contain $PLUGINSDIR/app-64.7z")
        run(["7z", "x", str(nested_archive), f"-o{payload}", "-y"])
        if not (payload / "Nexora Code Setup.exe").is_file() or not (payload / "resources" / "app.asar").is_file():
            raise RuntimeError("the extracted release is missing the Nexora Code executable or resources/app.asar")
        (payload / "Nexora Code Setup.exe").rename(payload / "NexoraCode.exe")

        disk = work / "nexoracode-app-disk.img"
        with disk.open("wb") as image:
            image.truncate(DISK_BYTES)
        total_sectors = DISK_BYTES // SECTOR_BYTES
        partition_sectors = total_sectors - PARTITION_START
        mbr = bytearray(SECTOR_BYTES)
        entry = bytearray(16)
        entry[1:4] = b"\xFE\xFF\xFF"
        entry[4] = 0x0C  # FAT32 with LBA addressing
        entry[5:8] = b"\xFE\xFF\xFF"
        struct.pack_into("<II", entry, 8, PARTITION_START, partition_sectors)
        mbr[446:462] = entry
        mbr[510:512] = b"\x55\xAA"
        with disk.open("r+b") as image:
            image.write(mbr)

        partition = f"{disk}@@{PARTITION_OFFSET}"
        tool_env = dict(os.environ)
        tool_env["MTOOLS_SKIP_CHECK"] = "1"
        run(["mformat", "-i", partition, "-F", "-c", "8", "-v", "NEXORACODE",
             "-T", str(partition_sectors), "::"], env=tool_env)
        for entry in sorted(payload.iterdir(), key=lambda item: item.name.casefold()):
            run(["mcopy", "-s", "-i", partition, str(entry), "::/"], env=tool_env)

        shutil.copyfile(disk, output)
    print(f"nexoracode-disk: {output} ({DISK_BYTES} bytes; full app payload included)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
