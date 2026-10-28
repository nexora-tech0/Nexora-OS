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

static bool PspIsPowerOfTwo32(uint32_t Value)
{
    return Value != 0 && (Value & (Value - 1u)) == 0;
}

static bool PspPeFileRvaRange(const PE_HEADERS *Headers, size_t FileSize, uint64_t Rva,
                              uint64_t Length, uint64_t *FileOffset)
{
    if (!Headers || !Headers->Optional || !FileOffset || Rva > Headers->Optional->SizeOfImage ||
        Length > Headers->Optional->SizeOfImage - Rva)
        return false;
    if (Rva < Headers->Optional->SizeOfHeaders) {
        if (Length > Headers->Optional->SizeOfHeaders - Rva || Rva > FileSize ||
            Length > FileSize - Rva)
            return false;
        *FileOffset = Rva;
        return true;
    }
    for (uint32_t Index = 0; Index < Headers->File->NumberOfSections; ++Index) {
        const PE_SECTION_HEADER *Section = &Headers->Sections[Index];
        uint64_t OffsetInSection = Rva - Section->VirtualAddress;
        if (Rva < Section->VirtualAddress || OffsetInSection > Section->SizeOfRawData ||
            Length > Section->SizeOfRawData - OffsetInSection)
            continue;
        uint64_t Offset = (uint64_t)Section->PointerToRawData + OffsetInSection;
        if (Offset > FileSize || Length > FileSize - Offset)
            return false;
        *FileOffset = Offset;
        return true;
    }
    return false;
}

static bool PspPeReadExportString(const uint8_t *File, size_t FileSize, const PE_HEADERS *Headers,
                                 uint32_t Rva, uint64_t LimitRva, char *Output, size_t Capacity)
{
    if (!Rva || !Output || Capacity < 2)
        return false;
    for (size_t Index = 0; Index + 1 < Capacity; ++Index) {
        uint64_t FileOffset;
        if ((uint64_t)Rva + Index >= LimitRva ||
            !PspPeFileRvaRange(Headers, FileSize, (uint64_t)Rva + Index, 1, &FileOffset))
            break;
        uint8_t Character = File[FileOffset];
        if (!Character) {
            Output[Index] = 0;
            return Index != 0;
        }
        if (Character < 0x20 || Character > 0x7E)
            break;
        Output[Index] = (char)Character;
    }
    Output[0] = 0;
    return false;
}

