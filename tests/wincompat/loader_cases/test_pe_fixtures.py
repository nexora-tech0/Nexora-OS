#!/usr/bin/env python3
"""Host-only fixtures for the PE32+ header checks documented in README.md."""

import struct
import unittest
from dataclasses import dataclass
from typing import Callable


DOS_SIZE = 64
NT_OFFSET = 0x80
FILE_HEADER_SIZE = 20
OPTIONAL_HEADER_SIZE = 240
SECTION_HEADER_SIZE = 40
SECTION_TABLE = NT_OFFSET + 4 + FILE_HEADER_SIZE + OPTIONAL_HEADER_SIZE
FILE_SIZE = 0x600

SUCCESS = "STATUS_SUCCESS"
INVALID_NOT_MZ = "STATUS_INVALID_IMAGE_NOT_MZ"
INVALID_FORMAT = "STATUS_INVALID_IMAGE_FORMAT"


def put_u16(image: bytearray, offset: int, value: int) -> None:
    struct.pack_into("<H", image, offset, value)


def put_u32(image: bytearray, offset: int, value: int) -> None:
    struct.pack_into("<I", image, offset, value)


def put_u64(image: bytearray, offset: int, value: int) -> None:
    struct.pack_into("<Q", image, offset, value)


def make_supported_pe() -> bytearray:
    image = bytearray(FILE_SIZE)
    image[0:2] = b"MZ"
    put_u32(image, 0x3C, NT_OFFSET)
    image[NT_OFFSET:NT_OFFSET + 4] = b"PE\0\0"

    file_header = NT_OFFSET + 4
    put_u16(image, file_header, 0x8664)
    put_u16(image, file_header + 2, 2)
    put_u16(image, file_header + 16, OPTIONAL_HEADER_SIZE)
    put_u16(image, file_header + 18, 0x0002)

    optional = file_header + FILE_HEADER_SIZE
    put_u16(image, optional, 0x020B)
    put_u32(image, optional + 16, 0x1000)
    put_u64(image, optional + 24, 0x0000000140000000)
    put_u32(image, optional + 32, 0x1000)
    put_u32(image, optional + 36, 0x200)
    put_u32(image, optional + 56, 0x3000)
    put_u32(image, optional + 60, 0x200)
    put_u16(image, optional + 68, 3)
    put_u32(image, optional + 108, 16)

    section_headers = (
        (b".text", 0x20, 0x1000, 0x200, 0x200, 0x60000020),
        (b".data", 0x20, 0x2000, 0x200, 0x400, 0xC0000040),
    )
    for index, (name, virtual_size, virtual_address, raw_size, raw_offset, flags) in enumerate(section_headers):
        offset = SECTION_TABLE + index * SECTION_HEADER_SIZE
        struct.pack_into(
            "<8sIIIIIIHHI",
            image,
            offset,
            name,
            virtual_size,
            virtual_address,
            raw_size,
            raw_offset,
            0,
            0,
            0,
            0,
            flags,
        )
    return image


@dataclass(frozen=True)
class Fixture:
    name: str
    description: str
    expected: str
    mutate: Callable[[bytearray], bytearray]


def edit(offset: int, width: int, value: int) -> Callable[[bytearray], bytearray]:
    def apply(image: bytearray) -> bytearray:
        {2: put_u16, 4: put_u32, 8: put_u64}[width](image, offset, value)
        return image

    return apply


def truncate(length: int) -> Callable[[bytearray], bytearray]:
    return lambda image: image[:length]


def mutate_section(field_offset: int, value: int) -> Callable[[bytearray], bytearray]:
    return edit(SECTION_TABLE + field_offset, 4, value)


