/*
 * Nexora OS - mm/section.c
 * Sections: shared memory objects. A section owns resident, zeroed physical pages for its whole
 * life (there is no paging); views map those pages into user address spaces (VADs of type
 * MI_VAD_SECTION, mm/vad.c) and at most once into system space. Every view holds a reference to
 * the section, so the pages outlive the last view and the last handle. win32k keeps window
 * surfaces in sections mapped into both the owning process and system space.
 */
#include "mi.h"
#include "ob.h"
#include "ps.h"

#define MI_TAG_SECTION HZ_POOL_TAG('M', 'm', 'S', 'c')
#define MI_MAX_SECTION_BYTES (256ull * 1024 * 1024)

OBJECT_TYPE *MmSectionObjectType;

void MiReferenceSection(MM_SECTION *Section)
{
    ObReferenceObject(Section);
}

void MiDereferenceSection(MM_SECTION *Section)
{
    ObDereferenceObject(Section);
}

static void MiDeleteSection(void *Object)
{
    MM_SECTION *Section = Object;
    if (Section->SystemView) {
        uint64_t Va = (uint64_t)Section->SystemView;
        KIRQL OldIrql;
        KeAcquireSpinLock(&MmKernelAddressSpace.Lock, &OldIrql);
        for (uint64_t i = 0; i < Section->PageCount; i++)
            MiUnmapPage(&MmKernelAddressSpace, Va + (i << PAGE_SHIFT));
        KeReleaseSpinLock(&MmKernelAddressSpace.Lock, OldIrql);
        MiFreeSystemVa(Va, Section->PageCount);
    }
    if (Section->Pages) {
        for (uint64_t i = 0; i < Section->PageCount; i++) {
            if (Section->Pages[i] != MM_INVALID_PFN)
                MmFreePhysicalPage(Section->Pages[i]);
        }
        ExFreePoolWithTag(Section->Pages, MI_TAG_SECTION);
    }
}

void MmInitializeSections(void)
{
    OBJECT_TYPE_INITIALIZER Init = {0};
    Init.PoolTag = MI_TAG_SECTION;
    Init.ValidAccessMask = SECTION_ALL_ACCESS;
    Init.DeleteProcedure = MiDeleteSection;
    if (!HZ_SUCCESS(ObCreateObjectType("Section", &Init, &MmSectionObjectType)))
        KeBugCheckEx(PROCESS_INITIALIZATION_FAILED, PS_INIT_FAILURE_OBJECT_TYPES, 0x40, 0, 0);
}

STATUS MmCreateSection(uint64_t Size, MM_SECTION **Out)
{
    if (Size == 0 || Size > MI_MAX_SECTION_BYTES)
        return STATUS_INVALID_PARAMETER;
    uint64_t Pages = BYTES_TO_PAGES(Size);
    if (Pages + 1024 > MmQueryAvailablePages())
        return STATUS_INSUFFICIENT_RESOURCES; /* keep a reserve for the rest of the system */
    MM_SECTION *Section;
    STATUS Status = ObCreateObject(MmSectionObjectType, sizeof(MM_SECTION), NULL, (void **)&Section);
    if (!HZ_SUCCESS(Status))
        return Status;
    Section->Size = Size;
    Section->Pages = ExAllocatePoolWithTag(NonPagedPool, Pages * sizeof(PFN_NUMBER), MI_TAG_SECTION);
    if (!Section->Pages) {
        ObDereferenceObject(Section);
        return STATUS_NO_MEMORY;
    }
    for (uint64_t i = 0; i < Pages; i++)
        Section->Pages[i] = MM_INVALID_PFN;
    Section->PageCount = Pages;
    for (uint64_t i = 0; i < Pages; i++) {
        Section->Pages[i] = MmAllocatePhysicalPage(true);
        if (Section->Pages[i] == MM_INVALID_PFN) {
            ObDereferenceObject(Section); /* frees what was allocated */
            return STATUS_NO_MEMORY;
        }
    }
    *Out = Section;
    return STATUS_SUCCESS;
}

uint64_t MmGetSectionSize(MM_SECTION *Section)
{
    return Section->PageCount << PAGE_SHIFT;
}