STATUS PeFindFileExport(const void *FileBuffer, size_t FileSize, const PE_HEADERS *Headers,
                        const char *Name, uint16_t Ordinal, bool ByOrdinal,
                        uint32_t *FunctionRva, char *Forwarder, size_t ForwarderCapacity)
{
    if (!FileBuffer || !Headers || !Headers->File || !Headers->Optional || !FunctionRva ||
        (ByOrdinal ? Name != NULL : (!Name || !Name[0])) ||
        (Forwarder && ForwarderCapacity < 2))
        return STATUS_INVALID_PARAMETER;
    *FunctionRva = 0;
    if (Forwarder && ForwarderCapacity)
        Forwarder[0] = 0;

    const PE_DATA_DIRECTORY *Directory = PeDirectory(Headers, PE_DIRECTORY_EXPORT);
    if (!Directory->VirtualAddress || Directory->Size < sizeof(PE_EXPORT_DIRECTORY) ||
        (uint64_t)Directory->VirtualAddress + Directory->Size > Headers->Optional->SizeOfImage)
        return STATUS_NOT_FOUND;
    uint64_t DirectoryEnd = (uint64_t)Directory->VirtualAddress + Directory->Size;
    uint64_t Offset;
    if (!PspPeFileRvaRange(Headers, FileSize, Directory->VirtualAddress,
                           sizeof(PE_EXPORT_DIRECTORY), &Offset))
        return STATUS_INVALID_IMAGE_FORMAT;

    PE_EXPORT_DIRECTORY Export;
    memcpy(&Export, (const uint8_t *)FileBuffer + Offset, sizeof(Export));
    if (!Export.NumberOfFunctions || Export.NumberOfFunctions > 65536u ||
        Export.NumberOfNames > Export.NumberOfFunctions ||
        Export.NumberOfNames > 65536u ||
        (uint64_t)Export.Base + Export.NumberOfFunctions > UINT32_MAX + 1ull)
        return STATUS_INVALID_IMAGE_FORMAT;

    uint32_t FunctionIndex = UINT32_MAX;
    uint64_t ArrayOffset;
    if (ByOrdinal) {
        if ((uint32_t)Ordinal < Export.Base ||
            (uint32_t)Ordinal - Export.Base >= Export.NumberOfFunctions)
            return STATUS_NOT_FOUND;
        FunctionIndex = (uint32_t)Ordinal - Export.Base;
    } else {
        if (!PspPeFileRvaRange(Headers, FileSize, Export.AddressOfNames,
                               (uint64_t)Export.NumberOfNames * sizeof(uint32_t), &ArrayOffset))
            return STATUS_INVALID_IMAGE_FORMAT;
        uint64_t OrdinalArrayOffset;
        if (!PspPeFileRvaRange(Headers, FileSize, Export.AddressOfNameOrdinals,
                               (uint64_t)Export.NumberOfNames * sizeof(uint16_t), &OrdinalArrayOffset))
            return STATUS_INVALID_IMAGE_FORMAT;
        size_t RequestedLength = 0;
        while (Name[RequestedLength]) {
            if (++RequestedLength >= PE_EXPORT_NAME_MAX)
                return STATUS_NAME_TOO_LONG;
        }
        for (uint32_t Index = 0; Index < Export.NumberOfNames; ++Index) {
            uint32_t NameRva;
            uint16_t NameOrdinal;
            memcpy(&NameRva, (const uint8_t *)FileBuffer + ArrayOffset + (uint64_t)Index * sizeof(NameRva),
                   sizeof(NameRva));
            memcpy(&NameOrdinal, (const uint8_t *)FileBuffer + OrdinalArrayOffset +
                                  (uint64_t)Index * sizeof(NameOrdinal), sizeof(NameOrdinal));
            if (NameOrdinal >= Export.NumberOfFunctions)
                return STATUS_INVALID_IMAGE_FORMAT;
            char Candidate[PE_EXPORT_NAME_MAX];
            if (!PspPeReadExportString(FileBuffer, FileSize, Headers, NameRva,
                                       Headers->Optional->SizeOfImage, Candidate, sizeof(Candidate)))
                return STATUS_INVALID_IMAGE_FORMAT;
            if (strlen(Candidate) == RequestedLength && memcmp(Candidate, Name, RequestedLength) == 0) {
                FunctionIndex = NameOrdinal;
                break;
            }
        }
        if (FunctionIndex == UINT32_MAX)
            return STATUS_NOT_FOUND;
    }

    if (!PspPeFileRvaRange(Headers, FileSize, Export.AddressOfFunctions,
                           (uint64_t)Export.NumberOfFunctions * sizeof(uint32_t), &ArrayOffset))
        return STATUS_INVALID_IMAGE_FORMAT;
    uint32_t TargetRva;
    memcpy(&TargetRva, (const uint8_t *)FileBuffer + ArrayOffset +
                       (uint64_t)FunctionIndex * sizeof(TargetRva), sizeof(TargetRva));
    if (!TargetRva)
        return STATUS_NOT_FOUND;
    if (TargetRva >= Headers->Optional->SizeOfImage)
        return STATUS_INVALID_IMAGE_FORMAT;

    if ((uint64_t)TargetRva >= Directory->VirtualAddress && (uint64_t)TargetRva < DirectoryEnd) {
        if (!Forwarder || !ForwarderCapacity ||
            !PspPeReadExportString(FileBuffer, FileSize, Headers, TargetRva, DirectoryEnd,
                                   Forwarder, ForwarderCapacity))
            return Forwarder ? STATUS_INVALID_IMAGE_FORMAT : STATUS_BUFFER_TOO_SMALL;
    }
    *FunctionRva = TargetRva;
    return STATUS_SUCCESS;
}

