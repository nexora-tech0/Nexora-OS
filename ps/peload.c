/*
 * Nexora OS - ps/peload.c
 * PE32+ image loader for user processes.
 *
 * The image is assembled in a kernel buffer (headers and sections copied to their virtual
 * addresses, base relocations applied), copied into a MEM_IMAGE region of the new process in
 * one go, and then each section gets its final protection. Writable-executable sections are
 * rejected. Imported Windows symbols are resolved by wincompat.c; unsupported imports fail closed.
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
    bool EntryPointExecutable = false;
    for (uint32_t i = 0; i < Headers.File->NumberOfSections; i++) {
        const PE_SECTION_HEADER *Section = &Headers.Sections[i];
        uint32_t Span = Section->VirtualSize ? Section->VirtualSize : Section->SizeOfRawData;
        if ((Section->Characteristics & (PE_SCN_MEM_WRITE | PE_SCN_MEM_EXECUTE)) ==
            (PE_SCN_MEM_WRITE | PE_SCN_MEM_EXECUTE))
            return STATUS_INVALID_IMAGE_FORMAT;
        if ((Section->Characteristics & PE_SCN_MEM_EXECUTE) &&
            Opt->AddressOfEntryPoint >= Section->VirtualAddress &&
            (uint64_t)Opt->AddressOfEntryPoint < (uint64_t)Section->VirtualAddress + Span)
            EntryPointExecutable = true;
    }
    if (!EntryPointExecutable)
        return STATUS_INVALID_IMAGE_FORMAT;
    if (Headers.File->Characteristics & PE_FILE_DLL)
        return STATUS_NOT_SUPPORTED;
    if (Opt->Subsystem != PE_SUBSYSTEM_WINDOWS_CUI && Opt->Subsystem != PE_SUBSYSTEM_WINDOWS_GUI) {
        DbgPrintEx(DPFLTR_WARNING_LEVEL, "Loader: %s uses unsupported PE subsystem %u\n",
                   Process->ImageName, Opt->Subsystem);
        return STATUS_NOT_SUPPORTED;
    }
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

    /* TLS directory fields are image VAs. Relocations are applied below, so retain the
       corresponding RVAs for per-thread TLS construction and callback dispatch. */
    const PE_DATA_DIRECTORY *TlsDirectory = PeDirectory(&Headers, PE_DIRECTORY_TLS);
    PE_TLS_DIRECTORY64 Tls = {0};
    uint32_t TlsRva = 0, TlsSize = 0, CallbackCount = 0;
    uint32_t CallbackRvas[64];
    if (TlsDirectory->Size) {
        if (TlsDirectory->Size < sizeof(Tls) ||
            (uint64_t)TlsDirectory->VirtualAddress + sizeof(Tls) > ImageSize)
            return STATUS_INVALID_IMAGE_FORMAT;
        TlsRva = TlsDirectory->VirtualAddress;
        memcpy(&Tls, Image + TlsRva, sizeof(Tls));
        if (Tls.EndAddressOfRawData < Tls.StartAddressOfRawData ||
            Tls.StartAddressOfRawData < Opt->ImageBase || Tls.EndAddressOfRawData < Opt->ImageBase ||
            Tls.AddressOfIndex < Opt->ImageBase || Tls.AddressOfIndex - Opt->ImageBase + sizeof(uint32_t) > ImageSize)
            return STATUS_INVALID_IMAGE_FORMAT;
        uint64_t StartRva = Tls.StartAddressOfRawData - Opt->ImageBase;
        uint64_t EndRva = Tls.EndAddressOfRawData - Opt->ImageBase;
        if (EndRva > ImageSize || EndRva - StartRva > UINT32_MAX || Tls.SizeOfZeroFill > (16u * 1024u * 1024u))
            return STATUS_INVALID_IMAGE_FORMAT;
        TlsRva = (uint32_t)StartRva;
        TlsSize = (uint32_t)(EndRva - StartRva);
        if (Tls.AddressOfCallBacks) {
            if (Tls.AddressOfCallBacks < Opt->ImageBase ||
                Tls.AddressOfCallBacks - Opt->ImageBase >= ImageSize)
                return STATUS_INVALID_IMAGE_FORMAT;
            uint64_t ArrayRva = Tls.AddressOfCallBacks - Opt->ImageBase;
            bool Terminated = false;
            for (uint32_t i = 0; i < 64u; i++) {
                if (ArrayRva + (uint64_t)(i + 1) * sizeof(uint64_t) > ImageSize)
                    return STATUS_INVALID_IMAGE_FORMAT;
                uint64_t CallbackVa;
                memcpy(&CallbackVa, Image + ArrayRva + (uint64_t)i * sizeof(uint64_t), sizeof(CallbackVa));
                if (!CallbackVa) { Terminated = true; break; }
                if (CallbackVa < Opt->ImageBase || CallbackVa - Opt->ImageBase >= ImageSize)
                    return STATUS_INVALID_IMAGE_FORMAT;
                CallbackRvas[CallbackCount++] = (uint32_t)(CallbackVa - Opt->ImageBase);
            }
            if (!Terminated)
                return STATUS_INVALID_IMAGE_FORMAT;
        }
        uint32_t TlsIndex = 0;
        memcpy(Image + (Tls.AddressOfIndex - Opt->ImageBase), &TlsIndex, sizeof(TlsIndex));
    }

    MMADDRESS_SPACE *As = Process->AddressSpace;
    /* Reserve the image range: the preferred base unless relocation is forced or it is taken. */
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

    uint64_t ThunkBase = 0;
    uint8_t *ThunkPage = ExAllocatePoolZero(NonPagedPool, PSP_WINCOMPAT_THUNK_PAGE_SIZE, PSP_IMAGE_TAG);
    if (!ThunkPage) {
        ExFreePoolWithTag(Image, PSP_IMAGE_TAG);
        MmFreeVirtualMemory(As, Base);
        return STATUS_NO_MEMORY;
    }
    int64_t Delta = (int64_t)((uint64_t)Base - Opt->ImageBase);
    if (Delta != 0)
        Status = PspApplyRelocations(Image, ImageSize, &Headers, Delta);
    /* Resolve imports after rebasing so the loader cannot relocate a patched IAT thunk. */
    if (HZ_SUCCESS(Status))
        Status = PspResolveWindowsImports(Process, Image, ImageSize, &Headers, &ThunkBase, ThunkPage);
    if (HZ_SUCCESS(Status) && CallbackCount) {
        /* The import thunk page reserves 12 KiB for 128 API entries. Emit a small ABI
           trampoline in the remaining 4 KiB to run TLS process-attach callbacks before
           entering the PE image. r12 preserves the original PEB argument across callbacks. */
        uint32_t O = 128u * 96u;
#define EMIT8(v) do { ThunkPage[O++] = (uint8_t)(v); } while (0)
#define EMIT64(v) do { uint64_t _x = (uint64_t)(v); memcpy(ThunkPage + O, &_x, 8); O += 8; } while (0)
        if (!ThunkBase || O + CallbackCount * 32u + 32u >= PSP_WINCOMPAT_THUNK_PAGE_SIZE)
            Status = STATUS_NOT_SUPPORTED;
        else {
            EMIT8(0x49); EMIT8(0x89); EMIT8(0xCC); /* mov r12, rcx */
            EMIT8(0x48); EMIT8(0x83); EMIT8(0xEC); EMIT8(0x28); /* home space + aligned calls */
            for (uint32_t i = 0; i < CallbackCount; i++) {
                EMIT8(0x48); EMIT8(0xB9); EMIT64((uint64_t)Base + CallbackRvas[i]); /* rcx=image */
                EMIT8(0xBA); EMIT8(1); EMIT8(0); EMIT8(0); EMIT8(0); /* edx=DLL_PROCESS_ATTACH */
                EMIT8(0x45); EMIT8(0x31); EMIT8(0xC0); /* xor r8d,r8d */
                EMIT8(0x48); EMIT8(0xB8); EMIT64((uint64_t)Base + CallbackRvas[i]);
                EMIT8(0xFF); EMIT8(0xD0); /* call rax */
            }
            EMIT8(0x4C); EMIT8(0x89); EMIT8(0xE1); /* mov rcx,r12 */
            EMIT8(0x48); EMIT8(0xB8); EMIT64((uint64_t)Base + Opt->AddressOfEntryPoint);
            EMIT8(0xFF); EMIT8(0xD0); EMIT8(0xCC); /* call entry; int3 if it returns */
            Process->TlsStartupThunk = ThunkBase + 128u * 96u;
        }
#undef EMIT8
#undef EMIT64
    }
    if (!HZ_SUCCESS(Status)) {
        if (ThunkBase)
            MmFreeVirtualMemory(As, (void *)ThunkBase);
        ExFreePoolWithTag(ThunkPage, PSP_IMAGE_TAG);
        MmFreeVirtualMemory(As, Base);
        ExFreePoolWithTag(Image, PSP_IMAGE_TAG);
        return Status;
    }

    if (HZ_SUCCESS(Status)) {
        MMADDRESS_SPACE *Previous = PsAttachProcess(Process);
        Status = MmCopyToUser(Base, Image, ImageSize);
        if (HZ_SUCCESS(Status) && ThunkBase)
            Status = MmCopyToUser((void *)ThunkBase, ThunkPage, PSP_WINCOMPAT_THUNK_PAGE_SIZE);
        if (HZ_SUCCESS(Status))
            Status = MmProtectVirtualMemory(As, Base, Opt->SizeOfHeaders, MM_PROT_READ, NULL);
        for (uint32_t i = 0; HZ_SUCCESS(Status) && i < Headers.File->NumberOfSections; i++) {
            const PE_SECTION_HEADER *S = &Headers.Sections[i];
            uint32_t Span = S->VirtualSize ? S->VirtualSize : S->SizeOfRawData;
            if (Span == 0)
                continue;
            uint32_t Protection = PspSectionProtection(S->Characteristics);
            Status = MmProtectVirtualMemory(As, (uint8_t *)Base + S->VirtualAddress, Span, Protection, NULL);
        }
        if (HZ_SUCCESS(Status) && ThunkBase)
            Status = MmProtectVirtualMemory(As, (void *)ThunkBase, PSP_WINCOMPAT_THUNK_PAGE_SIZE,
                                            MM_PROT_READ | MM_PROT_EXECUTE, NULL);
        PsDetachProcess(Previous);
    }
    ExFreePoolWithTag(Image, PSP_IMAGE_TAG);
    ExFreePoolWithTag(ThunkPage, PSP_IMAGE_TAG);
    if (!HZ_SUCCESS(Status)) {
        MmFreeVirtualMemory(As, Base);
        if (ThunkBase)
            MmFreeVirtualMemory(As, (void *)ThunkBase);
        return Status;
    }

    uint64_t Stack = Opt->SizeOfStackReserve ? Opt->SizeOfStackReserve : PSP_DEFAULT_STACK_RESERVE;
    Stack = HZ_ALIGN_UP(HZ_MIN(HZ_MAX(Stack, PSP_MIN_STACK_RESERVE), PSP_MAX_STACK_RESERVE), PAGE_SIZE);
    Process->ImageBase = (uint64_t)Base;
    Process->ImageSize = ImageSize;
    Process->EntryPoint = (uint64_t)Base + Opt->AddressOfEntryPoint;
    Process->HasStaticTls = TlsDirectory->Size != 0;
    Process->TlsTemplateRva = TlsRva;
    Process->TlsTemplateSize = TlsSize;
    Process->TlsZeroFillSize = Tls.SizeOfZeroFill;
    Process->TlsCallbackCount = CallbackCount;
    for (uint32_t i = 0; i < CallbackCount; i++)
        Process->TlsCallbacks[i] = (uint64_t)Base + CallbackRvas[i];
    Process->StackReserve = Stack;
    Status = PspRegisterWinModule(Process, Process->ImageName, NULL,
                                  Process->ImageBase, Process->ImageSize,
                                  PSP_WIN_MODULE_MAIN_IMAGE);
    if (!HZ_SUCCESS(Status)) {
        MmFreeVirtualMemory(As, Base);
        if (ThunkBase)
            MmFreeVirtualMemory(As, (void *)ThunkBase);
        Process->ImageBase = 0;
        Process->ImageSize = 0;
        Process->EntryPoint = 0;
        return Status;
    }
    DbgPrintEx(DPFLTR_TRACE_LEVEL, "Loader: %s mapped at 0x%llx (%llu KiB, %u sections, entry 0x%llx)%s\n",
               Process->ImageName, (unsigned long long)Base, (unsigned long long)(ImageSize / 1024),
               Headers.File->NumberOfSections, (unsigned long long)Process->EntryPoint,
               Delta ? ", relocated" : "");
    return STATUS_SUCCESS;
}