FIXTURES = (
    Fixture("supported_two_sections", "AMD64 PE32+ headers with executable .text and writable .data sections", SUCCESS, lambda image: image),
    Fixture("truncated_dos_header", "File ends before the 64-byte DOS header", INVALID_NOT_MZ, truncate(63)),
    Fixture("bad_dos_magic", "DOS header does not start with MZ", INVALID_NOT_MZ, edit(0, 2, 0)),
    Fixture("negative_nt_offset", "e_lfanew is negative", INVALID_FORMAT, edit(0x3C, 4, 0xFFFFFFFF)),
    Fixture("unaligned_nt_offset", "e_lfanew is not four-byte aligned", INVALID_FORMAT, edit(0x3C, 4, NT_OFFSET + 1)),
    Fixture("truncated_nt_headers", "File ends before the fixed PE and PE32+ headers", INVALID_FORMAT, truncate(NT_OFFSET + 100)),
    Fixture("bad_pe_signature", "NT signature is not PE\\0\\0", INVALID_FORMAT, lambda image: image.__setitem__(slice(NT_OFFSET, NT_OFFSET + 4), b"NOPE") or image),
    Fixture("wrong_machine", "COFF machine is not AMD64", INVALID_FORMAT, edit(NT_OFFSET + 4, 2, 0x014C)),
    Fixture("not_executable", "COFF executable-image characteristic is absent", INVALID_FORMAT, edit(NT_OFFSET + 4 + 18, 2, 0)),
    Fixture("pe32_not_pe32_plus", "Optional-header magic is PE32 rather than PE32+", INVALID_FORMAT, edit(NT_OFFSET + 4 + FILE_HEADER_SIZE, 2, 0x010B)),
    Fixture("optional_header_too_short", "Declared optional header cannot contain all declared data directories", INVALID_FORMAT, edit(NT_OFFSET + 4 + 16, 2, 0x70)),
    Fixture("zero_sections", "COFF section count is zero", INVALID_FORMAT, edit(NT_OFFSET + 4 + 2, 2, 0)),
    Fixture("too_many_sections", "COFF section count exceeds the validator limit of 96", INVALID_FORMAT, edit(NT_OFFSET + 4 + 2, 2, 97)),
    Fixture("truncated_section_table", "File ends before both declared section headers", INVALID_FORMAT, truncate(SECTION_TABLE + SECTION_HEADER_SIZE)),
    Fixture("section_alignment_too_small", "Section alignment is below the 4096-byte page size", INVALID_FORMAT, edit(NT_OFFSET + 4 + FILE_HEADER_SIZE + 32, 4, 0x200)),
    Fixture("section_alignment_not_page_multiple", "Section alignment is not a page-size multiple", INVALID_FORMAT, edit(NT_OFFSET + 4 + FILE_HEADER_SIZE + 32, 4, 0x1800)),
    Fixture("zero_image_size", "SizeOfImage is zero", INVALID_FORMAT, edit(NT_OFFSET + 4 + FILE_HEADER_SIZE + 56, 4, 0)),
    Fixture("oversized_image", "SizeOfImage exceeds the 256 MiB validator limit", INVALID_FORMAT, edit(NT_OFFSET + 4 + FILE_HEADER_SIZE + 56, 4, 0x10000001)),
    Fixture("misaligned_image_base", "ImageBase is not 64 KiB aligned", INVALID_FORMAT, edit(NT_OFFSET + 4 + FILE_HEADER_SIZE + 24, 8, 0x0000000140000001)),
    Fixture("image_base_below_user_range", "ImageBase is below the first user address", INVALID_FORMAT, edit(NT_OFFSET + 4 + FILE_HEADER_SIZE + 24, 8, 0)),
    Fixture("image_range_past_user_top", "ImageBase plus SizeOfImage exceeds the user address-space limit", INVALID_FORMAT, edit(NT_OFFSET + 4 + FILE_HEADER_SIZE + 24, 8, 0x00007FFFFFFFF000)),
    Fixture("headers_past_file", "SizeOfHeaders exceeds file size", INVALID_FORMAT, edit(NT_OFFSET + 4 + FILE_HEADER_SIZE + 60, 4, FILE_SIZE + 1)),
    Fixture("headers_before_section_table_end", "SizeOfHeaders does not cover the section table", INVALID_FORMAT, edit(NT_OFFSET + 4 + FILE_HEADER_SIZE + 60, 4, 0x100)),
    Fixture("zero_entry_point", "AddressOfEntryPoint is zero", INVALID_FORMAT, edit(NT_OFFSET + 4 + FILE_HEADER_SIZE + 16, 4, 0)),
    Fixture("entry_point_past_image", "AddressOfEntryPoint is outside SizeOfImage", INVALID_FORMAT, edit(NT_OFFSET + 4 + FILE_HEADER_SIZE + 16, 4, 0x3000)),
    Fixture("misaligned_section_rva", "Section VirtualAddress is not aligned to SectionAlignment", INVALID_FORMAT, mutate_section(12, 0x1100)),
    Fixture("section_before_headers", "Section VirtualAddress starts inside the headers", INVALID_FORMAT, mutate_section(12, 0)),
    Fixture("section_past_image", "Section virtual extent exceeds SizeOfImage", INVALID_FORMAT, mutate_section(12, 0x3000)),
    Fixture("raw_section_past_file", "Section raw-data extent exceeds file size", INVALID_FORMAT, mutate_section(20, FILE_SIZE - 0x100)),
)