static bool PspPeReadImageString(const uint8_t *Image, size_t ImageSize, uint32_t Rva,
                                uint64_t LimitRva, char *Output, size_t Capacity)
{
    if (!Rva || !Output || Capacity < 2)
        return false;
    for (size_t Index = 0; Index + 1 < Capacity; ++Index) {
        uint64_t Position = (uint64_t)Rva + Index;
        if (Position >= LimitRva || Position >= ImageSize) {
            Output[0] = 0;
            return false;
        }
        uint8_t Character = Image[Position];
        if (!Character) {
            Output[Index] = 0;
            return Index != 0;
        }
        if (Character < 0x20 || Character > 0x7E) {
            Output[0] = 0;
            return false;
        }
        Output[Index] = (char)Character;
    }
    Output[0] = 0;
    return false;
}

STATUS PeFindImageExport(const void *ImageBuffer, size_t ImageSize, const PE_HEADERS *Headers,
                         const char *Name, uint16_t Ordinal, bool ByOrdinal,
                         uint32_t *FunctionRva, char *Forwarder, size_t ForwarderCapacity)
{
    if (!ImageBuffer || !Headers || !Headers->Optional || !FunctionRva ||
        ImageSize < Headers->Optional->SizeOfImage ||
        (ByOrdinal ? Name != NULL : (!Name || !Name[0])) ||
        (Forwarder && ForwarderCapacity < 2))
        return STATUS_INVALID_PARAMETER;
    *FunctionRva = 0;
    if (Forwarder && ForwarderCapacity)
        Forwarder[0] = 0;
    const PE_DATA_DIRECTORY *Directory = PeDirectory(Headers, PE_DIRECTORY_EXPORT);
    if (!Directory->VirtualAddress || Directory->Size < sizeof(PE_EXPORT_DIRECTORY) ||
        (uint64_t)Directory->VirtualAddress + Directory->Size > ImageSize)
        return STATUS_NOT_FOUND;
    uint64_t DirectoryEnd = (uint64_t)Directory->VirtualAddress + Directory->Size;
    const uint8_t *Image = (const uint8_t *)ImageBuffer;
    PE_EXPORT_DIRECTORY Export;
    memcpy(&Export, Image + Directory->VirtualAddress, sizeof(Export));
    if (!Export.NumberOfFunctions || Export.NumberOfFunctions > 65536u ||
        Export.NumberOfNames > Export.NumberOfFunctions || Export.NumberOfNames > 65536u ||
        (uint64_t)Export.Base + Export.NumberOfFunctions > UINT32_MAX + 1ull)
        return STATUS_INVALID_IMAGE_FORMAT;

    uint32_t FunctionIndex = UINT32_MAX;
    if (ByOrdinal) {
        if ((uint32_t)Ordinal < Export.Base ||
            (uint32_t)Ordinal - Export.Base >= Export.NumberOfFunctions)
            return STATUS_NOT_FOUND;
        FunctionIndex = (uint32_t)Ordinal - Export.Base;
    } else {
        uint64_t NamesBytes = (uint64_t)Export.NumberOfNames * sizeof(uint32_t);
        uint64_t OrdinalsBytes = (uint64_t)Export.NumberOfNames * sizeof(uint16_t);
        if (Export.AddressOfNames > ImageSize || NamesBytes > ImageSize - Export.AddressOfNames ||
            Export.AddressOfNameOrdinals > ImageSize || OrdinalsBytes > ImageSize - Export.AddressOfNameOrdinals)
            return STATUS_INVALID_IMAGE_FORMAT;
        size_t RequestedLength = 0;
        while (Name[RequestedLength]) {
            if (++RequestedLength >= PE_EXPORT_NAME_MAX)
                return STATUS_NAME_TOO_LONG;
        }
        for (uint32_t Index = 0; Index < Export.NumberOfNames; ++Index) {
            uint32_t NameRva;
            uint16_t NameOrdinal;
            memcpy(&NameRva, Image + Export.AddressOfNames + (uint64_t)Index * sizeof(NameRva), sizeof(NameRva));
            memcpy(&NameOrdinal, Image + Export.AddressOfNameOrdinals +
                   (uint64_t)Index * sizeof(NameOrdinal), sizeof(NameOrdinal));
            if (NameOrdinal >= Export.NumberOfFunctions)
                return STATUS_INVALID_IMAGE_FORMAT;
            char Candidate[PE_EXPORT_NAME_MAX];
            if (!PspPeReadImageString(Image, ImageSize, NameRva, ImageSize, Candidate, sizeof(Candidate)))
                return STATUS_INVALID_IMAGE_FORMAT;
            if (strlen(Candidate) == RequestedLength && memcmp(Candidate, Name, RequestedLength) == 0) {
                FunctionIndex = NameOrdinal;
                break;
            }
        }
        if (FunctionIndex == UINT32_MAX)
            return STATUS_NOT_FOUND;
    }

    uint64_t FunctionsBytes = (uint64_t)Export.NumberOfFunctions * sizeof(uint32_t);
    if (Export.AddressOfFunctions > ImageSize || FunctionsBytes > ImageSize - Export.AddressOfFunctions)
        return STATUS_INVALID_IMAGE_FORMAT;
    uint32_t TargetRva;
    memcpy(&TargetRva, Image + Export.AddressOfFunctions +
           (uint64_t)FunctionIndex * sizeof(TargetRva), sizeof(TargetRva));
    if (!TargetRva)
        return STATUS_NOT_FOUND;
    if (TargetRva >= ImageSize)
        return STATUS_INVALID_IMAGE_FORMAT;
    if ((uint64_t)TargetRva >= Directory->VirtualAddress && (uint64_t)TargetRva < DirectoryEnd) {
        if (!Forwarder || !ForwarderCapacity)
            return STATUS_BUFFER_TOO_SMALL;
        if (!PspPeReadImageString(Image, ImageSize, TargetRva, DirectoryEnd,
                                  Forwarder, ForwarderCapacity))
            return STATUS_INVALID_IMAGE_FORMAT;
    }
    *FunctionRva = TargetRva;
    return STATUS_SUCCESS;
}

