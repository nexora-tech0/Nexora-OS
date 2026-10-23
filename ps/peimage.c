/*
 * Nexora OS - ps/peimage.c
 * PE32+ image validation. Every offset and size the loader will use is checked against the
 * file here, so ps/peload.c can trust the headers it gets from PspLocatePeHeaders.
 */
#include "pe.h"
#include "mm.h"
#include "ps.h"
#include "rtl.h"

#define PE_MAX_IMAGE_SIZE (256u * 1024 * 1024)
#define PE_MAX_SECTIONS 96

STATUS PspLocatePeHeaders(const void *Image, size_t Size, PE_HEADERS *Headers)
{
    const uint8_t *Base = (const uint8_t *)Image;
    if (!Image || Size < sizeof(PE_DOS_HEADER))
        return STATUS_INVALID_IMAGE_NOT_MZ;

    const PE_DOS_HEADER *Dos = (const PE_DOS_HEADER *)Base;
    if (Dos->Magic != PE_DOS_MAGIC)
        return STATUS_INVALID_IMAGE_NOT_MZ;
    if (Dos->NtHeaderOffset <= 0 || (Dos->NtHeaderOffset & 3))
        return STATUS_INVALID_IMAGE_FORMAT;

    size_t NtOffset = (size_t)Dos->NtHeaderOffset;
    if (NtOffset > Size || Size - NtOffset < 4 + sizeof(PE_FILE_HEADER) + sizeof(PE_OPTIONAL_HEADER64))
        return STATUS_INVALID_IMAGE_FORMAT;

    uint32_t Signature;
    memcpy(&Signature, Base + NtOffset, sizeof(Signature));
    if (Signature != PE_NT_SIGNATURE)
        return STATUS_INVALID_IMAGE_FORMAT;

    const PE_FILE_HEADER *File = (const PE_FILE_HEADER *)(Base + NtOffset + 4);
    const PE_OPTIONAL_HEADER64 *Opt = (const PE_OPTIONAL_HEADER64 *)(File + 1);
    if (File->Machine != PE_MACHINE_AMD64 || !(File->Characteristics & PE_FILE_EXECUTABLE_IMAGE))
        return STATUS_INVALID_IMAGE_FORMAT;
    if (Opt->Magic != PE_OPTIONAL_MAGIC_PE32PLUS)
        return STATUS_INVALID_IMAGE_FORMAT;
    uint32_t Directories = Opt->NumberOfRvaAndSizes < PE_DIRECTORY_COUNT ? Opt->NumberOfRvaAndSizes : PE_DIRECTORY_COUNT;
    if (File->SizeOfOptionalHeader < offsetof(PE_OPTIONAL_HEADER64, DataDirectory) + Directories * sizeof(PE_DATA_DIRECTORY))
        return STATUS_INVALID_IMAGE_FORMAT;
    if (File->NumberOfSections == 0 || File->NumberOfSections > PE_MAX_SECTIONS)
        return STATUS_INVALID_IMAGE_FORMAT;

    size_t SectionTable = NtOffset + 4 + sizeof(PE_FILE_HEADER) + File->SizeOfOptionalHeader;
    if (SectionTable + (size_t)File->NumberOfSections * sizeof(PE_SECTION_HEADER) > Size)
        return STATUS_INVALID_IMAGE_FORMAT;

    /* Layout: page-aligned sections inside SizeOfImage, raw data inside the file. */
    if (Opt->SectionAlignment < PAGE_SIZE || (Opt->SectionAlignment & (PAGE_SIZE - 1)))
        return STATUS_INVALID_IMAGE_FORMAT;
    if (Opt->SizeOfImage == 0 || Opt->SizeOfImage > PE_MAX_IMAGE_SIZE || (Opt->ImageBase & 0xFFFF) ||
        Opt->ImageBase < MM_USER_MIN || Opt->ImageBase + Opt->SizeOfImage > MM_USER_TOP)
        return STATUS_INVALID_IMAGE_FORMAT;
    if (Opt->SizeOfHeaders > Opt->SizeOfImage || Opt->SizeOfHeaders > Size || Opt->SizeOfHeaders < SectionTable)
        return STATUS_INVALID_IMAGE_FORMAT;
    if (Opt->AddressOfEntryPoint == 0 || Opt->AddressOfEntryPoint >= Opt->SizeOfImage)
        return STATUS_INVALID_IMAGE_FORMAT;

    const PE_SECTION_HEADER *Sections = (const PE_SECTION_HEADER *)(Base + SectionTable);
    for (uint32_t i = 0; i < File->NumberOfSections; i++) {
        const PE_SECTION_HEADER *S = &Sections[i];
        uint64_t VirtualEnd = (uint64_t)S->VirtualAddress + (S->VirtualSize ? S->VirtualSize : S->SizeOfRawData);
        if ((S->VirtualAddress & (Opt->SectionAlignment - 1)) || S->VirtualAddress < Opt->SizeOfHeaders ||
            VirtualEnd > Opt->SizeOfImage)
            return STATUS_INVALID_IMAGE_FORMAT;
        if (S->SizeOfRawData && (uint64_t)S->PointerToRawData + S->SizeOfRawData > Size)
            return STATUS_INVALID_IMAGE_FORMAT;
    }

    Headers->File = File;
    Headers->Optional = Opt;
    Headers->Sections = Sections;
    return STATUS_SUCCESS;
}

STATUS PsValidatePeImage(const void *Image, size_t Size, PE_IMAGE_INFO *Info)
{
    PE_HEADERS Headers;
    STATUS Status = PspLocatePeHeaders(Image, Size, &Headers);
    if (!HZ_SUCCESS(Status))
        return Status;
    if (Info) {
        const PE_OPTIONAL_HEADER64 *Opt = Headers.Optional;
        Info->Machine = Headers.File->Machine;
        Info->SectionCount = Headers.File->NumberOfSections;
        Info->Subsystem = Opt->Subsystem;
        Info->Characteristics = Headers.File->Characteristics;
        Info->EntryPointRva = Opt->AddressOfEntryPoint;
        Info->ImageBase = Opt->ImageBase;
        Info->SizeOfImage = Opt->SizeOfImage;
        Info->SizeOfHeaders = Opt->SizeOfHeaders;
        Info->SectionAlignment = Opt->SectionAlignment;
        Info->StackReserve = Opt->SizeOfStackReserve;
        Info->StackCommit = Opt->SizeOfStackCommit;
        Info->Relocatable = (Opt->DllCharacteristics & PE_DLLCHAR_DYNAMIC_BASE) != 0 &&
                            !(Headers.File->Characteristics & PE_FILE_RELOCS_STRIPPED) &&
                            PeDirectory(&Headers, PE_DIRECTORY_BASERELOC)->Size != 0;
    }
    return STATUS_SUCCESS;
}