def validate_header_contract(image: bytes) -> tuple[str, dict[str, int]]:
    """Model the structural predicates in ps/peimage.c; does not call production C."""
    info: dict[str, int] = {}
    if len(image) < DOS_SIZE or image[0:2] != b"MZ":
        return INVALID_NOT_MZ, info

    nt_offset = struct.unpack_from("<i", image, 0x3C)[0]
    minimum_nt_size = 4 + FILE_HEADER_SIZE + OPTIONAL_HEADER_SIZE
    if nt_offset <= 0 or nt_offset & 3 or nt_offset > len(image) or len(image) - nt_offset < minimum_nt_size:
        return INVALID_FORMAT, info
    if image[nt_offset:nt_offset + 4] != b"PE\0\0":
        return INVALID_FORMAT, info

    file_header = nt_offset + 4
    machine, section_count = struct.unpack_from("<HH", image, file_header)
    optional_size = struct.unpack_from("<H", image, file_header + 16)[0]
    characteristics = struct.unpack_from("<H", image, file_header + 18)[0]
    optional = file_header + FILE_HEADER_SIZE
    if machine != 0x8664 or not characteristics & 0x0002:
        return INVALID_FORMAT, info
    if struct.unpack_from("<H", image, optional)[0] != 0x020B:
        return INVALID_FORMAT, info

    directory_count = min(struct.unpack_from("<I", image, optional + 108)[0], 16)
    if optional_size < 112 + directory_count * 8 or not 1 <= section_count <= 96:
        return INVALID_FORMAT, info
    section_table = nt_offset + 4 + FILE_HEADER_SIZE + optional_size
    if section_table + section_count * SECTION_HEADER_SIZE > len(image):
        return INVALID_FORMAT, info

    entry_point = struct.unpack_from("<I", image, optional + 16)[0]
    image_base = struct.unpack_from("<Q", image, optional + 24)[0]
    section_alignment = struct.unpack_from("<I", image, optional + 32)[0]
    image_size = struct.unpack_from("<I", image, optional + 56)[0]
    headers_size = struct.unpack_from("<I", image, optional + 60)[0]
    if section_alignment < 4096 or section_alignment & 4095:
        return INVALID_FORMAT, info
    if (
        image_size == 0
        or image_size > 256 * 1024 * 1024
        or image_base & 0xFFFF
        or image_base < 0x10000
        or image_base + image_size > 0x0000800000000000
    ):
        return INVALID_FORMAT, info
    if headers_size > image_size or headers_size > len(image) or headers_size < section_table + section_count * SECTION_HEADER_SIZE:
        return INVALID_FORMAT, info
    if entry_point == 0 or entry_point >= image_size:
        return INVALID_FORMAT, info

    for index in range(section_count):
        offset = section_table + index * SECTION_HEADER_SIZE
        virtual_size, virtual_address, raw_size, raw_offset = struct.unpack_from("<IIII", image, offset + 8)
        virtual_end = virtual_address + (virtual_size if virtual_size else raw_size)
        if (
            virtual_address & (section_alignment - 1)
            or virtual_address < headers_size
            or virtual_end > image_size
        ):
            return INVALID_FORMAT, info
        if raw_size and raw_offset + raw_size > len(image):
            return INVALID_FORMAT, info

    info = {
        "machine": machine,
        "sections": section_count,
        "entry_point": entry_point,
        "image_base": image_base,
        "image_size": image_size,
    }
    return SUCCESS, info


class PeFixtureTests(unittest.TestCase):
    def assert_fixture(self, fixture: Fixture) -> None:
        result, info = validate_header_contract(fixture.mutate(make_supported_pe()))
        self.assertEqual(result, fixture.expected, fixture.description)
        if fixture.expected == SUCCESS:
            self.assertEqual(info["machine"], 0x8664)
            self.assertEqual(info["sections"], 2)
            self.assertEqual(info["entry_point"], 0x1000)
            self.assertEqual(info["image_base"], 0x0000000140000000)
            self.assertEqual(info["image_size"], 0x3000)
        else:
            self.assertEqual(info, {})


def fixture_test(fixture: Fixture) -> Callable[[PeFixtureTests], None]:
    def test(self: PeFixtureTests) -> None:
        self.assert_fixture(fixture)

    return test


for _fixture in FIXTURES:
    setattr(PeFixtureTests, "test_" + _fixture.name, fixture_test(_fixture))


if __name__ == "__main__":
    unittest.main(verbosity=2)