STATUS PspLocatePeHeaders(const void *Image, size_t Size, PE_HEADERS *Headers)
{
    const uint8_t *Base = (const uint8_t *)Image;
    if (!Image || !Headers || Size < sizeof(PE_DOS_HEADER))
        return STATUS_INVALID_IMAGE_NOT_MZ;

    const PE_DOS_HEADER *Dos = (const PE_DOS_HEADER *)Base;
    if (Dos->Magic != PE_DOS_MAGIC)
        return STATUS_INVALID_IMAGE_NOT_MZ;
    if (Dos->NtHeaderOffset <= 0 || (Dos->NtHeaderOffset & 3))
        return STATUS_INVALID_IMAGE_FORMAT;

    size_t NtOffset = (size_t)Dos->NtHeaderOffset;
    if (NtOffset > Size || Size - NtOffset < 4 + sizeof(PE_FILE_HEADER) + sizeof(uint16_t))
        return STATUS_INVALID_IMAGE_FORMAT;

    uint32_t Signature;
    memcpy(&Signature, Base + NtOffset, sizeof(Signature));
    if (Signature != PE_NT_SIGNATURE)
        return STATUS_INVALID_IMAGE_FORMAT;

    size_t FileHeaderOffset = NtOffset + 4;
    if (FileHeaderOffset > Size || Size - FileHeaderOffset < sizeof(PE_FILE_HEADER))
        return STATUS_INVALID_IMAGE_FORMAT;

    const PE_FILE_HEADER *File = (const PE_FILE_HEADER *)(Base + FileHeaderOffset);
    if (File->Machine == 0x014Cu)
        return STATUS_NOT_SUPPORTED; /* PE32 x86 needs the separate 32-bit execution ABI. */
    if (File->Machine != PE_MACHINE_AMD64 || !(File->Characteristics & PE_FILE_EXECUTABLE_IMAGE))
        return STATUS_INVALID_IMAGE_FORMAT;
    if (File->SizeOfOptionalHeader < sizeof(PE_OPTIONAL_HEADER64))
        return STATUS_INVALID_IMAGE_FORMAT;
    if (File->NumberOfSections == 0 || File->NumberOfSections > PE_MAX_SECTIONS)
        return STATUS_INVALID_IMAGE_FORMAT;

    size_t OptionalHeaderOffset = FileHeaderOffset + sizeof(PE_FILE_HEADER);
    size_t OptionalHeaderEnd = OptionalHeaderOffset + File->SizeOfOptionalHeader;
    if (OptionalHeaderEnd < OptionalHeaderOffset || OptionalHeaderEnd > Size)
        return STATUS_INVALID_IMAGE_FORMAT;

    const PE_OPTIONAL_HEADER64 *Opt = (const PE_OPTIONAL_HEADER64 *)(Base + OptionalHeaderOffset);
    if (Opt->Magic != PE_OPTIONAL_MAGIC_PE32PLUS)
        return STATUS_INVALID_IMAGE_FORMAT;
    if (!PspIsPowerOfTwo32(Opt->FileAlignment) || Opt->FileAlignment < 512u)
        return STATUS_INVALID_IMAGE_FORMAT;
    if (!PspIsPowerOfTwo32(Opt->SectionAlignment) || Opt->SectionAlignment < PAGE_SIZE ||
        Opt->SectionAlignment < Opt->FileAlignment)
        return STATUS_INVALID_IMAGE_FORMAT;

    uint32_t Directories = Opt->NumberOfRvaAndSizes < PE_DIRECTORY_COUNT ? Opt->NumberOfRvaAndSizes : PE_DIRECTORY_COUNT;
    if (File->SizeOfOptionalHeader < offsetof(PE_OPTIONAL_HEADER64, DataDirectory) +
                                      (size_t)Directories * sizeof(PE_DATA_DIRECTORY))
        return STATUS_INVALID_IMAGE_FORMAT;

    size_t SectionTable = OptionalHeaderEnd;
    size_t SectionTableSize = (size_t)File->NumberOfSections * sizeof(PE_SECTION_HEADER);
    if (SectionTable > Size || SectionTableSize > Size - SectionTable)
        return STATUS_INVALID_IMAGE_FORMAT;

    /* Layout: page-aligned sections inside SizeOfImage, raw data inside the file and never overlapping. */
    if (Opt->SizeOfImage == 0 || Opt->SizeOfImage > PE_MAX_IMAGE_SIZE ||
        (Opt->ImageBase & 0xFFFF) || Opt->ImageBase < MM_USER_MIN ||
        (uint64_t)Opt->ImageBase + Opt->SizeOfImage > MM_USER_TOP)
        return STATUS_INVALID_IMAGE_FORMAT;
    if (Opt->SizeOfHeaders < SectionTable || Opt->SizeOfHeaders > Opt->SizeOfImage || Opt->SizeOfHeaders > Size)
        return STATUS_INVALID_IMAGE_FORMAT;
    if (SectionTable + SectionTableSize > Opt->SizeOfHeaders)
        return STATUS_INVALID_IMAGE_FORMAT;
    if (Opt->AddressOfEntryPoint == 0 || Opt->AddressOfEntryPoint >= Opt->SizeOfImage)
        return STATUS_INVALID_IMAGE_FORMAT;

    const PE_SECTION_HEADER *Sections = (const PE_SECTION_HEADER *)(Base + SectionTable);
    for (uint32_t i = 0; i < File->NumberOfSections; i++) {
        const PE_SECTION_HEADER *S = &Sections[i];
        uint64_t VirtualSpan = S->VirtualSize ? S->VirtualSize : S->SizeOfRawData;
        uint64_t VirtualEnd = (uint64_t)S->VirtualAddress + VirtualSpan;
        if ((S->VirtualAddress & (Opt->SectionAlignment - 1)) || S->VirtualAddress < Opt->SizeOfHeaders ||
            VirtualEnd > Opt->SizeOfImage)
            return STATUS_INVALID_IMAGE_FORMAT;
        if (S->SizeOfRawData != 0) {
            if ((S->PointerToRawData & (Opt->FileAlignment - 1)) ||
                S->PointerToRawData < Opt->SizeOfHeaders ||
                (uint64_t)S->PointerToRawData + S->SizeOfRawData > Size)
                return STATUS_INVALID_IMAGE_FORMAT;
        }
        for (uint32_t j = 0; j < i; j++) {
            const PE_SECTION_HEADER *Prev = &Sections[j];
            uint64_t PrevStart = Prev->VirtualAddress;
            uint64_t PrevEnd = PrevStart + (Prev->VirtualSize ? Prev->VirtualSize : Prev->SizeOfRawData);
            uint64_t CurrStart = S->VirtualAddress;
            uint64_t CurrEnd = CurrStart + VirtualSpan;
            if (CurrStart < PrevEnd && PrevStart < CurrEnd)
                return STATUS_INVALID_IMAGE_FORMAT;
            if (Prev->SizeOfRawData != 0 && S->SizeOfRawData != 0) {
                uint64_t PrevRawStart = Prev->PointerToRawData;
                uint64_t PrevRawEnd = PrevRawStart + Prev->SizeOfRawData;
                uint64_t CurrRawStart = S->PointerToRawData;
                uint64_t CurrRawEnd = CurrRawStart + S->SizeOfRawData;
                if (CurrRawStart < PrevRawEnd && PrevRawStart < CurrRawEnd)
                    return STATUS_INVALID_IMAGE_FORMAT;
            }
        }
    }

    for (uint32_t i = 0; i < Directories; i++) {
        const PE_DATA_DIRECTORY *Directory = &Opt->DataDirectory[i];
        if (Directory->Size == 0)
            continue;
        if (i == 4) {
            if (Directory->VirtualAddress > Size || Directory->Size > Size - Directory->VirtualAddress)
                return STATUS_INVALID_IMAGE_FORMAT;
            continue;
        }
        if ((uint64_t)Directory->VirtualAddress + Directory->Size > Opt->SizeOfImage)
            return STATUS_INVALID_IMAGE_FORMAT;
    }

    Headers->File = File;
    Headers->Optional = Opt;
    Headers->Sections = Sections;
    return STATUS_SUCCESS;
}