STATUS MmMapViewInSystemSpace(MM_SECTION *Section, void **Base)
{
    if (!Section->SystemView) {
        uint64_t Va = MiAllocateSystemVa(Section->PageCount);
        if (!Va)
            return STATUS_NO_MEMORY;
        uint64_t Flags = MiProtectionToPte(MM_PROT_READWRITE, false);
        KIRQL OldIrql;
        KeAcquireSpinLock(&MmKernelAddressSpace.Lock, &OldIrql);
        for (uint64_t i = 0; i < Section->PageCount; i++)
            MiMapPage(&MmKernelAddressSpace, Va + (i << PAGE_SHIFT), Section->Pages[i], Flags);
        KeReleaseSpinLock(&MmKernelAddressSpace.Lock, OldIrql);
        Section->SystemView = (void *)Va;
    }
    *Base = Section->SystemView;
    return STATUS_SUCCESS;
}

/* ---- System services ------------------------------------------------------------------------- */

/* CreateSection(HANDLE *Section, uint64_t Size) */
STATUS NtCreateSection(const uint64_t *Args)
{
    HANDLE Handle = NULL;
    STATUS Status = MmCopyToUser((void *)Args[0], &Handle, sizeof(Handle)); /* probe */
    if (!HZ_SUCCESS(Status))
        return Status;
    MM_SECTION *Section;
    Status = MmCreateSection(Args[1], &Section);
    if (!HZ_SUCCESS(Status))
        return Status;
    Status = ObOpenObjectByPointer(Section, SECTION_ALL_ACCESS, NULL, &Handle);
    ObDereferenceObject(Section);
    if (!HZ_SUCCESS(Status))
        return Status;
    Status = MmCopyToUser((void *)Args[0], &Handle, sizeof(Handle));
    if (!HZ_SUCCESS(Status))
        ObCloseHandle(Handle, KernelMode);
    return Status;
}

/* MapViewOfSection(HANDLE Section, HANDLE Process, void **Base, uint64_t *ViewSize, uint32_t PageProtection) */
STATUS NtMapViewOfSection(const uint64_t *Args)
{
    uint32_t PageProtection = (uint32_t)Args[4];
    uint32_t Protection;
    if ((PageProtection != PAGE_READONLY && PageProtection != PAGE_READWRITE) ||
        !MmPageFlagsToProtection(PageProtection, &Protection))
        return STATUS_INVALID_PAGE_PROTECTION;
    void *Base;
    uint64_t ViewSize = 0;
    STATUS Status = MmCopyFromUser(&Base, (const void *)Args[2], sizeof(Base));
    if (HZ_SUCCESS(Status) && Args[3])
        Status = MmCopyToUser((void *)Args[3], &ViewSize, sizeof(ViewSize)); /* probe */
    if (!HZ_SUCCESS(Status))
        return Status;
    if ((uint64_t)Base >= MM_USER_TOP)
        return STATUS_INVALID_PARAMETER;
    if ((HANDLE)Args[1] != HZ_CURRENT_PROCESS)
        return STATUS_NOT_SUPPORTED; /* views in other processes need an attach */
    MM_SECTION *Section;
    Status = ObReferenceObjectByHandle((HANDLE)Args[0], MmSectionObjectType,
                                       PageProtection == PAGE_READWRITE ? SECTION_MAP_WRITE | SECTION_MAP_READ
                                                                        : SECTION_MAP_READ,
                                       UserMode, (void **)&Section);
    if (!HZ_SUCCESS(Status))
        return Status;
    MMADDRESS_SPACE *As = PsGetCurrentProcess()->AddressSpace;
    Status = MmMapViewOfSection(Section, As, &Base, Protection);
    ViewSize = Section->PageCount << PAGE_SHIFT;
    ObDereferenceObject(Section);
    if (!HZ_SUCCESS(Status))
        return Status;
    Status = MmCopyToUser((void *)Args[2], &Base, sizeof(Base));
    if (HZ_SUCCESS(Status) && Args[3])
        Status = MmCopyToUser((void *)Args[3], &ViewSize, sizeof(ViewSize));
    if (!HZ_SUCCESS(Status))
        MmUnmapViewOfSection(As, Base);
    return Status;
}

/* UnmapViewOfSection(HANDLE Process, void *Base) */
STATUS NtUnmapViewOfSection(const uint64_t *Args)
{
    if ((HANDLE)Args[0] != HZ_CURRENT_PROCESS)
        return STATUS_NOT_SUPPORTED;
    if (Args[1] >= MM_USER_TOP)
        return STATUS_INVALID_PARAMETER;
    return MmUnmapViewOfSection(PsGetCurrentProcess()->AddressSpace, (void *)Args[1]);
}
