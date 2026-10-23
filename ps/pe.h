/*
 * Nexora OS - ps/pe.h
 * PE32+ on-disk structures used by the image validator and loader. Written from the published
 * Microsoft PE/COFF format specification (field names follow the spec), not copied from an SDK.
 */
#ifndef PE_H
#define PE_H

#include "hzbase.h"

#define PE_DOS_MAGIC 0x5A4D            /* "MZ" */
#define PE_NT_SIGNATURE 0x00004550     /* "PE\0\0" */
#define PE_OPTIONAL_MAGIC_PE32PLUS 0x20B
#define PE_MACHINE_AMD64 0x8664

#define PE_FILE_RELOCS_STRIPPED 0x0001
#define PE_FILE_EXECUTABLE_IMAGE 0x0002
#define PE_FILE_DLL 0x2000

#define PE_DLLCHAR_DYNAMIC_BASE 0x0040
#define PE_DLLCHAR_NX_COMPAT 0x0100

#define PE_DIRECTORY_EXPORT 0
#define PE_DIRECTORY_IMPORT 1
#define PE_DIRECTORY_BASERELOC 5
#define PE_DIRECTORY_TLS 9
#define PE_DIRECTORY_COUNT 16

#define PE_SCN_CNT_UNINITIALIZED_DATA 0x00000080u
#define PE_SCN_MEM_DISCARDABLE 0x02000000u
#define PE_SCN_MEM_EXECUTE 0x20000000u
#define PE_SCN_MEM_READ 0x40000000u
#define PE_SCN_MEM_WRITE 0x80000000u

#define PE_REL_BASED_ABSOLUTE 0
#define PE_REL_BASED_DIR64 10

typedef struct HZ_PACKED _PE_DOS_HEADER {
    uint16_t Magic;
    uint8_t Unused[58];
    int32_t NtHeaderOffset; /* e_lfanew */
} PE_DOS_HEADER;

typedef struct HZ_PACKED _PE_FILE_HEADER {
    uint16_t Machine;
    uint16_t NumberOfSections;
    uint32_t TimeDateStamp;
    uint32_t PointerToSymbolTable;
    uint32_t NumberOfSymbols;
    uint16_t SizeOfOptionalHeader;
    uint16_t Characteristics;
} PE_FILE_HEADER;

typedef struct HZ_PACKED _PE_DATA_DIRECTORY {
    uint32_t VirtualAddress;
    uint32_t Size;
} PE_DATA_DIRECTORY;

typedef struct HZ_PACKED _PE_OPTIONAL_HEADER64 {
    uint16_t Magic;
    uint8_t MajorLinkerVersion;
    uint8_t MinorLinkerVersion;
    uint32_t SizeOfCode;
    uint32_t SizeOfInitializedData;
    uint32_t SizeOfUninitializedData;
    uint32_t AddressOfEntryPoint;
    uint32_t BaseOfCode;
    uint64_t ImageBase;
    uint32_t SectionAlignment;
    uint32_t FileAlignment;
    uint16_t MajorOperatingSystemVersion;
    uint16_t MinorOperatingSystemVersion;
    uint16_t MajorImageVersion;
    uint16_t MinorImageVersion;
    uint16_t MajorSubsystemVersion;
    uint16_t MinorSubsystemVersion;
    uint32_t Win32VersionValue;
    uint32_t SizeOfImage;
    uint32_t SizeOfHeaders;
    uint32_t CheckSum;
    uint16_t Subsystem;
    uint16_t DllCharacteristics;
    uint64_t SizeOfStackReserve;
    uint64_t SizeOfStackCommit;
    uint64_t SizeOfHeapReserve;
    uint64_t SizeOfHeapCommit;
    uint32_t LoaderFlags;
    uint32_t NumberOfRvaAndSizes;
    PE_DATA_DIRECTORY DataDirectory[PE_DIRECTORY_COUNT];
} PE_OPTIONAL_HEADER64;

typedef struct HZ_PACKED _PE_SECTION_HEADER {
    char Name[8];
    uint32_t VirtualSize;
    uint32_t VirtualAddress;
    uint32_t SizeOfRawData;
    uint32_t PointerToRawData;
    uint32_t PointerToRelocations;
    uint32_t PointerToLinenumbers;
    uint16_t NumberOfRelocations;
    uint16_t NumberOfLinenumbers;
    uint32_t Characteristics;
} PE_SECTION_HEADER;

typedef struct HZ_PACKED _PE_BASE_RELOCATION {
    uint32_t PageRva;
    uint32_t BlockSize; /* including this header */
} PE_BASE_RELOCATION;

_Static_assert(sizeof(PE_OPTIONAL_HEADER64) == 240, "PE32+ optional header is 240 bytes");
_Static_assert(sizeof(PE_SECTION_HEADER) == 40, "section header is 40 bytes");

/* Located headers of an image validated by PsValidatePeImage. */
typedef struct _PE_HEADERS {
    const PE_FILE_HEADER *File;
    const PE_OPTIONAL_HEADER64 *Optional;
    const PE_SECTION_HEADER *Sections;
} PE_HEADERS;

STATUS PspLocatePeHeaders(const void *Image, size_t Size, PE_HEADERS *Headers);

static inline const PE_DATA_DIRECTORY *PeDirectory(const PE_HEADERS *Headers, unsigned Index)
{
    static const PE_DATA_DIRECTORY Empty = {0, 0};
    return Index < Headers->Optional->NumberOfRvaAndSizes ? &Headers->Optional->DataDirectory[Index] : &Empty;
}

#endif /* PE_H */
