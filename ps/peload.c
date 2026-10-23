/*
 * Nexora OS - ps/peload.c
 * PE32+ image loader for user processes.
 *
 * The image is assembled in a kernel buffer (headers and sections copied to their virtual
 * addresses, base relocations applied), copied into a MEM_IMAGE region of the new process in
 * one go, and then each section gets its final protection (W^X unless the image asks for a
 * writable code section). Imports are not supported yet: Nexora OS programs are linked
 * statically against hzsys/hzrt, so an import directory means a foreign image.
 */
#include "pe.h"
#include "psp.h"

static uint32_t PspSectionProtection(uint32_t Characteristics)
{
    uint32_t Protection = 0;
    if (Characteristics & PE_SCN_MEM_READ)
        Protection |= MM_PROT_READ;
    if (Characteristics & PE_SCN_MEM_WRITE)
        Protection |= MM_PROT_READWRITE;
    if (Characteristics & PE_SCN_MEM_EXECUTE)
        Protection |= MM_PROT_EXECUTE | MM_PROT_READ;
    return Protection ? Protection : MM_PROT_READ;
}

static STATUS PspApplyRelocations(uint8_t *Image, uint64_t ImageSize, const PE_HEADERS *Headers, int64_t Delta)
{
    const PE_DATA_DIRECTORY *Directory = PeDirectory(Headers, PE_DIRECTORY_BASERELOC);
    if (Directory->Size == 0)
        return STATUS_INVALID_IMAGE_FORMAT; /* must move but cannot */
    if ((uint64_t)Directory->VirtualAddress + Directory->Size > ImageSize)
        return STATUS_INVALID_IMAGE_FORMAT;

    uint32_t Offset = 0, Applied = 0;
    while (Offset + sizeof(PE_BASE_RELOCATION) <= Directory->Size) {
        PE_BASE_RELOCATION Block;
        memcpy(&Block, Image + Directory->VirtualAddress + Offset, sizeof(Block));
        if (Block.BlockSize < sizeof(Block) || Block.BlockSize > Directory->Size - Offset || (Block.BlockSize & 1))
            return STATUS_INVALID_IMAGE_FORMAT;
        uint32_t Count = (Block.BlockSize - sizeof(Block)) / sizeof(uint16_t);
        const uint8_t *Entries = Image + Directory->VirtualAddress + Offset + sizeof(Block);
        for (uint32_t i = 0; i < Count; i++) {
            uint16_t Entry;
            memcpy(&Entry, Entries + i * sizeof(uint16_t), sizeof(Entry));
            unsigned Type = Entry >> 12;
            uint64_t Rva = (uint64_t)Block.PageRva + (Entry & 0xFFF);
            if (Type == PE_REL_BASED_ABSOLUTE)
                continue; /* padding */
            if (Type != PE_REL_BASED_DIR64 || Rva + sizeof(uint64_t) > ImageSize)
                return STATUS_INVALID_IMAGE_FORMAT;
            uint64_t Value;
            memcpy(&Value, Image + Rva, sizeof(Value));
            Value += (uint64_t)Delta;
            memcpy(Image + Rva, &Value, sizeof(Value));
            Applied++;
        }
        Offset += Block.BlockSize;
    }
    DbgPrintEx(DPFLTR_TRACE_LEVEL, "Loader: %u relocations applied (delta %s0x%llx)\n", Applied, Delta < 0 ? "-" : "+",
               (unsigned long long)(Delta < 0 ? -Delta : Delta));
    return STATUS_SUCCESS;
}