STATUS PspLocateMappedPeHeaders(const void *Image, size_t Size, PE_HEADERS *Headers)
{
    if (!Image || !Headers || Size < sizeof(PE_DOS_HEADER))
        return STATUS_INVALID_IMAGE_FORMAT;
    const uint8_t *Base = (const uint8_t *)Image;
    const PE_DOS_HEADER *Dos = (const PE_DOS_HEADER *)Base;
    if (Dos->Magic != PE_DOS_MAGIC || Dos->NtHeaderOffset < 0)
        return STATUS_INVALID_IMAGE_FORMAT;
    uint64_t NtOffset = (uint32_t)Dos->NtHeaderOffset;
    if (NtOffset > Size || Size - NtOffset < sizeof(uint32_t) + sizeof(PE_FILE_HEADER))
        return STATUS_INVALID_IMAGE_FORMAT;
    uint32_t Signature;
    memcpy(&Signature, Base + NtOffset, sizeof(Signature));
    if (Signature != PE_NT_SIGNATURE)
        return STATUS_INVALID_IMAGE_FORMAT;
    const PE_FILE_HEADER *File = (const PE_FILE_HEADER *)(Base + NtOffset + sizeof(Signature));
    if (File->Machine != PE_MACHINE_AMD64 || !(File->Characteristics & PE_FILE_EXECUTABLE_IMAGE) ||
        File->NumberOfSections == 0 || File->NumberOfSections > PE_MAX_SECTIONS ||
        File->SizeOfOptionalHeader < sizeof(PE_OPTIONAL_HEADER64))
        return STATUS_INVALID_IMAGE_FORMAT;
    uint64_t OptionalOffset = NtOffset + sizeof(Signature) + sizeof(PE_FILE_HEADER);
    uint64_t OptionalEnd = OptionalOffset + File->SizeOfOptionalHeader;
    if (OptionalEnd < OptionalOffset || OptionalEnd > Size)
        return STATUS_INVALID_IMAGE_FORMAT;
    const PE_OPTIONAL_HEADER64 *Optional = (const PE_OPTIONAL_HEADER64 *)(Base + OptionalOffset);
    if (Optional->Magic != PE_OPTIONAL_MAGIC_PE32PLUS || Optional->SizeOfImage > Size ||
        Optional->SizeOfImage == 0 || Optional->SizeOfHeaders > Optional->SizeOfImage)
        return STATUS_INVALID_IMAGE_FORMAT;
    uint64_t SectionBytes = (uint64_t)File->NumberOfSections * sizeof(PE_SECTION_HEADER);
    if (OptionalEnd > Optional->SizeOfHeaders ||
        SectionBytes > Optional->SizeOfHeaders - OptionalEnd)
        return STATUS_INVALID_IMAGE_FORMAT;
    Headers->File = File;
    Headers->Optional = Optional;
    Headers->Sections = (const PE_SECTION_HEADER *)(Base + OptionalEnd);
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
