#!/usr/bin/env python3
"""Build the kernel symbol table module (hzkrnl.sym) from the linked kernel.

    mksym.py <llvm-nm> <hzkrnl.elf> <out.sym>

Format (little endian), read by ke/symbols.c:
    char     Magic[8]      "HZSYMTB1"
    uint32   Count
    uint32   StringsSize
    uint64   TextStart, TextEnd
    { uint64 Address; uint32 NameOffset; uint32 Reserved; } [Count]   sorted by Address
    char     Strings[StringsSize]                                     NUL-terminated names
Only code symbols are kept. C++ and Rust names are demangled; Rust hashes are dropped.
"""

import re
import struct
import subprocess
import sys

MAX_NAME = 120


def main():
    nm, elf, out = sys.argv[1:4]
    text = subprocess.run([nm, "--defined-only", "-n", "-C", elf], check=True,
                          capture_output=True, text=True).stdout

    by_address = {}
    text_start = text_end = None
    for line in text.splitlines():
        parts = line.split(" ", 2)
        if len(parts) != 3:
            continue
        address, kind, name = int(parts[0], 16), parts[1], parts[2]
        if name == "__TextStart":
            text_start = address
        elif name == "__TextEnd":
            text_end = address
        # Skip linker-script markers (__TextStart, __KernelEnd, ...), but keep real functions
        # such as __rustc::rust_begin_unwind or __cxa_pure_virtual.
        if kind not in "tTwW" or re.match(r"__[A-Z]", name):
            continue
        name = re.sub(r"::h[0-9a-f]{16}$", "", name)
        if len(name) > MAX_NAME:
            name = name[:MAX_NAME - 3] + "..."
        # Prefer global names over local labels at the same address.
        if address not in by_address or (kind in "TW" and by_address[address][1] not in "TW"):
            by_address[address] = (name, kind)

    if text_start is None or text_end is None:
        sys.exit("mksym: __TextStart/__TextEnd not found in " + elf)

    strings = bytearray()
    entries = []
    for address in sorted(by_address):
        entries.append((address, len(strings)))
        strings += by_address[address][0].encode("utf-8", "replace") + b"\0"

    blob = bytearray(b"HZSYMTB1")
    blob += struct.pack("<IIQQ", len(entries), len(strings), text_start, text_end)
    for address, offset in entries:
        blob += struct.pack("<QII", address, offset, 0)
    blob += strings
    with open(out, "wb") as f:
        f.write(blob)
    print(f"mksym: {len(entries)} symbols, {len(blob)} bytes -> {out}", file=sys.stderr)


if __name__ == "__main__":
    main()