STATUS PspMapImage(EPROCESS *Process, const void *File, uint64_t FileSize, uint32_t Flags)
{
    PE_HEADERS Headers;
    STATUS Status = PspLocatePeHeaders(File, FileSize, &Headers);
    if (!HZ_SUCCESS(Status))
        return Status;
    const PE_OPTIONAL_HEADER64 *Opt = Headers.Optional;
    if (PeDirectory(&Headers, PE_DIRECTORY_IMPORT)->Size != 0)
        return STATUS_DLL_NOT_FOUND;
    if (PeDirectory(&Headers, PE_DIRECTORY_TLS)->Size != 0)
        return STATUS_NOT_SUPPORTED;
    bool Relocatable = (Opt->DllCharacteristics & PE_DLLCHAR_DYNAMIC_BASE) &&
                       !(Headers.File->Characteristics & PE_FILE_RELOCS_STRIPPED);
    if ((Flags & PS_CREATE_FORCE_RELOCATION) && !Relocatable)
        return STATUS_CONFLICTING_ADDRESSES;

    /* Assemble the image in a kernel buffer. */
    uint64_t ImageSize = HZ_ALIGN_UP((uint64_t)Opt->SizeOfImage, PAGE_SIZE);
    uint8_t *Image = ExAllocatePoolZero(NonPagedPool, ImageSize, PSP_IMAGE_TAG);
    if (!Image)
        return STATUS_NO_MEMORY;
    memcpy(Image, File, Opt->SizeOfHeaders);
    for (uint32_t i = 0; i < Headers.File->NumberOfSections; i++) {
        const PE_SECTION_HEADER *S = &Headers.Sections[i];
        uint32_t Length = S->SizeOfRawData;
        if (S->VirtualSize && S->VirtualSize < Length)
            Length = S->VirtualSize; /* raw data is padded to the file alignment */
        if (Length)
            memcpy(Image + S->VirtualAddress, (const uint8_t *)File + S->PointerToRawData, Length);
    }

    /* Reserve the image range: the preferred base unless relocation is forced or it is taken. */
    MMADDRESS_SPACE *As = Process->AddressSpace;
    void *Base = (Flags & PS_CREATE_FORCE_RELOCATION) ? NULL : (void *)Opt->ImageBase;
    Status = MmAllocateVirtualMemory(As, &Base, ImageSize, MEM_RESERVE | MEM_COMMIT | MEM_IMAGE, MM_PROT_READWRITE);
    if (!HZ_SUCCESS(Status) && Base && Relocatable) {
        Base = NULL;
        Status = MmAllocateVirtualMemory(As, &Base, ImageSize, MEM_RESERVE | MEM_COMMIT | MEM_IMAGE, MM_PROT_READWRITE);
    }
    if (!HZ_SUCCESS(Status)) {
        ExFreePoolWithTag(Image, PSP_IMAGE_TAG);
        return Status;
    }

    int64_t Delta = (int64_t)((uint64_t)Base - Opt->ImageBase);
    if (Delta != 0)
        Status = PspApplyRelocations(Image, ImageSize, &Headers, Delta);

    if (HZ_SUCCESS(Status)) {
        MMADDRESS_SPACE *Previous = PsAttachProcess(Process);
        Status = MmCopyToUser(Base, Image, ImageSize);
        if (HZ_SUCCESS(Status))
            Status = MmProtectVirtualMemory(As, Base, Opt->SizeOfHeaders, MM_PROT_READ, NULL);
        for (uint32_t i = 0; HZ_SUCCESS(Status) && i < Headers.File->NumberOfSections; i++) {
            const PE_SECTION_HEADER *S = &Headers.Sections[i];
            uint32_t Span = S->VirtualSize ? S->VirtualSize : S->SizeOfRawData;
            if (Span == 0)
                continue;
            uint32_t Protection = PspSectionProtection(S->Characteristics);
            if ((Protection & MM_PROT_WRITE) && (Protection & MM_PROT_EXECUTE))
                DbgPrintEx(DPFLTR_WARNING_LEVEL, "Loader: %s section %.8s is writable and executable\n",
                           Process->ImageName, S->Name);
            Status = MmProtectVirtualMemory(As, (uint8_t *)Base + S->VirtualAddress, Span, Protection, NULL);
        }
        PsDetachProcess(Previous);
    }
    ExFreePoolWithTag(Image, PSP_IMAGE_TAG);
    if (!HZ_SUCCESS(Status)) {
        MmFreeVirtualMemory(As, Base);
        return Status;
    }

    uint64_t Stack = Opt->SizeOfStackReserve ? Opt->SizeOfStackReserve : PSP_DEFAULT_STACK_RESERVE;
    Stack = HZ_ALIGN_UP(HZ_MIN(HZ_MAX(Stack, PSP_MIN_STACK_RESERVE), PSP_MAX_STACK_RESERVE), PAGE_SIZE);
    Process->ImageBase = (uint64_t)Base;
    Process->ImageSize = ImageSize;
    Process->EntryPoint = (uint64_t)Base + Opt->AddressOfEntryPoint;
    Process->StackReserve = Stack;
    DbgPrintEx(DPFLTR_TRACE_LEVEL, "Loader: %s mapped at 0x%llx (%llu KiB, %u sections, entry 0x%llx)%s\n",
               Process->ImageName, (unsigned long long)Base, (unsigned long long)(ImageSize / 1024),
               Headers.File->NumberOfSections, (unsigned long long)Process->EntryPoint,
               Delta ? ", relocated" : "");
    return STATUS_SUCCESS;
}
